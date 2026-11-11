/* §60: report whether <pid> is sandboxed and whether mach-lookup <name> passes. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
extern int sandbox_check(pid_t pid, const char *operation, int type, ...);
int main(int argc, char **argv) {
    pid_t p = argc > 1 ? atoi(argv[1]) : getpid();
    const char *n = argc > 2 ? argv[2] : "com.apple.WebKit.WebContent";
    printf("pid=%d sandboxed=%d", p, sandbox_check(p, NULL, 0));
    printf(" mach-lookup(global-name)=%d", sandbox_check(p, "mach-lookup", 2 | 0x40000000 /* no report */, n));
    printf("\n");
    return 0;
}
