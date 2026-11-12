/*
 * §58 throwaway: read-only unified-log stream for N seconds via the
 * os_activity_stream SPI (LoggingSupport, struct layout from LLDB's
 * debugserver ActivityStreamSPI.h, fetched at build time; not committed).
 * Prints only entries whose process image path or message matches argv[2]
 * (substring list separated by '|'). Does not change log privacy settings;
 * private arguments stay redacted.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dispatch/dispatch.h>
#include "ActivityStreamSPI.h"

static char *g_filters[16];
static int g_nf;

static int match(const char *s) {
    if (!s) return 0;
    for (int i = 0; i < g_nf; i++)
        if (strstr(s, g_filters[i])) return 1;
    return 0;
}

int main(int argc, char **argv) {
    int secs = argc > 1 ? atoi(argv[1]) : 10;
    char *f = strdup(argc > 2 ? argv[2] : "xpcproxy|launchd|appsvc");
    for (char *t = strtok(f, "|"); t && g_nf < 16; t = strtok(NULL, "|")) g_filters[g_nf++] = t;
    dlopen("/System/Library/PrivateFrameworks/LoggingSupport.framework/LoggingSupport", RTLD_NOW);
    os_activity_stream_for_pid_t sfp = dlsym(RTLD_DEFAULT, "os_activity_stream_for_pid");
    os_activity_stream_resume_t res = dlsym(RTLD_DEFAULT, "os_activity_stream_resume");
    os_log_copy_formatted_message_t fmt = dlsym(RTLD_DEFAULT, "os_log_copy_formatted_message");
    os_activity_stream_set_event_handler_t seh = dlsym(RTLD_DEFAULT, "os_activity_stream_set_event_handler");
    if (!sfp || !res || !fmt) { fprintf(stderr, "SPI missing %p %p %p\n", sfp, res, fmt); return 2; }
    os_activity_stream_t st = sfp(-1, OS_ACTIVITY_STREAM_INFO | OS_ACTIVITY_STREAM_DEBUG,
        ^bool(os_activity_stream_entry_t e, int err) {
            if (err || !e) return true;
            if (e->type != OS_ACTIVITY_STREAM_TYPE_LOG_MESSAGE &&
                e->type != OS_ACTIVITY_STREAM_TYPE_LEGACY_LOG_MESSAGE) return true;
            char *msg = fmt(&e->log_message);
            if (match(e->proc_imagepath) || match(msg)) {
                printf("%ld.%06d pid=%d %s [%s:%s] %s\n", (long)e->log_message.tv_gmt.tv_sec,
                       (int)e->log_message.tv_gmt.tv_usec, e->pid,
                       e->proc_imagepath ? e->proc_imagepath : "?",
                       e->log_message.subsystem ? e->log_message.subsystem : "",
                       e->log_message.category ? e->log_message.category : "",
                       msg ? msg : "(null)");
                fflush(stdout);
            }
            free(msg);
            return true;
        });
    if (!st) { fprintf(stderr, "stream NULL\n"); return 3; }
    if (seh) seh(st, ^(os_activity_stream_t s, os_activity_stream_event_t ev) {
        fprintf(stderr, "stream event %u\n", ev);
    });
    res(st);
    fprintf(stderr, "streaming %ds\n", secs);
    sleep(secs);
    return 0;
}
