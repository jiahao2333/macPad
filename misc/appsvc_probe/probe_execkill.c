/*
 * §58 throwaway: who destroys a same-task RECEIVE right across exec?
 * iOS host, root. Modes (argv[1]):
 *   nochroot-suspend : register RECV in slot 2, posix_spawn(SETEXEC|START_SUSPENDED)
 *                      an iOS image without chroot. The new image never runs
 *                      user code (task created suspended), so any death of the
 *                      right is kernel-side.
 *   chroot-suspend   : same, but chroot(/var/mnt/rootfs) first and SETEXEC the
 *                      stock macOS /bin/echo, suspended (no dyld, no libmachook,
 *                      no libxpc ever runs).
 *   nochroot-exec    : plain execve of this binary in "after" mode (iOS image,
 *                      no chroot) with XPC_FLAGS=0x8099 to show libxpc's
 *                      flags & 0x82 rewrite.
 *   watch <name> <pid> : separate observer. Holds a send right obtained by
 *                      bootstrap_look_up, arms MACH_NOTIFY_DEAD_NAME, timestamps
 *                      the transition, tries task_for_pid on the exec'd task.
 *   after            : post-exec snapshot of mach_ports_lookup + XPC_FLAGS.
 * Probe names only: com.macwsguide.probe.execkill.<pid>.
 */
#include "probe_log.h"
#include <errno.h>
#include <mach/mach.h>
#include <spawn.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#ifndef MACH_PORT_TYPE_DEAD_NAME
#define MACH_PORT_TYPE_DEAD_NAME 0x00100000
#endif
#ifndef POSIX_SPAWN_SETEXEC
#define POSIX_SPAWN_SETEXEC 0x0040
#endif
#ifndef POSIX_SPAWN_START_SUSPENDED
#define POSIX_SPAWN_START_SUSPENDED 0x0080
#endif

extern char **environ;
extern mach_port_t bootstrap_port;
extern kern_return_t bootstrap_register(mach_port_t, const char *, mach_port_t);
extern kern_return_t bootstrap_look_up(mach_port_t, const char *, mach_port_t *);

#define SELF_PATH "/var/mobile/appsvc-probe/probe_execkill"

static void L(const char *fmt, ...) {
    char buf[900];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    macws_probe_log("execkill", buf);
}

static unsigned long long now_us(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (unsigned long long)tv.tv_sec * 1000000ull + (unsigned long long)tv.tv_usec;
}

static void dump_slots(task_t task, const char *tag) {
    mach_port_array_t ports = NULL;
    mach_msg_type_number_t count = 0;
    kern_return_t kr = mach_ports_lookup(task, &ports, &count);
    char inv[600];
    int used = snprintf(inv, sizeof(inv), "%s ports_lookup kr=%d count=%u", tag, kr, count);
    if (kr == KERN_SUCCESS && ports) {
        for (unsigned i = 0; i < count && i < 3; i++) {
            mach_port_type_t typ = 0;
            kern_return_t tk = KERN_SUCCESS;
            if (task == mach_task_self())
                tk = mach_port_type(task, ports[i], &typ);
            used += snprintf(inv + used, sizeof(inv) - used, " [%u]=0x%x typ=0x%x%s%s", i,
                             ports[i], typ,
                             (typ & MACH_PORT_TYPE_RECEIVE) ? " RECV" : "",
                             ports[i] == MACH_PORT_DEAD ? " DEAD" : "");
            (void)tk;
        }
        vm_deallocate(mach_task_self(), (vm_address_t)ports, count * sizeof(*ports));
    }
    const char *f = getenv("XPC_FLAGS");
    snprintf(inv + used, sizeof(inv) - used, " XPC_FLAGS=%s", f ? f : "(null)");
    L("%s", inv);
}

static int watcher(const char *name, pid_t pid) {
    L("watch start name=%s target_pid=%d", name, pid);
    mach_port_t s = MACH_PORT_NULL;
    kern_return_t kr = KERN_FAILURE;
    for (int i = 0; i < 300 && kr != KERN_SUCCESS; i++) {
        kr = bootstrap_look_up(bootstrap_port, name, &s);
        if (kr != KERN_SUCCESS) usleep(10000);
    }
    L("watch look_up kr=%d send=0x%x", kr, s);
    if (kr != KERN_SUCCESS) return 2;
    mach_port_t np = MACH_PORT_NULL, prev = MACH_PORT_NULL;
    mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &np);
    kr = mach_port_request_notification(mach_task_self(), s, MACH_NOTIFY_DEAD_NAME, 0,
                                        np, MACH_MSG_TYPE_MAKE_SEND_ONCE, &prev);
    L("watch armed dead-name notify kr=%d", kr);
    mach_port_type_t last = 0;
    mach_port_type(mach_task_self(), s, &last);
    L("watch initial typ=0x%x t_us=%llu", last, now_us());
    unsigned long long deadline = now_us() + 9000000ull;
    int dead = 0;
    while (now_us() < deadline) {
        struct { mach_msg_header_t h; char body[256]; } m;
        kern_return_t mr = mach_msg(&m.h, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(m), np, 2,
                                    MACH_PORT_NULL);
        if (mr == MACH_MSG_SUCCESS) {
            L("watch DEAD_NAME_NOTIFICATION msgh_id=%d t_us=%llu", m.h.msgh_id, now_us());
            dead = 1;
        }
        mach_port_type_t t = 0;
        mach_port_type(mach_task_self(), s, &t);
        if (t != last) {
            L("watch typ 0x%x -> 0x%x%s t_us=%llu", last, t,
              (t & MACH_PORT_TYPE_DEAD_NAME) ? " (receive right destroyed)" : "", now_us());
            last = t;
        }
        if (dead) break;
    }
    if (!dead) L("watch no dead-name within window; final typ=0x%x", last);
    usleep(300000);
    mach_port_t task = MACH_PORT_NULL;
    kr = task_for_pid(mach_task_self(), pid, &task);
    L("watch task_for_pid(%d) kr=%d", pid, kr);
    if (kr == KERN_SUCCESS) {
        struct task_basic_info info;
        mach_msg_type_number_t c = TASK_BASIC_INFO_COUNT;
        kern_return_t ik = task_info(task, TASK_BASIC_INFO, (task_info_t)&info, &c);
        L("watch exec'd task suspend_count=%d (kr=%d)", ik == KERN_SUCCESS ? info.suspend_count : -1, ik);
        mach_port_array_t ports = NULL;
        mach_msg_type_number_t count = 0;
        kr = mach_ports_lookup(task, &ports, &count);
        if (kr == KERN_SUCCESS) {
            for (unsigned i = 0; i < count && i < 3; i++) {
                mach_port_type_t t = 0;
                kern_return_t tk = MACH_PORT_VALID(ports[i]) ? mach_port_type(mach_task_self(), ports[i], &t) : KERN_INVALID_NAME;
                L("watch exec'd task registered[%u] -> local 0x%x %s typ=0x%x tk=%d", i, ports[i],
                  ports[i] == MACH_PORT_DEAD ? "DEAD" : (ports[i] ? "send" : "null"), t, tk);
            }
        } else {
            L("watch remote mach_ports_lookup kr=%d", kr);
        }
    }
    return dead ? 0 : 3;
}

