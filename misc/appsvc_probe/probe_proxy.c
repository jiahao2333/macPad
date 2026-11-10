/*
 * Throwaway first image for the Application-type bring-up probe.
 * iOS host, root: allocate a RECEIVE right, mach_ports_register it into
 * SERVICE_SLOT (2), preserve NAME_SERVER / ENVIRONMENT if present,
 * bootstrap_register a unique probe name (not macosbooter.WebKit.*),
 * set XPC_FLAGS=8019, chroot(/var/mnt/rootfs), execve the macOS test
 * service. No DYLD_INSERT_LIBRARIES (do not load live libmachook).
 */
#include "probe_log.h"
#include <errno.h>
#include <stdarg.h>
#include <mach/mach.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef MACH_PORT_TYPE_RECEIVE
#define MACH_PORT_TYPE_RECEIVE 0x00020000
#endif
#ifndef MACH_PORT_TYPE_SEND
#define MACH_PORT_TYPE_SEND 0x00010000
#endif

extern mach_port_t bootstrap_port;
extern kern_return_t bootstrap_register(mach_port_t bp, const char *service_name,
                                       mach_port_t sp);

#define MACWS_ROOTFS "/var/mnt/rootfs"
#define SERVICE_PATH "/tmp/macws-appsvc-probe.xpc/macws-appsvc-prexpc"

static void logf_proxy(const char *fmt, ...) {
    char buf[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    macws_probe_log("proxy", buf);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    logf_proxy("start uid=%d euid=%d", (int)getuid(), (int)geteuid());

    mach_port_array_t old_ports = NULL;
    mach_msg_type_number_t old_count = 0;
    kern_return_t kr = mach_ports_lookup(mach_task_self(), &old_ports, &old_count);
    logf_proxy("pre-allocate ports_lookup kr=%d count=%u", (int)kr, (unsigned)old_count);
    mach_port_t slots[3] = { MACH_PORT_NULL, MACH_PORT_NULL, MACH_PORT_NULL };
    if (kr == KERN_SUCCESS && old_ports) {
        if (old_count > 0) slots[0] = old_ports[0];
        if (old_count > 1) slots[1] = old_ports[1];
        /* slot 2 will be replaced with our receive right */
    }

    mach_port_t recv = MACH_PORT_NULL;
    kr = mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &recv);
    if (kr != KERN_SUCCESS || !MACH_PORT_VALID(recv)) {
        logf_proxy("mach_port_allocate failed kr=%d", (int)kr);
        return 2;
    }
    kr = mach_port_insert_right(mach_task_self(), recv, recv,
                                MACH_MSG_TYPE_MAKE_SEND);
    logf_proxy("allocated recv=0x%x insert_send kr=%d", (unsigned)recv, (int)kr);

    slots[2] = recv;
    kr = mach_ports_register(mach_task_self(), slots, 3);
    logf_proxy("mach_ports_register kr=%d slots=[0x%x, 0x%x, 0x%x]",
               (int)kr, (unsigned)slots[0], (unsigned)slots[1], (unsigned)slots[2]);

    mach_port_array_t ports = NULL;
    mach_msg_type_number_t count = 0;
    kr = mach_ports_lookup(mach_task_self(), &ports, &count);
    int recv_rights = 0;
    char inv[512];
    int used = snprintf(inv, sizeof(inv), "post-register lookup kr=%d count=%u",
                        (int)kr, (unsigned)count);
    if (kr == KERN_SUCCESS && ports) {
        for (mach_msg_type_number_t i = 0; i < count && i < 4; i++) {
            mach_port_type_t typ = 0;
            (void)mach_port_type(mach_task_self(), ports[i], &typ);
            if (typ & MACH_PORT_TYPE_RECEIVE) recv_rights++;
            if (used > 0 && (size_t)used + 48 < sizeof(inv)) {
                int w = snprintf(inv + used, sizeof(inv) - (size_t)used,
                                 " [%u]=0x%x typ=0x%x%s",
                                 (unsigned)i, (unsigned)ports[i],
                                 (unsigned)typ,
                                 (typ & MACH_PORT_TYPE_RECEIVE) ? " RECV" : "");
                if (w > 0) used += w;
            }
        }
    }
    if (used > 0 && (size_t)used + 32 < sizeof(inv))
        snprintf(inv + used, sizeof(inv) - (size_t)used,
                 " recv_rights=%d", recv_rights);
    logf_proxy("%s", inv);

    if (old_ports)
        (void)vm_deallocate(mach_task_self(), (vm_address_t)old_ports,
                            old_count * sizeof(*old_ports));
    if (ports)
        (void)vm_deallocate(mach_task_self(), (vm_address_t)ports,
                            count * sizeof(*ports));

    char name[96];
    snprintf(name, sizeof(name), "%s.%d", MACWS_PROBE_DEFAULT_NAME, (int)getpid());
    kr = bootstrap_register(bootstrap_port, name, recv);
    logf_proxy("bootstrap_register name=%s kr=%d (not macosbooter.WebKit)",
               name, (int)kr);
    /* Host-visible name file for the iOS client. */
    int nfd = open(MACWS_PROBE_NAME_FILE, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (nfd >= 0) {
        (void)write(nfd, name, strlen(name));
        (void)write(nfd, "\n", 1);
        close(nfd);
    }

    if (chdir(MACWS_ROOTFS) != 0) {
        logf_proxy("chdir %s failed errno=%d", MACWS_ROOTFS, errno);
        return 111;
    }
    if (chroot(MACWS_ROOTFS) != 0) {
        logf_proxy("chroot %s failed errno=%d", MACWS_ROOTFS, errno);
        return 111;
    }
    if (chdir("/") != 0) {
        logf_proxy("chdir / after chroot failed errno=%d", errno);
        return 112;
    }
    logf_proxy("chroot ok, execve %s", SERVICE_PATH);

    char *args[] = { SERVICE_PATH, NULL };
    /* A macOS image execve'd after an iOS-side chroot SIGTRAPs (signal 5)
     * unless the live chroot libmachook is inserted — reproduced with
     * stock /bin/echo. This does not replace the on-disk net34 dylib.
     * macws_is_webkit_helper_process() keys only on WebContent/Networking/
     * GPU/SandboxBroker paths, so this probe still calls stock xpc_main. */
    char *envp[] = {
        "XPC_FLAGS=8019",
        "XPC_SERVICE_NAME=" MACWS_PROBE_DEFAULT_NAME,
        "PATH=/usr/bin:/bin:/usr/sbin:/sbin",
        "HOME=/Users/root",
        "TMPDIR=/tmp",
        "GIGACAGE_ENABLED=0",
        "MallocNanoZone=0",
        "MACWS_CHROOT_HOST_ROOT=/var/mnt/rootfs",
        "DYLD_INSERT_LIBRARIES=/usr/local/lib/libmachook.dylib",
        NULL
    };
    execve(SERVICE_PATH, args, envp);
    logf_proxy("execve failed errno=%d", errno);
    return 113;
}
