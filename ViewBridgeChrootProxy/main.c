// Freestanding launch stub for macOS XPC services inside the chroot.
//
// launchd/xpcproxy start this iOS first image for an XPCService job; it only
// crosses into the chroot and execs the stock macOS executable in the same
// task (same pid). Pass every launchd-provided XPC_* entry through unchanged.
//
// Field notes §58: no receive right survives exec on this kernel (cloneproc
// gives the new image a fresh IPC space), so nothing is "carried" in
// SERVICE_SLOT and XPC_FLAGS must not be reconstituted (libxpc rewrites it
// to flags & 0x82 anyway). Ventura xpc_main gets its XPCService dictionary
// (launchd routine 718) and listener RECEIVE (bootstrap_check_in3) from iOS
// launchd *by pid* after exec, which works whenever the job is launchd-
// managed in the client's process domain (xpc_add_bundle(path, 2)).
//
// xpcproxy refuses a setuid CFBundleExecutable, so the bundle executable is
// shipped 0755; when it starts unprivileged it execs its setuid "*.priv"
// sibling (same pid) before chroot.
//
// Generic overlay mode (§59): for .../macws-xpc-overlay/<key>/<leaf>.xpc/
// MacWSXPCProxy the macOS target is read from the root-owned MacWSTarget
// symlink next to it, so one proxy serves every Application-type bundle.
// The transition below calls no libSystem APIs, although the current build
// still links libSystem (-lSystem in the Makefile and the deployed Mach-O).
// Calling this executable dependency-free would therefore be inaccurate.

#include <stddef.h>
#include <stdint.h>
#include "../include/macws_settings_paths.h"
#include "../include/macws_chroot_environment.h"
#if defined(MACWS_WEBKIT_PROXY_EXPERIMENT)
#include "../include/macws_webkit_services.h"
#endif

#define MACWS_SYS_exit    1
#define MACWS_SYS_fork    2
#define MACWS_SYS_chdir  12
#define MACWS_SYS_getuid  24
#define MACWS_SYS_geteuid 25
#define MACWS_SYS_getppid 39
#define MACWS_SYS_getgid  47
#define MACWS_SYS_getegid 43
#define MACWS_SYS_execve 59
#define MACWS_SYS_chroot 61
#define MACWS_SYS_setuid 23
#define MACWS_SYS_setgid 181
#define MACWS_SYS_poll 230

static long MacWSSyscall0(long number) {
    register long x0 __asm("x0");
    register long x16 __asm("x16") = number;
    __asm__ volatile("svc #0x80" : "=r"(x0) : "r"(x16) : "memory", "cc");
    return x0;
}

static long MacWSFork(int *isChild) {
    register long x0 __asm("x0");
    register long x1 __asm("x1");
    register long x16 __asm("x16") = MACWS_SYS_fork;
    __asm__ volatile("svc #0x80"
                     : "=r"(x0), "=r"(x1)
                     : "r"(x16)
                     : "memory", "cc");
    // Darwin's raw fork ABI returns the child PID in x0 to both tasks and
    // identifies the child with x1=1.  libc normally translates that to the
    // POSIX child return value 0; this freestanding proxy must do it itself.
    if (isChild) *isChild = x1 != 0;
    return x0;
}

#define MACWS_ROOTFS "/var/mnt/rootfs"
#define MACWS_MAX_ARGUMENTS 128
#define MACWS_MAX_ENVIRONMENT 256

static long MacWSSyscall1(long number, const void *argument0) {
    register long x0 __asm("x0") = (long)argument0;
    register long x16 __asm("x16") = number;
    __asm__ volatile("svc #0x80" : "+r"(x0) : "r"(x16) : "memory", "cc");
    return x0;
}

static long MacWSSyscall3(long number, const void *argument0,
                          const void *argument1, const void *argument2) {
    register long x0 __asm("x0") = (long)argument0;
    register long x1 __asm("x1") = (long)argument1;
    register long x2 __asm("x2") = (long)argument2;
    register long x16 __asm("x16") = number;
    __asm__ volatile("svc #0x80"
                     : "+r"(x0)
                     : "r"(x1), "r"(x2), "r"(x16)
                     : "memory", "cc");
    return x0;
}