int main(int argc, char **argv) {
    const char *mode = argc > 1 ? argv[1] : "";
    if (!strcmp(mode, "watch") && argc > 3) return watcher(argv[2], (pid_t)atoi(argv[3]));
    if (!strcmp(mode, "after")) {
        L("after-exec image start pid=%d (iOS image, libSystem+libxpc init already ran)", getpid());
        dump_slots(mach_task_self(), "after-exec");
        return 0;
    }
    int do_chroot = !strcmp(mode, "chroot-suspend");
    int suspend = strstr(mode, "suspend") != NULL;
    L("run mode=%s pid=%d uid=%d", mode, getpid(), getuid());

    mach_port_array_t old = NULL;
    mach_msg_type_number_t oc = 0;
    mach_port_t slots[3] = {0, 0, 0};
    if (mach_ports_lookup(mach_task_self(), &old, &oc) == KERN_SUCCESS && old) {
        if (oc > 0) slots[0] = old[0];
        if (oc > 1) slots[1] = old[1];
    }
    mach_port_t recv = MACH_PORT_NULL;
    mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &recv);
    mach_port_insert_right(mach_task_self(), recv, recv, MACH_MSG_TYPE_MAKE_SEND);
    slots[2] = recv;
    kern_return_t kr = mach_ports_register(mach_task_self(), slots, 3);
    L("allocated recv=0x%x register kr=%d", recv, kr);
    dump_slots(mach_task_self(), "pre-exec");
    char name[96];
    snprintf(name, sizeof(name), "com.macwsguide.probe.execkill.%d", getpid());
    kr = bootstrap_register(bootstrap_port, name, recv);
    L("bootstrap_register %s kr=%d", name, kr);

    char pidbuf[16];
    snprintf(pidbuf, sizeof(pidbuf), "%d", getpid());
    char *wargv[] = {SELF_PATH, "watch", name, pidbuf, NULL};
    pid_t wpid = 0;
    int sr = posix_spawn(&wpid, SELF_PATH, NULL, NULL, wargv, environ);
    L("spawned watcher rc=%d pid=%d", sr, wpid);
    usleep(800000);

    if (do_chroot) {
        if (chdir("/var/mnt/rootfs") || chroot("/var/mnt/rootfs") || chdir("/")) {
            L("chroot failed errno=%d", errno);
            return 111;
        }
        mach_port_type_t t = 0;
        mach_port_type(mach_task_self(), recv, &t);
        L("post-chroot (no exec yet) recv typ=0x%x %s", t,
          (t & MACH_PORT_TYPE_RECEIVE) ? "RECV still alive" : "NOT RECV");
        usleep(1200000);
        mach_port_type(mach_task_self(), recv, &t);
        L("post-chroot +1.2s recv typ=0x%x", t);
    }

    const char *target = do_chroot ? "/bin/echo" : (suspend ? "/var/jb/usr/bin/true" : SELF_PATH);
    char *targv[] = {(char *)target, suspend ? "suspended-never-runs" : "after", NULL};
    char *tenv[] = {"XPC_FLAGS=0x8099", "XPC_SERVICE_NAME=com.macwsguide.probe.execkill", "PATH=/usr/bin:/bin", NULL};
    L("exec now target=%s suspend=%d t_us=%llu", target, suspend, now_us());
    if (suspend) {
        posix_spawnattr_t a;
        posix_spawnattr_init(&a);
        posix_spawnattr_setflags(&a, POSIX_SPAWN_SETEXEC | POSIX_SPAWN_START_SUSPENDED);
        int r = posix_spawn(NULL, target, NULL, &a, targv, tenv);
        L("SETEXEC returned (failure) rc=%d", r);
        return 113;
    }
    execve(target, targv, tenv);
    L("execve failed errno=%d", errno);
    return 113;
}
