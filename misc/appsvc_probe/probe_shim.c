/*
 * §58 throwaway: iOS CFBundleExecutable of a host-path Application-type XPC
 * bundle (com.macwsguide.probe.appsvcb). iOS launchd + iOS xpcproxy start it
 * as a real launchd-managed xpcservice job. It only logs what xpcproxy gave
 * it, then chroot(/var/mnt/rootfs) + execve's the macOS test service with the
 * *incoming* XPC_* environment unchanged (no 8019 reconstitution, no
 * homemade receive right). The live libmachook is inserted (required for any
 * macOS image after an iOS-side chroot) but is not modified.
 */
#include "probe_log.h"
#include <errno.h>
#include <mach/mach.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;
#define TARGET "/tmp/macws-appsvc-probe.xpc/macws-appsvc-probe"

static void L(const char *fmt, ...) {
    char buf[900];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    macws_probe_log("shim", buf);
}

int main(int argc, char **argv) {
    (void)argc;
    L("start pid=%d ppid=%d uid=%d euid=%d argv0=%s", getpid(), getppid(), getuid(),
      geteuid(), argv[0]);
    for (char **e = environ; e && *e; e++)
        if (!strncmp(*e, "XPC_", 4) || !strncmp(*e, "__XPC", 5) || !strncmp(*e, "DYLD", 4))
            L("env %s", *e);
    /* Production shape: xpcproxy refuses a setuid CFBundleExecutable, so a
     * mobile-launched first image is unprivileged and hops to a setuid
     * "<exe>.priv" sibling (same pid) before chroot, like
     * ViewBridgeChrootProxy's MacWSWebKitExecPrivSibling. */
    if (geteuid() != 0) {
        size_t al = strlen(argv[0]);
        if (al < 5 || strcmp(argv[0] + al - 5, ".priv")) {
            static char priv[1024];
            snprintf(priv, sizeof(priv), "%s.priv", argv[0]);
            argv[0] = priv;
            L("unprivileged; execve setuid sibling %s", priv);
            execve(priv, argv, environ);
            L("priv execve failed errno=%d", errno);
            return 117;
        }
    }
    mach_port_array_t ports = NULL;
    mach_msg_type_number_t count = 0;
    kern_return_t kr = mach_ports_lookup(mach_task_self(), &ports, &count);
    char inv[400];
    int used = snprintf(inv, sizeof(inv), "ports_lookup kr=%d count=%u", kr, count);
    for (unsigned i = 0; kr == KERN_SUCCESS && i < count && i < 3; i++) {
        mach_port_type_t t = 0;
        if (MACH_PORT_VALID(ports[i])) mach_port_type(mach_task_self(), ports[i], &t);
        used += snprintf(inv + used, sizeof(inv) - used, " [%u]=0x%x typ=0x%x", i, ports[i], t);
    }
    L("%s", inv);

    /* Build the exec environment: incoming env (minus DYLD_*), plus chroot needs. */
    static char *envp[256];
    int n = 0;
    for (char **e = environ; e && *e && n < 240; e++) {
        if (!strncmp(*e, "DYLD_", 5) || !strncmp(*e, "MallocNanoZone=", 15) ||
            !strncmp(*e, "GIGACAGE_ENABLED=", 17) || !strncmp(*e, "MACWS_CHROOT_HOST_ROOT=", 23) ||
            !strncmp(*e, "HOME=", 5) || !strncmp(*e, "TMPDIR=", 7) || !strncmp(*e, "PATH=", 5))
            continue;
        envp[n++] = *e;
    }
    /* Variant switch (throwaway): mimic ViewBridgeChrootProxy's net16
     * reconstitution of XPC_FLAGS=8019 when this marker file exists. */
    if (access("/var/mobile/appsvc-probe/force8019", F_OK) == 0) {
        for (int i = 0; i < n; i++)
            if (!strncmp(envp[i], "XPC_FLAGS=", 10)) envp[i] = "XPC_FLAGS=8019";
        L("variant: XPC_FLAGS forced to 8019");
    }
    envp[n++] = "PATH=/usr/bin:/bin:/usr/sbin:/sbin";
    envp[n++] = "HOME=/Users/root";
    envp[n++] = "TMPDIR=/tmp";
    envp[n++] = "GIGACAGE_ENABLED=0";
    envp[n++] = "MallocNanoZone=0";
    envp[n++] = "MACWS_CHROOT_HOST_ROOT=/var/mnt/rootfs";
    envp[n++] = "DYLD_INSERT_LIBRARIES=/usr/local/lib/libmachook.dylib";
    envp[n] = NULL;

    if (chdir("/var/mnt/rootfs") || chroot("/var/mnt/rootfs") || chdir("/")) {
        L("chroot failed errno=%d", errno);
        return 111;
    }
    char *args[] = {TARGET, NULL};
    L("chroot ok; execve %s with incoming XPC_* unchanged", TARGET);
    execve(TARGET, args, envp);
    L("execve failed errno=%d", errno);
    return 113;
}