// Unlike libc, Darwin's raw BSD syscall returns positive errno with carry
// set. Namespace setup opens a real descriptor, so preserve that distinction
// and never treat an error number as a descriptor to query/close.
static long MacWSCheckedSyscall3(long number, long a, long b, long c) {
    register long x0 __asm("x0") = a;
    register long x1 __asm("x1") = b;
    register long x2 __asm("x2") = c;
    register long x16 __asm("x16") = number;
    __asm__ volatile("svc #0x80\n\tcneg x0, x0, cs"
                     : "+r"(x0), "+r"(x1), "+r"(x2)
                     : "r"(x16) : "memory", "cc");
    return x0;
}

__attribute__((noreturn))
static void MacWSExit(long status) {
    (void)MacWSSyscall1(MACWS_SYS_exit, (const void *)status);
    __builtin_unreachable();
}

static int MacWSStringContains(const char *string, const char *needle) {
    if (!string || !needle || !*needle) return 0;
    for (const char *start = string; *start; start++) {
        const char *left = start;
        const char *right = needle;
        while (*left && *right && *left == *right) {
            left++;
            right++;
        }
        if (!*right) return 1;
    }
    return 0;
}

static int MacWSHasEnvironmentKey(const char *entry, const char *key) {
    if (!entry || !key) return 0;
    while (*key && *entry == *key) {
        entry++;
        key++;
    }
    return !*key && *entry == '=';
}

static const char *MacWSEnvironmentValue(char *envp[], const char *key) {
    for (char **entry = envp; entry && *entry; entry++) {
        if (!MacWSHasEnvironmentKey(*entry, key)) continue;
        const char *value = *entry;
        while (*value && *value != '=') value++;
        return *value == '=' ? value + 1 : (const char *)0;
    }
    return (const char *)0;
}

#if defined(MACWS_WEBKIT_PROXY_EXPERIMENT)
#define MACWS_SYS_open  5
#define MACWS_SYS_write 4
#define MACWS_SYS_close 6
#define MACWS_O_WRONLY  0x0001
#define MACWS_O_APPEND  0x0008
#define MACWS_O_CREAT   0x0200
#define MACWS_LOG_MODE  0644

static void MacWSAppendString(char *buf, size_t cap, size_t *len, const char *s) {
    if (!s) s = "(null)";
    while (*s && *len + 1 < cap) buf[(*len)++] = *s++;
}

static void MacWSAppendUnsigned(char *buf, size_t cap, size_t *len, unsigned long v) {
    char tmp[24];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10));
        v /= 10;
    }
    while (n && *len + 1 < cap) buf[(*len)++] = tmp[--n];
}

static void MacWSAppendHex(char *buf, size_t cap, size_t *len, unsigned long v) {
    char tmp[20];
    int n = 0;
    MacWSAppendString(buf, cap, len, "0x");
    if (v == 0) tmp[n++] = '0';
    while (v && n < (int)sizeof(tmp)) {
        unsigned d = (unsigned)(v & 0xf);
        tmp[n++] = (char)(d < 10 ? '0' + d : 'a' + (d - 10));
        v >>= 4;
    }
    while (n && *len + 1 < cap) buf[(*len)++] = tmp[--n];
}

static int MacWSEndsWith(const char *s, const char *suffix) {
    if (!s || !suffix) return 0;
    unsigned n = 0, m = 0;
    while (s[n]) n++;
    while (suffix[m]) m++;
    if (m > n) return 0;
    s += n - m;
    while (m) {
        if (*s != *suffix) return 0;
        s++;
        suffix++;
        m--;
    }
    return 1;
}

static int MacWSPathHasSlash(const char *s) {
    if (!s) return 0;
    while (*s) {
        if (*s == '/') return 1;
        s++;
    }
    return 0;
}

// libsystem_kernel only: inspect inherited registered ports. Does not
// unregister them and does not call libxpc.
typedef unsigned int MacWSMachPort;
typedef int MacWSKernReturn;
extern MacWSMachPort mach_task_self_;
extern MacWSKernReturn mach_ports_lookup(
    MacWSMachPort task, MacWSMachPort **ports, unsigned int *count);
extern MacWSKernReturn mach_port_type(
    MacWSMachPort task, MacWSMachPort name, unsigned int *ptype);
#define MACWS_MACH_PORT_TYPE_RECEIVE ((unsigned int)0x00020000)

