/*
 * iOS-host client for the throwaway Application-type probe.
 * Looks up the unique probe name (not macosbooter.WebKit.*) and sends a
 * bootstrap-shaped XPC dictionary. Logs whether a reply arrives.
 *
 * xpc_connection_create_mach_service is unavailable in the iOS SDK
 * headers; resolve it at runtime (it exists on the jailbroken host)
 * and fall back to xpc_pipe_create_from_port if needed.
 */
#include "probe_log.h"
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xpc/xpc.h>
#include <dispatch/dispatch.h>

extern mach_port_t bootstrap_port;
extern kern_return_t bootstrap_look_up(mach_port_t bp, const char *name,
                                      mach_port_t *sp);

typedef xpc_connection_t (*create_ms_fn)(const char *name,
                                         dispatch_queue_t target,
                                         uint64_t flags);
typedef void *(*pipe_from_port_fn)(mach_port_t port, uint64_t flags);
typedef int (*pipe_routine_fn)(void *pipe, xpc_object_t request,
                               xpc_object_t *reply);

static void logf_cli(const char *fmt, ...) {
    char buf[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    macws_probe_log("client", buf);
}

static int read_name(char *out, size_t cap) {
    int fd = open(MACWS_PROBE_NAME_FILE, O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, out, cap - 1);
    close(fd);
    if (n <= 0) return -1;
    out[n] = 0;
    char *nl = strchr(out, '\n');
    if (nl) *nl = 0;
    return 0;
}

static xpc_object_t make_bootstrap_msg(void) {
    xpc_object_t msg = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_string(msg, "message-name", "bootstrap");
    xpc_dictionary_set_string(msg, "service-name", MACWS_PROBE_DEFAULT_NAME);
    return msg;
}

static int try_pipe(mach_port_t sp) {
    pipe_from_port_fn mk = (pipe_from_port_fn)dlsym(RTLD_DEFAULT,
                                                    "xpc_pipe_create_from_port");
    pipe_routine_fn routine = (pipe_routine_fn)dlsym(RTLD_DEFAULT,
                                                     "xpc_pipe_routine");
    logf_cli("pipe fallback create=%p routine=%p port=0x%x",
             (void *)mk, (void *)routine, (unsigned)sp);
    if (!mk || !routine || !MACH_PORT_VALID(sp))
        return 0;
    void *pipe = mk(sp, 0);
    if (!pipe) {
        logf_cli("xpc_pipe_create_from_port returned NULL");
        return 0;
    }
    xpc_object_t msg = make_bootstrap_msg();
    xpc_object_t reply = NULL;
    logf_cli("xpc_pipe_routine bootstrap");
    int pr = routine(pipe, msg, &reply);
    xpc_release(msg);
    const char *mn = NULL;
    const char *tn = "null";
    if (reply) {
        xpc_type_t typ = xpc_get_type(reply);
        if (typ == XPC_TYPE_DICTIONARY) {
            tn = "dictionary";
            mn = xpc_dictionary_get_string(reply, "message-name");
        } else if (typ == XPC_TYPE_ERROR) {
            tn = "error";
        } else {
            tn = "other";
        }
    }
    logf_cli("pipe routine rc=%d reply type=%s message-name=%s",
             pr, tn, mn ? mn : "(null)");
    if (reply) xpc_release(reply);
    return (pr == 0 && mn && strcmp(mn, "process-finished-launching") == 0);
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    logf_cli("start uid=%d euid=%d", (int)getuid(), (int)geteuid());

    char name[128];
    int tries = 0;
    while (read_name(name, sizeof(name)) != 0 && tries < 50) {
        usleep(100000);
        tries++;
    }
    if (tries >= 50 && read_name(name, sizeof(name)) != 0) {
        snprintf(name, sizeof(name), "%s", MACWS_PROBE_DEFAULT_NAME);
        logf_cli("name file missing; falling back to %s", name);
    } else {
        logf_cli("service name=%s", name);
    }

    mach_port_t sp = MACH_PORT_NULL;
    kern_return_t kr = bootstrap_look_up(bootstrap_port, name, &sp);
    logf_cli("bootstrap_look_up kr=%d port=0x%x", (int)kr, (unsigned)sp);

    create_ms_fn create_ms = (create_ms_fn)dlsym(RTLD_DEFAULT,
        "xpc_connection_create_mach_service");
    logf_cli("dlsym xpc_connection_create_mach_service=%p", (void *)create_ms);

    int got_reply = 0;
    if (create_ms) {
        xpc_connection_t conn = create_ms(name, NULL, 0);
        if (!conn) {
            logf_cli("xpc_connection_create_mach_service returned NULL");
        } else {
            dispatch_semaphore_t sem = dispatch_semaphore_create(0);
            __block int local_got = 0;
            xpc_connection_set_event_handler(conn, ^(xpc_object_t ev) {
                xpc_type_t typ = ev ? xpc_get_type(ev) : NULL;
                const char *tn = "null";
                if (typ == XPC_TYPE_ERROR) tn = "error";
                else if (typ == XPC_TYPE_DICTIONARY) tn = "dictionary";
                else if (ev) tn = "other";
                const char *desc = NULL;
                if (typ == XPC_TYPE_ERROR)
                    desc = xpc_dictionary_get_string(ev, XPC_ERROR_KEY_DESCRIPTION);
                logf_cli("conn event type=%s desc=%s", tn, desc ? desc : "-");
            });
            xpc_connection_resume(conn);
            xpc_object_t msg = make_bootstrap_msg();
            logf_cli("send_message_with_reply bootstrap");
            xpc_connection_send_message_with_reply(
                conn, msg,
                dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0),
                ^(xpc_object_t reply) {
                    xpc_type_t typ = reply ? xpc_get_type(reply) : NULL;
                    const char *tn = "null";
                    if (typ == XPC_TYPE_ERROR) tn = "error";
                    else if (typ == XPC_TYPE_DICTIONARY) tn = "dictionary";
                    else if (reply) tn = "other";
                    const char *mn = NULL;
                    const char *desc = NULL;
                    if (typ == XPC_TYPE_DICTIONARY)
                        mn = xpc_dictionary_get_string(reply, "message-name");
                    if (typ == XPC_TYPE_ERROR)
                        desc = xpc_dictionary_get_string(reply,
                                                         XPC_ERROR_KEY_DESCRIPTION);
                    logf_cli("reply type=%s message-name=%s desc=%s",
                             tn, mn ? mn : "(null)", desc ? desc : "-");
                    if (typ == XPC_TYPE_DICTIONARY)
                        local_got = 1;
                    dispatch_semaphore_signal(sem);
                });
            xpc_release(msg);
            long rc = dispatch_semaphore_wait(
                sem, dispatch_time(DISPATCH_TIME_NOW, 10ull * NSEC_PER_SEC));
            if (rc != 0)
                logf_cli("reply WAIT_TIMEOUT 10s got_reply=%d", local_got);
            else
                logf_cli("reply waiter done got_reply=%d", local_got);
            got_reply = local_got;
        }
    }

    if (!got_reply && MACH_PORT_VALID(sp)) {
        logf_cli("trying xpc_pipe fallback");
        got_reply = try_pipe(sp);
    }

    if (got_reply)
        logf_cli("CLIENT_REPLY=dictionary");
    else
        logf_cli("CLIENT_REPLY=none");
    return got_reply ? 0 : 3;
}
