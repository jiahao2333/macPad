/* Shared log helper for the throwaway Application-type bring-up probe.
 * Always write the host-visible rootfs tmp path when possible, and also
 * /tmp (same file after chroot). */
#pragma once
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

#define MACWS_PROBE_LOG_HOST "/var/mnt/rootfs/tmp/macws-appsvc-probe.log"
#define MACWS_PROBE_LOG_CHROOT "/tmp/macws-appsvc-probe.log"
#define MACWS_PROBE_NAME_FILE "/var/mnt/rootfs/tmp/macws-appsvc-probe.name"
#define MACWS_PROBE_NAME_FILE_CHROOT "/tmp/macws-appsvc-probe.name"
#define MACWS_PROBE_DEFAULT_NAME "com.macwsguide.probe.appsvc"

static void macws_probe_log(const char *tag, const char *msg) {
    char line[1024];
    struct timeval tv;
    gettimeofday(&tv, NULL);
    int n = snprintf(line, sizeof(line), "%ld.%03ld %s[%d] %s\n",
                     (long)tv.tv_sec, (long)(tv.tv_usec / 1000),
                     tag ? tag : "probe", (int)getpid(),
                     msg ? msg : "");
    if (n < 0) return;
    size_t len = (size_t)n;
    if (len >= sizeof(line)) len = sizeof(line) - 1;
    const char *paths[] = { MACWS_PROBE_LOG_HOST, MACWS_PROBE_LOG_CHROOT, NULL };
    for (int i = 0; paths[i]; i++) {
        int fd = open(paths[i], O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) continue;
        (void)write(fd, line, len);
        close(fd);
        /* After chroot, HOST path is not visible; writing both is fine. */
    }
    (void)write(STDERR_FILENO, line, len);
}