static void MacWSLogWebKitProxyLine(const char *line, size_t len) {
    static const char path[] = "/var/mnt/rootfs/tmp/macws-webkit-proxy.log";
    long fd = MacWSCheckedSyscall3(
        MACWS_SYS_open, (long)path,
        MACWS_O_WRONLY | MACWS_O_CREAT | MACWS_O_APPEND, MACWS_LOG_MODE);
    if (fd < 0) return;
    (void)MacWSSyscall3(MACWS_SYS_write, (const void *)fd, line,
                        (const void *)len);
    (void)MacWSSyscall1(MACWS_SYS_close, (const void *)fd);
}

// Host-visible path so the line lands in the chroot /tmp log tree.
static void MacWSLogWebKitProxyFlags(const char *incoming, const char *outgoing,
                                     const char *program, const char *target,
                                     int uniquePeer) {
    char line[512];
    size_t len = 0;
    MacWSAppendString(line, sizeof(line), &len, "proxy XPC_FLAGS incoming=");
    MacWSAppendString(line, sizeof(line), &len,
                      incoming && *incoming ? incoming : "(unset)");
    MacWSAppendString(line, sizeof(line), &len, " outgoing=");
    MacWSAppendString(line, sizeof(line), &len, outgoing);
    MacWSAppendString(line, sizeof(line), &len, " unique_peer=");
    MacWSAppendUnsigned(line, sizeof(line), &len,
                        (unsigned long)(uniquePeer ? 1 : 0));
    MacWSAppendString(line, sizeof(line), &len, " program=");
    MacWSAppendString(line, sizeof(line), &len, program);
    MacWSAppendString(line, sizeof(line), &len, " target=");
    MacWSAppendString(line, sizeof(line), &len, target);
    if (len + 1 < sizeof(line)) line[len++] = '\n';
    MacWSLogWebKitProxyLine(line, len);
}

static void MacWSLogWebKitProxyHandshake(const char *program, int alreadyPriv,
                                         int willExecPriv) {
    char line[768];
    size_t len = 0;
    long uid = MacWSSyscall0(MACWS_SYS_getuid);
    long euid = MacWSSyscall0(MACWS_SYS_geteuid);
    long gid = MacWSSyscall0(MACWS_SYS_getgid);
    long egid = MacWSSyscall0(MACWS_SYS_getegid);
    MacWSMachPort *ports = 0;
    unsigned int count = 0;
    MacWSKernReturn kr = mach_ports_lookup(mach_task_self_, &ports, &count);
    unsigned recv = 0;
    MacWSAppendString(line, sizeof(line), &len, "proxy handshake uid=");
    MacWSAppendUnsigned(line, sizeof(line), &len, (unsigned long)uid);
    MacWSAppendString(line, sizeof(line), &len, " euid=");
    MacWSAppendUnsigned(line, sizeof(line), &len, (unsigned long)euid);
    MacWSAppendString(line, sizeof(line), &len, " gid=");
    MacWSAppendUnsigned(line, sizeof(line), &len, (unsigned long)gid);
    MacWSAppendString(line, sizeof(line), &len, " egid=");
    MacWSAppendUnsigned(line, sizeof(line), &len, (unsigned long)egid);
    MacWSAppendString(line, sizeof(line), &len, " already_priv=");
    MacWSAppendUnsigned(line, sizeof(line), &len, (unsigned long)(alreadyPriv ? 1 : 0));
    MacWSAppendString(line, sizeof(line), &len, " will_exec_priv=");
    MacWSAppendUnsigned(line, sizeof(line), &len, (unsigned long)(willExecPriv ? 1 : 0));
    MacWSAppendString(line, sizeof(line), &len, " ports_lookup kr=");
    MacWSAppendUnsigned(line, sizeof(line), &len, (unsigned long)(unsigned int)kr);
    MacWSAppendString(line, sizeof(line), &len, " count=");
    MacWSAppendUnsigned(line, sizeof(line), &len, (unsigned long)count);
    if (kr == 0 && ports) {
        unsigned i;
        for (i = 0; i < count && i < 4; i++) {
            unsigned int typ = 0;
            MacWSAppendString(line, sizeof(line), &len, " [");
            MacWSAppendUnsigned(line, sizeof(line), &len, (unsigned long)i);
            MacWSAppendString(line, sizeof(line), &len, "]=");
            MacWSAppendHex(line, sizeof(line), &len, (unsigned long)ports[i]);
            if (ports[i] &&
                mach_port_type(mach_task_self_, ports[i], &typ) == 0) {
                MacWSAppendString(line, sizeof(line), &len, " typ=");
                MacWSAppendHex(line, sizeof(line), &len, (unsigned long)typ);
                if (typ & MACWS_MACH_PORT_TYPE_RECEIVE) recv++;
            }
        }
    }
    MacWSAppendString(line, sizeof(line), &len, " recv_rights=");
    MacWSAppendUnsigned(line, sizeof(line), &len, (unsigned long)recv);
    MacWSAppendString(line, sizeof(line), &len, " program=");
    MacWSAppendString(line, sizeof(line), &len, program);
    if (len + 1 < sizeof(line)) line[len++] = '\n';
    MacWSLogWebKitProxyLine(line, len);
}

