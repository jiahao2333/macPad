/*
 * Throwaway macOS Application-type test service (C, no Foundation).
 * After the proxy chroot+execve, log mach_ports_lookup / XPC_FLAGS, then
 * call stock xpc_main (not a custom dispatch_main). On a bootstrap-shaped
 * dictionary, run dispatch_sync_f(main) and reply.
 * Exec environment must not set DYLD_INSERT_LIBRARIES (no live libmachook).
 */
#include "probe_log.h"
#include <CoreFoundation/CoreFoundation.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <errno.h>
#include <mach/mach.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <xpc/xpc.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <sys/ucontext.h>

#ifndef MACH_PORT_TYPE_RECEIVE
#define MACH_PORT_TYPE_RECEIVE 0x00020000
#endif

static volatile int g_accepted;
static volatile int g_sync_returned;
static volatile int g_sync_ran;
static volatile int g_gcd_async_ran;

static void logf_svc(const char *fmt, ...) {
    char buf[768];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    macws_probe_log("appsvc", buf);
}

__attribute__((constructor))
static void macws_appsvc_ctor(void) {
    macws_probe_log("appsvc", "constructor");
}

static void log_ports_and_flags(const char *tag) {
    mach_port_array_t ports = NULL;
    mach_msg_type_number_t count = 0;
    kern_return_t kr = mach_ports_lookup(mach_task_self(), &ports, &count);
    int recv_rights = 0;
    int slot2_recv = 0;
    char inv[640];
    int used = snprintf(inv, sizeof(inv), "%s ports_lookup kr=%d count=%u",
                        tag, (int)kr, (unsigned)count);
    if (kr == KERN_SUCCESS && ports) {
        for (mach_msg_type_number_t i = 0; i < count && i < 4; i++) {
            mach_port_type_t typ = 0;
            (void)mach_port_type(mach_task_self(), ports[i], &typ);
            int isrecv = (typ & MACH_PORT_TYPE_RECEIVE) != 0;
            if (isrecv) recv_rights++;
            if (i == 2 && isrecv) slot2_recv = 1;
            if (used > 0 && (size_t)used + 48 < sizeof(inv)) {
                int w = snprintf(inv + used, sizeof(inv) - (size_t)used,
                                 " [%u]=0x%x typ=0x%x%s",
                                 (unsigned)i, (unsigned)ports[i],
                                 (unsigned)typ, isrecv ? " RECV" : "");
                if (w > 0) used += w;
            }
        }
        (void)vm_deallocate(mach_task_self(), (vm_address_t)ports,
                            count * sizeof(*ports));
    }
    const char *flags = getenv("XPC_FLAGS");
    const char *svc = getenv("XPC_SERVICE_NAME");
    const char *dyld = getenv("DYLD_INSERT_LIBRARIES");
    if (used > 0 && (size_t)used + 200 < sizeof(inv))
        snprintf(inv + used, sizeof(inv) - (size_t)used,
                 " recv_rights=%d slot2_recv=%d flags=%s service=%s dyld_insert=%s",
                 recv_rights, slot2_recv,
                 flags ? flags : "(null)",
                 svc ? svc : "(null)",
                 dyld && dyld[0] ? dyld : "(unset)");
    logf_svc("%s", inv);
    if (slot2_recv)
        logf_svc("SERVICE_SLOT_SURVIVED=yes");
    else
        logf_svc("SERVICE_SLOT_SURVIVED=no");
    if (dyld && dyld[0])
        logf_svc("dyld insert is set (required to survive iOS-side chroot+exec of a macOS image)");
}

static void log_gcd_cf_state(const char *tag) {
    CFStringRef mode = CFRunLoopCopyCurrentMode(CFRunLoopGetMain());
    char modebuf[128];
    if (mode) {
        CFStringGetCString(mode, modebuf, sizeof(modebuf), kCFStringEncodingUTF8);
        CFRelease(mode);
    } else {
        snprintf(modebuf, sizeof(modebuf), "(null)");
    }
    typedef mach_port_t (*port4cf_fn)(void);
    port4cf_fn getport = (port4cf_fn)dlsym(RTLD_DEFAULT,
                                           "_dispatch_get_main_queue_port_4CF");
    typedef void (*cb4cf_fn)(void *);
    cb4cf_fn cb = (cb4cf_fn)dlsym(RTLD_DEFAULT, "_dispatch_main_queue_callback_4CF");
    mach_port_t mqport = MACH_PORT_NULL;
    if (getport)
        mqport = getport();
    logf_svc("%s cf_main_mode=%s dispatch_main_queue_port_4CF=0x%x "
             "callback_4CF=%p pthread_main=%d",
             tag, modebuf, (unsigned)mqport, (void *)cb, pthread_main_np());
    if (MACH_PORT_VALID(mqport))
        logf_svc("GCD_MAIN_CF_PORT=valid");
    else
        logf_svc("GCD_MAIN_CF_PORT=invalid");
}

static void *sampler_thread(void *unused) {
    (void)unused;
    for (int i = 0; i < 40; i++) {
        usleep(250000);
        char tag[32];
        snprintf(tag, sizeof(tag), "sample%02d", i);
        log_gcd_cf_state(tag);
        logf_svc("%s accepted=%d sync_ran=%d sync_returned=%d gcd_async_ran=%d",
                 tag, g_accepted, g_sync_ran, g_sync_returned, g_gcd_async_ran);
        if (g_sync_returned)
            break;
    }
    return NULL;
}

