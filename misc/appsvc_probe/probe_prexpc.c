/*
 * libSystem-only after-exec snapshot, then POSIX_SPAWN_SETEXEC of the
 * real test service. Does not link XPC/CoreFoundation, so __libxpc_initializer
 * does not run in this image. Live libmachook is still inserted (required to
 * survive an iOS-side chroot+exec of a macOS image). Logs whether SERVICE_SLOT
 * and XPC_FLAGS survived exec *before* stock xpc_main's image is admitted,
 * then applies registered ports the way xpcproxy does (spawnattr, SETEXEC).
 */
#include "probe_log.h"
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef MACH_PORT_TYPE_RECEIVE
#define MACH_PORT_TYPE_RECEIVE 0x00020000
#endif
#ifndef MACH_PORT_TYPE_DEAD_NAME
#define MACH_PORT_TYPE_DEAD_NAME 0x00100000
#endif
#ifndef POSIX_SPAWN_SETEXEC
#define POSIX_SPAWN_SETEXEC 0x0040
#endif

extern char **environ;
extern int posix_spawnattr_set_registered_ports_np(
    posix_spawnattr_t *attr, mach_port_t portarray[], uint32_t count);

#define SERVICE_PATH "/tmp/macws-appsvc-probe.xpc/macws-appsvc-probe"

static void logf_pre(const char *fmt, ...) {
    char buf[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    macws_probe_log("prexpc", buf);
}

int main(int argc, char **argv) {
    (void)argc;
    logf_pre("start uid=%d euid=%d argv0=%s (libSystem-only; no libxpc link)",
             (int)getuid(), (int)geteuid(),
             (argv && argv[0]) ? argv[0] : "(null)");

    const char *flags = getenv("XPC_FLAGS");
    const char *svc = getenv("XPC_SERVICE_NAME");
    const char *dyld = getenv("DYLD_INSERT_LIBRARIES");
    logf_pre("environ flags=%s service=%s dyld_insert=%s",
             flags ? flags : "(null)",
             svc ? svc : "(null)",
             dyld && dyld[0] ? dyld : "(unset)");

    mach_port_array_t ports = NULL;
    mach_msg_type_number_t count = 0;
    kern_return_t kr = mach_ports_lookup(mach_task_self(), &ports, &count);
    int recv_rights = 0;
    int slot2_recv = 0;
    int slot2_dead = 0;
    char inv[640];
    int used = snprintf(inv, sizeof(inv), "after-exec ports_lookup kr=%d count=%u",
                        (int)kr, (unsigned)count);
    mach_port_t slots[3] = { MACH_PORT_NULL, MACH_PORT_NULL, MACH_PORT_NULL };
    if (kr == KERN_SUCCESS && ports) {
        for (mach_msg_type_number_t i = 0; i < count && i < 4; i++) {
            mach_port_type_t typ = 0;
            (void)mach_port_type(mach_task_self(), ports[i], &typ);
            int isrecv = (typ & MACH_PORT_TYPE_RECEIVE) != 0;
            int isdead = (typ & MACH_PORT_TYPE_DEAD_NAME) != 0;
            if (isrecv) recv_rights++;
            if (i == 2 && isrecv) slot2_recv = 1;
            if (i == 2 && isdead) slot2_dead = 1;
            if (i < 3) slots[i] = ports[i];
            if (used > 0 && (size_t)used + 56 < sizeof(inv)) {
                int w = snprintf(inv + used, sizeof(inv) - (size_t)used,
                                 " [%u]=0x%x typ=0x%x%s%s",
                                 (unsigned)i, (unsigned)ports[i],
                                 (unsigned)typ,
                                 isrecv ? " RECV" : "",
                                 isdead ? " DEAD" : "");
                if (w > 0) used += w;
            }
        }
        (void)vm_deallocate(mach_task_self(), (vm_address_t)ports,
                            count * sizeof(*ports));
    }
    logf_pre("%s recv_rights=%d", inv, recv_rights);
    if (slot2_recv)
        logf_pre("PREXPC_SERVICE_SLOT_SURVIVED=yes");
    else if (slot2_dead)
        logf_pre("PREXPC_SERVICE_SLOT_SURVIVED=dead_name");
    else
        logf_pre("PREXPC_SERVICE_SLOT_SURVIVED=no");

    /* Inherited fds (xpcproxy can pass fileports / packed fds). */
    char fds[256];
    int fu = snprintf(fds, sizeof(fds), "inherited_fds");
    for (int fd = 0; fd < 32 && fu > 0 && (size_t)fu + 8 < sizeof(fds); fd++) {
        if (fcntl(fd, F_GETFD) < 0) continue;
        int w = snprintf(fds + fu, sizeof(fds) - (size_t)fu, " %d", fd);
        if (w > 0) fu += w;
    }
    logf_pre("%s", fds);

    if (access(SERVICE_PATH, X_OK) != 0) {
        logf_pre("service %s not executable errno=%d", SERVICE_PATH, errno);
        return 2;
    }

    /* Re-apply 8019 in case something already rewrote environ, then SETEXEC
     * the xpc_main service with the current registered ports. This is the
     * same apply path xpcproxy uses after unpacking launchd's "blob"+ "ports"
     * (HookCase; xpcproxy "Insufficient spawnattr length: %zu < %lu" min
     * 0x104). We cannot transplant a real launchd blob without launchd
     * cooperation; SETEXEC + set_registered_ports_np is the public equivalent.
     */
    setenv("XPC_FLAGS", "8019", 1);
    const char *now = getenv("XPC_FLAGS");
    logf_pre("re-set XPC_FLAGS before SETEXEC now=%s slot2=0x%x",
             now ? now : "(null)", (unsigned)slots[2]);

    posix_spawnattr_t attr;
    int arc = posix_spawnattr_init(&attr);
    if (arc != 0) {
        logf_pre("posix_spawnattr_init rc=%d", arc);
        return 3;
    }
    int frc = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETEXEC);
    int prc = posix_spawnattr_set_registered_ports_np(&attr, slots, 3);
    logf_pre("setexec flags rc=%d set_registered_ports rc=%d errno=%d",
             frc, prc, errno);

    char *args[] = { SERVICE_PATH, NULL };
    pid_t child = 0;
    logf_pre("posix_spawn SETEXEC %s", SERVICE_PATH);
    int src = posix_spawn(&child, SERVICE_PATH, NULL, &attr, args, environ);
    posix_spawnattr_destroy(&attr);
    /* SETEXEC should not return on success. */
    logf_pre("posix_spawn SETEXEC returned rc=%d errno=%d child=%d",
             src, src ? errno : 0, (int)child);
    return src ? 4 : 5;
}