// If xpcproxy started the non-setuid stub as an unprivileged user, exec
// the 4755 sibling so chroot can proceed. argv[0] must be an absolute
// path (xpcproxy provides one). Do not loop if we are already the sibling.
extern int MacWSLibSystemExecve(const char *path, char *const argv[],
                                char *const envp[]) __asm__("_execve");

static int MacWSWebKitExecPrivSibling(const char *program, char *argv[],
                                      char *envp[]) {
    static char privPath[1024];
    size_t n = 0;
    if (!program || !MacWSPathHasSlash(program) || MacWSEndsWith(program, ".priv"))
        return 0;
    while (program[n] && n + 6 < sizeof(privPath)) {
        privPath[n] = program[n];
        n++;
    }
    privPath[n++] = '.';
    privPath[n++] = 'p';
    privPath[n++] = 'r';
    privPath[n++] = 'i';
    privPath[n++] = 'v';
    privPath[n] = 0;
    // §59: go through libSystem's execve (not the raw trap) so the
    // jailbreak's exec hook sees the setuid sibling and applies its
    // credentials; a raw SYS_execve left euid=mobile and looped. argv[0]
    // becomes the *.priv path so the second image never hops again.
    if (argv && argv[0]) argv[0] = privPath;
    (void)MacWSLibSystemExecve(privPath, argv, envp);
    return 1;
}
#endif

#if defined(MACWS_WEBKIT_PROXY_EXPERIMENT)
#define MACWS_SYS_readlink 58
#define MACWS_SYS_lstat64 340
#define MACWS_OVERLAY_MARKER "/usr/macOS/libexec/macws-xpc-overlay/"

// Darwin arm64 struct stat64: st_mode u16 @4, st_uid u32 @16.
static int MacWSLstatOwnerMode(const char *path, unsigned *uid,
                               unsigned *mode) {
    unsigned char st[160];
    for (unsigned i = 0; i < sizeof(st); i++) st[i] = 0;
    if (MacWSCheckedSyscall3(MACWS_SYS_lstat64, (long)path, (long)st, 0) != 0)
        return 0;
    *mode = (unsigned)st[4] | ((unsigned)st[5] << 8);
    *uid = (unsigned)st[16] | ((unsigned)st[17] << 8) |
           ((unsigned)st[18] << 16) | ((unsigned)st[19] << 24);
    return 1;
}

static int MacWSIsOverlayProgram(const char *program) {
    return program && MacWSStringContains(program, MACWS_OVERLAY_MARKER) &&
        (MacWSEndsWith(program, ".xpc/MacWSXPCProxy") ||
         MacWSEndsWith(program, ".xpc/MacWSXPCProxy.priv"));
}

// <overlay>/<key>/<leaf>.xpc/MacWSTarget -> stock macOS executable (chroot
// path). Only a root-owned, non-group/other-writable overlay directory and a
// root-owned link are honoured; the target must be an absolute XPCServices
// executable without "..".
static const char *MacWSOverlayTarget(const char *program) {
    static char dir[1024];
    static char link[1100];
    static char target[1024];
    if (!MacWSIsOverlayProgram(program)) return (const char *)0;
    unsigned n = 0, slash = 0;
    while (program[n] && n + 1 < sizeof(dir)) {
        dir[n] = program[n];
        if (program[n] == '/') slash = n;
        n++;
    }
    if (!slash) return (const char *)0;
    dir[slash] = 0;
    unsigned uid = 1, mode = 0;
    if (!MacWSLstatOwnerMode(dir, &uid, &mode)) return (const char *)0;
    if (uid != 0 || (mode & 0170000) != 0040000 || (mode & 0022))
        return (const char *)0;
    unsigned l = 0;
    for (unsigned i = 0; dir[i] && l + 1 < sizeof(link); i++) link[l++] = dir[i];
    static const char leaf[] = "/MacWSTarget";
    for (unsigned i = 0; leaf[i] && l + 1 < sizeof(link); i++) link[l++] = leaf[i];
    link[l] = 0;
    if (!MacWSLstatOwnerMode(link, &uid, &mode)) return (const char *)0;
    if (uid != 0 || (mode & 0170000) != 0120000) return (const char *)0;
    long got = MacWSCheckedSyscall3(MACWS_SYS_readlink, (long)link,
                                    (long)target, (long)(sizeof(target) - 1));
    if (got <= 0 || got >= (long)sizeof(target)) return (const char *)0;
    target[got] = 0;
    if (target[0] != '/' || MacWSStringContains(target, "/../") ||
        !MacWSStringContains(target, "/XPCServices/") ||
        !MacWSStringContains(target, ".xpc/"))
        return (const char *)0;
    return target;
}
#endif