static void sync_fn(void *ctx) {
    (void)ctx;
    g_sync_ran = 1;
    logf_svc("dispatch_sync_f ran pthread_main=%d", pthread_main_np());
}

static void handle_peer(xpc_connection_t peer) {
    g_accepted = 1;
    logf_svc("xpc_main accepted connection=%p pthread_main=%d",
             peer, pthread_main_np());
    log_gcd_cf_state("on-accept");
    xpc_connection_set_event_handler(peer, ^(xpc_object_t event) {
        xpc_type_t typ = event ? xpc_get_type(event) : NULL;
        const char *tn = "null";
        if (typ == XPC_TYPE_DICTIONARY) tn = "dictionary";
        else if (typ == XPC_TYPE_ERROR) tn = "error";
        else if (typ == XPC_TYPE_CONNECTION) tn = "connection";
        else if (event) tn = "other";
        const char *mn = NULL;
        if (event && typ == XPC_TYPE_DICTIONARY)
            mn = xpc_dictionary_get_string(event, "message-name");
        logf_svc("xpc event type=%s message-name=%s pthread_main=%d",
                 tn, mn ? mn : "(null)", pthread_main_np());
        if (event && typ == XPC_TYPE_DICTIONARY) {
            logf_svc("dispatch_sync_f start");
            dispatch_sync_f(dispatch_get_main_queue(), NULL, sync_fn);
            g_sync_returned = 1;
            logf_svc("dispatch_sync_f returned");
            logf_svc("DISPATCH_SYNC_F_MAIN=returned");
            xpc_object_t reply = xpc_dictionary_create_reply(event);
            if (reply) {
                xpc_dictionary_set_string(reply, "message-name",
                                          "process-finished-launching");
                xpc_connection_send_message(peer, reply);
                xpc_release(reply);
                logf_svc("sent reply process-finished-launching");
            } else {
                logf_svc("xpc_dictionary_create_reply returned NULL");
            }
        }
    });
    xpc_connection_resume(peer);
}


/* §58: on SIGTRAP/SIGABRT/SIGILL, print the faulting PC and every loaded
 * image's __DATA,__crash_info message (libxpc stores its configuration-error
 * text in gCRAnnotations.message right before `brk #1`). */
static void crash_handler(int sig, siginfo_t *si, void *ucv) {
    (void)si;
    ucontext_t *uc = (ucontext_t *)ucv;
    uint64_t pc = 0;
    if (uc && uc->uc_mcontext)
        pc = (uint64_t)__darwin_arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
    Dl_info di;
    memset(&di, 0, sizeof(di));
    dladdr((void *)pc, &di);
    logf_svc("CRASH signal=%d pc=0x%llx sym=%s image=%s", sig,
             (unsigned long long)pc, di.dli_sname ? di.dli_sname : "?",
             di.dli_fname ? di.dli_fname : "?");
    uint32_t n = _dyld_image_count();
    for (uint32_t i = 0; i < n; i++) {
        const struct mach_header_64 *h =
            (const struct mach_header_64 *)_dyld_get_image_header(i);
        unsigned long sz = 0;
        uint8_t *p = h ? getsectiondata(h, "__DATA", "__crash_info", &sz) : NULL;
        if (!p || sz < 16) continue;
        uint64_t *a = (uint64_t *)p;
        if (a[1])
            logf_svc("CRASH_INFO image=%s message=%s", _dyld_get_image_name(i),
                     (const char *)a[1]);
        if (sz >= 40 && a[4])
            logf_svc("CRASH_INFO image=%s message2=%s", _dyld_get_image_name(i),
                     (const char *)a[4]);
    }
    _exit(128 + sig);
}

static void install_crash_handler(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGTRAP, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
}

static void alarm_die(int sig) {
    (void)sig;
    logf_svc("WATCHDOG timeout accepted=%d sync_ran=%d sync_returned=%d",
             g_accepted, g_sync_ran, g_sync_returned);
    _exit(4);
}

int main(int argc, const char **argv) {
    logf_svc("start uid=%d euid=%d argv0=%s",
             (int)getuid(), (int)geteuid(),
             (argv && argv[0]) ? argv[0] : "(null)");
    install_crash_handler();
    log_ports_and_flags("after-exec");
    log_gcd_cf_state("before-xpc_main");

    dispatch_async(dispatch_get_main_queue(), ^{
        g_gcd_async_ran = 1;
        logf_svc("dispatch_async(main) ran pthread_main=%d", pthread_main_np());
        logf_svc("GCD_MAIN_ASYNC=ran");
    });

    pthread_t th;
    pthread_create(&th, NULL, sampler_thread, NULL);
    pthread_detach(th);

    signal(SIGALRM, alarm_die);
    alarm(20);

    typedef void (*xpc_main_fn)(xpc_connection_handler_t);
    xpc_main_fn xm = (xpc_main_fn)dlsym(RTLD_DEFAULT, "xpc_main");
    logf_svc("dlsym xpc_main=%p; calling stock xpc_main", (void *)xm);
    if (!xm) {
        logf_svc("xpc_main missing; cannot continue");
        return 5;
    }
    xm(handle_peer);
    logf_svc("stock xpc_main returned (unexpected if it was parked)");
    return 0;
}
