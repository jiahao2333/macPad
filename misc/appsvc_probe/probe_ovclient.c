/*
 * §59 macOS in-chroot client for the REAL generic path: a plain
 * xpc_connection_create(<name>) with NO xpc_add_bundle. libmachook's
 * interposer must find <main bundle>/Contents/XPCServices/<name>.xpc, register
 * its generated overlay (MacWSXPCProxy) in this process's launchd domain and
 * fall through to the stock create. Sends one bootstrap-shaped message and
 * waits for "process-finished-launching". Probe names only.
 */
#include "probe_log.h"
#include <dispatch/dispatch.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <xpc/xpc.h>

static void L(const char *fmt, ...) {
    char buf[900];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    macws_probe_log("ovclient", buf);
}

int main(int argc, char **argv) {
    const char *name = argc > 1 ? argv[1] : "com.macwsguide.probe.overlay";
    L("start pid=%d uid=%d euid=%d name=%s create=%p", getpid(), getuid(),
      geteuid(), name, (void *)xpc_connection_create);
    dispatch_queue_t q = dispatch_queue_create("probe.ovclient", NULL);
    xpc_connection_t c = xpc_connection_create(name, q);
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
    L("send_message_with_reply to %s", name);
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
    sleep(2);
    xpc_connection_cancel(c);
    usleep(200000);
    return ok ? 0 : 1;
}