static int MacWSIsSettingsExtensionTarget(const char *target) {
    return MacWSIsStockSettingsExecutable(target);
}

static int MacWSIsAuthdTarget(const char *target) {
    static const char authdTarget[] =
        "/System/Library/Frameworks/Security.framework/Versions/A/"
        "XPCServices/authd.xpc/Contents/MacOS/authd";
    return target && MacWSStringContains(target, authdTarget) &&
        MacWSStringContains(authdTarget, target);
}

// Narrow allow-list for system trust/keychain daemons. Exact path match only —
// do not accept arbitrary MACWS_XPC_TARGET values.
static int MacWSIsExactPath(const char *target, const char *expected) {
    if (!target || !expected) return 0;
    const char *a = target, *b = expected;
    while (*a && *b && *a == *b) { a++; b++; }
    return !*a && !*b;
}

static int MacWSIsTrustdTarget(const char *target) {
    return MacWSIsExactPath(target, "/usr/libexec/trustd");
}

static int MacWSIsSecuritydTarget(const char *target) {
    return MacWSIsExactPath(target, "/usr/sbin/securityd");
}

static const char *MacWSTargetForProxy(const char *program, char *envp[]) {
#if defined(MACWS_WEBKIT_PROXY_EXPERIMENT)
    if (MacWSIsOverlayProgram(program)) {
        const char *overlay = MacWSOverlayTarget(program);
        // Never fall through to a fixed target for an overlay image.
        if (!overlay) MacWSExit(120);
        return overlay;
    }
    for (unsigned i = 0; i < sizeof(MacWSWebKitServices) / sizeof(MacWSWebKitServices[0]); ++i)
        if (MacWSStringContains(program, MacWSWebKitServices[i].proxy))
            return MacWSWebKitServices[i].target;
#endif
    // RunningBoard cannot submit a macOS .appex path to the iPadOS launchd
    // service cache.  MacWSCatalystLaunch therefore submits this already
    // registered iOS proxy and carries the original executable path in the
    // extension overlay.  Validate that path narrowly before entering the
    // chroot; every launchd-provided XPC endpoint/environment entry remains
    // untouched for the real ExtensionKit process.
    const char *extensionTarget =
        MacWSEnvironmentValue(envp, "MACWS_EXTENSION_TARGET");
    if (MacWSIsSettingsExtensionTarget(extensionTarget)) {
        return extensionTarget;
    }
    // A normal login launchd discovers CarbonCore's csnameddatad through its
    // XPC bundle. MacWS has no chroot XPC bundle domain, so the root launchd
    // job reuses this freestanding chroot transition and selects exactly that
    // stock executable. Do not accept an arbitrary path from the environment.
    const char *xpcTarget = MacWSEnvironmentValue(envp, "MACWS_XPC_TARGET");
    static const char csNamedDataTarget[] =
        "/System/Library/Frameworks/CoreServices.framework/Versions/A/"
        "Frameworks/CarbonCore.framework/Versions/A/XPCServices/"
        "csnameddatad.xpc/Contents/MacOS/csnameddatad";
    if (xpcTarget && MacWSStringContains(xpcTarget, csNamedDataTarget) &&
        MacWSStringContains(csNamedDataTarget, xpcTarget)) {
        return csNamedDataTarget;
    }
    // Authorization.framework asks launchd for the system XPC service
    // `com.apple.authd`. MacWS publishes the unmodified Ventura service on a
    // private endpoint because iPadOS owns the public name with an incompatible
    // protocol. Preserve the one-shot XPC launch context through this
    // freestanding proxy just as for csnameddatad above.
    static const char authdTarget[] =
        "/System/Library/Frameworks/Security.framework/Versions/A/"
        "XPCServices/authd.xpc/Contents/MacOS/authd";
    if (xpcTarget && MacWSIsAuthdTarget(xpcTarget)) {
        return authdTarget;
    }
    // System-wide TLS/keychain: iPadOS owns public trustd/securityd names.
    // Publish Ventura binaries on macosbooter.* (see macos_gui.sh) and enter
    // chroot through this same freestanding first image.
    if (xpcTarget && MacWSIsTrustdTarget(xpcTarget)) {
        return "/usr/libexec/trustd";
    }
    if (xpcTarget && MacWSIsSecuritydTarget(xpcTarget)) {
        return "/usr/sbin/securityd";
    }
    if (MacWSStringContains(program, "SettingsExtensionProxy")) {
        return "/System/Library/ExtensionKit/Extensions/Appearance.appex/"
               "Contents/MacOS/Appearance";
    }
    if (MacWSStringContains(program, "HIServicesProxy")) {
        return "/System/Library/Frameworks/ApplicationServices.framework/"
               "Versions/A/Frameworks/HIServices.framework/Versions/A/"
               "XPCServices/com.apple.hiservices-xpcservice.xpc/Contents/"
               "MacOS/com.apple.hiservices-xpcservice";
    }
    if (MacWSStringContains(program, "OpenAndSavePanelProxy")) {
        return "/System/Library/Frameworks/AppKit.framework/Versions/C/"
               "XPCServices/com.apple.appkit.xpc.openAndSavePanelService.xpc/"
               "Contents/MacOS/com.apple.appkit.xpc.openAndSavePanelService";
    }
    if (MacWSStringContains(program, "QuickLookUIServiceProxy")) {
        return "/System/Library/Frameworks/QuickLookUI.framework/Versions/A/"
               "XPCServices/QuickLookUIService.xpc/Contents/MacOS/"
               "QuickLookUIService";
    }
    if (MacWSStringContains(program, "DockHelperProxy")) {
        return "/System/Library/CoreServices/Dock.app/Contents/XPCServices/"
               "DockHelper.xpc/Contents/MacOS/DockHelper";
    }
    if (MacWSStringContains(program, "ExtensionKitProxy")) {
        return "/System/Library/Frameworks/ExtensionFoundation.framework/"
               "Versions/A/XPCServices/extensionkitservice.xpc/Contents/"
               "MacOS/extensionkitservice";
    }
    if (MacWSStringContains(program, "GeodProxy")) {
        return "/System/Library/PrivateFrameworks/GeoServices.framework/"
               "Versions/A/XPCServices/com.apple.geod.xpc/Contents/MacOS/"
               "com.apple.geod";
    }
    if (MacWSStringContains(program, "WriteConfigProxy")) {
        return "/System/Library/PrivateFrameworks/"
               "SystemAdministration.framework/XPCServices/"
               "writeconfig.xpc/Contents/MacOS/writeconfig";
    }
    if (MacWSStringContains(program, "LocationdProxy")) {
        return "/usr/libexec/locationd";
    }
    return "/System/Library/PrivateFrameworks/ViewBridge.framework/Versions/A/"
           "XPCServices/ViewBridgeAuxiliary.xpc/Contents/MacOS/"
           "ViewBridgeAuxiliary";
}

