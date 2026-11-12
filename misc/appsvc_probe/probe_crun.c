/*
 * §58 throwaway iOS launcher: chroot(/var/mnt/rootfs) + execve(argv[1..])
 * with the minimal chroot environment (live libmachook inserted, unchanged).
 * Used to start the macOS in-chroot probe client.
 */
#include "probe_log.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    /* §59: "-u501 <exe> ..." drops to mobile after chroot (mobile client). */
    int dropMobile = 0;
    if (!strcmp(argv[1], "-u501")) {
        dropMobile = 1;
        argv++;
        argc--;
        if (argc < 2) return 2;
    }
    char *envp[] = {"PATH=/usr/bin:/bin:/usr/sbin:/sbin", "HOME=/Users/root", "TMPDIR=/tmp",
                    "GIGACAGE_ENABLED=0", "MallocNanoZone=0",
                    "MACWS_CHROOT_HOST_ROOT=/var/mnt/rootfs",
                    "DYLD_INSERT_LIBRARIES=/usr/local/lib/libmachook.dylib", NULL};
    if (chdir("/var/mnt/rootfs") || chroot("/var/mnt/rootfs") || chdir("/")) return 111;
    if (dropMobile && (setgid(501) || setuid(501))) return 114;
    execve(argv[1], argv + 1, envp);
    macws_probe_log("crun", "execve failed");
    return 113;
}
