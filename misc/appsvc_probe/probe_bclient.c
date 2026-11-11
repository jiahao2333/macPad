/*
 * §58 throwaway iOS client (root, no chroot): xpc_add_bundle() the host-path
 * probe bundle into this process's launchd PID domain, then
 * xpc_connection_create(com.macwsguide.probe.appsvcb) and send one message
 * with reply. iOS launchd spawns the bundle through iOS xpcproxy (a real
 * launchd-managed Application-type job). Probe names only.
 */
#include "probe_log.h"
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xpc/xpc.h>

#define BUNDLE "/var/mobile/appsvc-probe/b/macws-appsvcb.xpc"
#define NAME "com.macwsguide.probe.appsvcb"

static void L(const char *fmt, ...) {
    char buf[900];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
#ifdef MACWS_MAC_CLIENT
    macws_probe_log("mclient", buf);
#else
    macws_probe_log("bclient", buf);
#endif
}

static void lc(const char *what) {
    /* rootless: no /bin/sh for popen; the runner prints launchctl state
     * during these pauses. */
    L("pause 3s for launchctl print (%s)", what);
    sleep(3);
}

typedef void (*add_bundle_fn)(const char *, int);
typedef xpc_connection_t (*create_fn)(const char *, dispatch_queue_t);

int main(int argc, char **argv) {
    const char *bundle = argc > 1 ? argv[1] : BUNDLE;
    L("start pid=%d uid=%d", getpid(), getuid());
    add_bundle_fn ab = (add_bundle_fn)dlsym(RTLD_DEFAULT, "xpc_add_bundle");
    create_fn cr = (create_fn)dlsym(RTLD_DEFAULT, "xpc_connection_create");
    L("dlsym xpc_add_bundle=%p xpc_connection_create=%p", (void *)ab, (void *)cr);
    if (!ab || !cr) return 2;
    ab(bundle, 2);
    L("xpc_add_bundle(%s, 2) done", bundle);
    lc(NAME);
    dispatch_queue_t q = dispatch_queue_create("probe.bclient", NULL);
    xpc_connection_t c = cr(NAME, q);
    if (!c) { L("xpc_connection_create NULL"); return 3; }
    xpc_connection_set_event_handler(c, ^(xpc_object_t ev) {
        const char *d = (xpc_get_type(ev) == XPC_TYPE_ERROR)
            ? xpc_dictionary_get_string(ev, XPC_ERROR_KEY_DESCRIPTION) : "non-error";
        L("conn event %s", d ? d : "?");
    });
    xpc_connection_resume(c);
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    __block int ok = 0;
    xpc_object_t m = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_string(m, "message-name", "bootstrap");
    L("send_message_with_reply to %s", NAME);
    xpc_connection_send_message_with_reply(c, m, q, ^(xpc_object_t r) {
        if (xpc_get_type(r) == XPC_TYPE_DICTIONARY) {
            const char *mn = xpc_dictionary_get_string(r, "message-name");
            L("REPLY dictionary message-name=%s", mn ? mn : "(null)");
            ok = mn && !strcmp(mn, "process-finished-launching");
        } else {
            const char *d = xpc_dictionary_get_string(r, XPC_ERROR_KEY_DESCRIPTION);
            L("REPLY error %s", d ? d : "?");
        }
        dispatch_semaphore_signal(sem);
    });
    xpc_release(m);
    long w = dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 15LL * NSEC_PER_SEC));
    L("done wait_timeout=%d ok=%d", w != 0, ok);
    lc(NAME);
    xpc_connection_cancel(c);
    usleep(200000);
    return ok ? 0 : 1;
}