static int MacWSSpawnPostExecDebugMarker(char *envp[]) {
    int isChild = 0;
    long child = MacWSFork(&isChild);
    if (isChild) {
        static char pidString[24];
        static char jbctl[] = "/var/jb/usr/bin/jbctl";
        static char operation[] = "proc_set_debugged";
        static char *arguments[] = { jbctl, operation, pidString, NULL };
        unsigned long value =
            (unsigned long)MacWSSyscall0(MACWS_SYS_getppid);
        char reversed[24];
        size_t length = 0;
        do {
            reversed[length++] = (char)('0' + value % 10);
            value /= 10;
        } while (value && length < sizeof(reversed));
        for (size_t index = 0; index < length; index++)
            pidString[index] = reversed[length - index - 1];
        pidString[length] = '\0';

        // execve replaces the proxy in the parent and clears CS_DEBUGGED.
        // Wait until that transition has completed, then ask Dopamine's
        // supported process primitive to mark the final Appearance image.
        // poll(NULL, 0, 75) is a raw-syscall sleep and does not initialize
        // libSystem/libxpc in this one-shot helper child.
        (void)MacWSSyscall3(MACWS_SYS_poll, NULL, (const void *)0,
                            (const void *)75);
        (void)MacWSSyscall3(MACWS_SYS_execve, jbctl, arguments, envp);
        MacWSExit(117);
    }
    return child > 0;
}

