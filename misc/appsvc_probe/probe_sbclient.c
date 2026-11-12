/*
 * §60 iOS client: which sandbox filter does iOS launchd evaluate for a
 * pid-domain XPC service lookup with/without an instance UUID?
 *   probe_sbclient <variant> <inst 0|1> <service-name> <overlay host path>
 * variant: nosb | none | global | xpcsvc | ext:<extension-class>
 * Registers the overlay with xpc_add_bundle(path, 2), applies
 * "(deny mach-lookup) + (allow mach-lookup <filter>)", sends one message.
 */
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <uuid/uuid.h>
#include <xpc/xpc.h>
extern void xpc_add_bundle(const char *path, int flags);
extern void xpc_connection_set_oneshot_instance(xpc_connection_t, const uuid_t);
extern int sandbox_init(const char *profile, uint64_t flags, char **errorbuf);
extern char *sandbox_extension_issue_mach(const char *ext, const char *name, uint32_t flags);
extern int64_t sandbox_extension_consume(const char *token);
extern int sandbox_check(pid_t pid, const char *operation, int type, ...);

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage\n"); return 2; }
    const char *variant = argv[1]; int inst = atoi(argv[2]);
    const char *name = argv[3], *bundle = argv[4];
    xpc_add_bundle(bundle, 2);
    char *token = NULL; const char *cls = NULL;
    if (!strncmp(variant, "ext:", 4)) {
        cls = variant + 4;
        token = sandbox_extension_issue_mach(cls, name, 0);
        printf("issue class=%s token=%s\n", cls, token ? "yes" : "NULL");
    }
    if (strcmp(variant, "nosb")) {
        char prof[1024];
        const char *allow = "";
        char buf[512];
        if (!strcmp(variant, "global")) { snprintf(buf, sizeof buf, "(allow mach-lookup (global-name \"%s\"))", name); allow = buf; }
        else if (!strcmp(variant, "xpcsvc")) { snprintf(buf, sizeof buf, "(allow mach-lookup (xpc-service-name \"%s\"))", name); allow = buf; }
        else if (cls) { snprintf(buf, sizeof buf, "(allow mach-lookup (extension \"%s\"))", cls); allow = buf; }
        snprintf(prof, sizeof prof, "(version 1)(allow default)(deny mach-lookup)%s", allow);
        char *err = NULL;
        int rc = sandbox_init(prof, 0, &err);
        printf("sandbox_init rc=%d err=%s sandboxed=%d\n", rc, err ? err : "-", sandbox_check(getpid(), NULL, 0));
        if (rc) return 4;
        if (token) printf("consume=%lld\n", (long long)sandbox_extension_consume(token));
    }
    dispatch_queue_t q = dispatch_queue_create("sbclient", NULL);
    xpc_connection_t c = xpc_connection_create(name, q);
    if (inst) { uuid_t u; uuid_generate(u); xpc_connection_set_oneshot_instance(c, u); }
    xpc_connection_set_event_handler(c, ^(xpc_object_t ev) { (void)ev; });
    xpc_connection_resume(c);
    dispatch_semaphore_t sem = dispatch_semaphore_create(0);
    __block int ok = 0;
    xpc_object_t m = xpc_dictionary_create(NULL, NULL, 0);
    xpc_dictionary_set_string(m, "message-name", "bootstrap");
    xpc_connection_send_message_with_reply(c, m, q, ^(xpc_object_t r) {
        if (xpc_get_type(r) == XPC_TYPE_DICTIONARY) {
            const char *mn = xpc_dictionary_get_string(r, "message-name");
            ok = mn && !strcmp(mn, "process-finished-launching");
        }
        dispatch_semaphore_signal(sem);
    });
    long w = dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 8LL * NSEC_PER_SEC));
    printf("RESULT variant=%s inst=%d timeout=%d ok=%d\n", variant, inst, w != 0, ok);
    xpc_connection_cancel(c);
    usleep(300000);
    return ok ? 0 : 1;
}