int main(int argc, char *argv[], char *envp[]) {
    static char *targetArguments[MACWS_MAX_ARGUMENTS];
    static char *targetEnvironment[MACWS_MAX_ENVIRONMENT];
    static char insertLibrary[] =
        "DYLD_INSERT_LIBRARIES=/usr/local/lib/libmachook.dylib";
    static char appExtension[] = "MACWS_APP_EXTENSION=1";
    static char home[] = "HOME=/Users/root";
    static char temporaryDirectory[] = "TMPDIR=/tmp";
    static char nanoZone[] = "MallocNanoZone=0";
    static char rootEnvironment[MACWS_CHROOT_ROOT_ENV_SIZE];

    // Every entry route must establish the same real chroot namespace as
    // launchdchrootexec. Without it, CoreServices sees a host root parent and
    // can traverse/cache paths that are not reachable inside this process.
    if (!MacWSBuildChrootRootEnvironment(MACWS_ROOTFS, rootEnvironment,
            sizeof(rootEnvironment), MacWSCheckedSyscall3)) {
        // Already inside the rootfs (client posix_spawn): F_GETPATH is "/".
        // Keep the parent's canonical host root; do not exit 118 or use "/".
        const char *inherited = MacWSEnvironmentValue(envp, MACWS_CHROOT_ROOT_KEY);
        if (!MacWSAdoptInheritedChrootRootEnvironment(
                inherited, rootEnvironment, sizeof(rootEnvironment)))
            MacWSExit(118);
    }

    const char *target = MacWSTargetForProxy(
        argc > 0 && argv ? argv[0] : (const char *)0, envp);
    const int isSettingsExtension = MacWSIsSettingsExtensionTarget(target);

    // The real settings extension cannot fork after its ExtensionKit sandbox
    // is active, and exec clears a debug state acquired in this proxy.  Keep a
    // minimal root child outside the chroot just long enough to mark the final
    // post-exec process through Dopamine's supported jbctl primitive.
    if (isSettingsExtension && !MacWSSpawnPostExecDebugMarker(envp)) {
        MacWSExit(116);
    }

    size_t argumentCount = 0;
    targetArguments[argumentCount++] = (char *)target;
    for (int index = 1; index < argc &&
         argumentCount + 1 < MACWS_MAX_ARGUMENTS; index++) {
        targetArguments[argumentCount++] = argv[index];
    }
    targetArguments[argumentCount] = (char *)0;

#if defined(MACWS_WEBKIT_PROXY_EXPERIMENT)
    // XPC_* is passed through unchanged (field notes §58/§59). Only the
    // credential hop and the WebKit Gigacage default remain here.
    int isWebKit = 0;
    for (unsigned i = 0; i < sizeof(MacWSWebKitServices) / sizeof(MacWSWebKitServices[0]); ++i) {
        if (target && MacWSStringContains(target, MacWSWebKitServices[i].target))
            isWebKit = 1;
    }
    const int isOverlay = MacWSIsOverlayProgram(
        argc > 0 && argv ? argv[0] : (const char *)0);
    const char *incomingFlags = MacWSEnvironmentValue(envp, "XPC_FLAGS");
    const char *peerEnv = MacWSEnvironmentValue(envp, "MACWS_WK_PEER");
    const int isUniquePeer = peerEnv &&
        MacWSStringContains(peerEnv, "com.macwsguide.wk.i.");
    const char *passFlags =
        incomingFlags && *incomingFlags ? incomingFlags : "(unset)";
        const char *program = argc > 0 && argv ? argv[0] : (const char *)0;
        const int alreadyPriv = MacWSEndsWith(program, ".priv");
        const long euid = MacWSSyscall0(MACWS_SYS_geteuid);
        const int willExecPriv = (isWebKit || isOverlay) && euid != 0 &&
            !alreadyPriv && MacWSPathHasSlash(program);
        if (isWebKit || isOverlay) {
            MacWSLogWebKitProxyFlags(incomingFlags, passFlags,
                program, target, isUniquePeer);
            MacWSLogWebKitProxyHandshake(program, alreadyPriv, willExecPriv);
        }
        // Non-setuid CFBundleExecutable first; only elevate via *.priv when
        // this image is still unprivileged. Do not bootstrap_check_in.
        if (willExecPriv) {
            if (MacWSWebKitExecPrivSibling(program, argv, envp))
                MacWSExit(117);
        }
#endif

    size_t environmentCount = 0;
    for (char **entry = envp; entry && *entry; entry++) {
        // Replace only values that must describe the macOS process.  Preserve
        // XPC_FLAGS and every launchd-provided entry byte-for-byte.
        if (MacWSHasEnvironmentKey(*entry, "DYLD_INSERT_LIBRARIES") ||
            MacWSHasEnvironmentKey(*entry, "HOME") ||
            MacWSHasEnvironmentKey(*entry, "TMPDIR") ||
            MacWSHasEnvironmentKey(*entry, "MallocNanoZone") ||
            MacWSHasEnvironmentKey(*entry, MACWS_CHROOT_ROOT_KEY))
            continue;
        // Five canonical values plus NULL remain. Reject an oversized launch
        // context instead of silently discarding trailing one-shot XPC data.
        if (environmentCount + 6 >= MACWS_MAX_ENVIRONMENT) MacWSExit(119);
        targetEnvironment[environmentCount++] = *entry;
    }
    // Settings panes are sandboxed ExtensionKit processes.  They cannot fork,
    // so libmachook's fork+ptrace JIT entitlement bootstrap is neither usable
    // nor needed by these native AppKit bundles.  Their compatibility library
    // is carried by a bundle-local LC_LOAD_DYLIB (installed by postinst), which
    // also avoids the extension sandbox's denial of the global /usr/local path.
    if (isSettingsExtension) {
        targetEnvironment[environmentCount++] = appExtension;
    } else {
        targetEnvironment[environmentCount++] = insertLibrary;
    }
    // Belt-and-suspenders with libmachook getenv_new (system default
    // GIGACAGE_ENABLED=0). Full 80GiB/32GiB-align Gigacage is VA-infeasible on
    // iPad14,5 chroot (see field notes); keep explicit env for WebKit proxies
    // until getenv interpose is verified in every XPC spawn path.
#if defined(MACWS_WEBKIT_PROXY_EXPERIMENT)
    {
        static char gigacageOff[] = "GIGACAGE_ENABLED=0";
        if (isWebKit) {
            if (environmentCount + 1 >= MACWS_MAX_ENVIRONMENT) MacWSExit(119);
            targetEnvironment[environmentCount++] = gigacageOff;
        }
    }
#endif
    targetEnvironment[environmentCount++] = home;
    targetEnvironment[environmentCount++] = temporaryDirectory;
    targetEnvironment[environmentCount++] = nanoZone;
    targetEnvironment[environmentCount++] = rootEnvironment;
    targetEnvironment[environmentCount] = (char *)0;

    if (MacWSSyscall1(MACWS_SYS_chroot, MACWS_ROOTFS) != 0) MacWSExit(111);
    if (MacWSSyscall1(MACWS_SYS_chdir, "/") != 0) MacWSExit(112);
    if (isSettingsExtension) {
        // The proxy is setuid-root only so this first image can cross the
        // chroot boundary.  RunningBoard created the extension job for the
        // mobile host and iPadOS container setup is performed at exec time;
        // leaving euid 0 here makes the kernel reject that mobile container.
        // Drop every saved/effective credential before the real extension
        // image is admitted, while retaining the one-shot XPC environment.
        // Do not generalize this to ViewBridge/HIServices: a runtime A/B on
        // 2026-08-04 showed that mobile ViewBridge cannot look up the root
        // WindowServer session and exits 2 before publishing its listener.
        if (MacWSSyscall1(MACWS_SYS_setgid, (const void *)501) != 0)
            MacWSExit(114);
        if (MacWSSyscall1(MACWS_SYS_setuid, (const void *)501) != 0)
            MacWSExit(115);
    }
    (void)MacWSSyscall3(MACWS_SYS_execve, target, targetArguments,
                        targetEnvironment);
    MacWSExit(113);
}
