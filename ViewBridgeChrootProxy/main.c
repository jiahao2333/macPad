// Freestanding launch stub for macOS XPC services inside the chroot.
//
// xpcproxy gives the service process a one-shot launchd receive right in
// XPC_FLAGS/XPC_* and SERVICE_SLOT (mach_ports_register slot 2). Preserve
// that context through a raw chroot/exec transition; do not introduce an
// intermediate posix_spawn or XPC client initialization of the public
// macosbooter name.
// For Application-type WebKit helpers, iOS xpcproxy leaves XPC_FLAGS=0x0
// and deposits no receive right when the bundle executable is setuid
// (setugid XPC services are not allowed). The CFBundleExecutable is
// therefore shipped mode 0755 so the host xpcproxy / xpc_add_bundle
// handshake can stash the one-shot right. If this image is still
// unprivileged after that handshake, exec the setuid sibling (*.priv)
// before chroot — Mach ports are expected to survive that exec — then
// reconstitute XPC_FLAGS=8019 if the incoming value is still zero so
// Ventura xpc_main can consume the inherited right after execve.
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
static int MacWSFlagsAreZero(const char *value) {
    // Treat unset, empty, and 0 / 0x0 / 0x00 as "no managed receive right".
    if (!value || !*value) return 1;
    if (value[0] == '0' && (value[1] == 'x' || value[1] == 'X'))
        value += 2;
    if (!*value) return 1;
    while (*value) {
        if (*value != '0') return 0;
        value++;
    }
    return 1;
}

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
                                     const char *program, const char *target) {
    char line[512];
    size_t len = 0;
    MacWSAppendString(line, sizeof(line), &len, "proxy XPC_FLAGS incoming=");
    MacWSAppendString(line, sizeof(line), &len,
                      incoming && *incoming ? incoming : "(unset)");
    MacWSAppendString(line, sizeof(line), &len, " outgoing=");
    MacWSAppendString(line, sizeof(line), &len, outgoing);
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
    (void)MacWSSyscall3(MACWS_SYS_execve, privPath, argv, envp);
    return 1;
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
            sizeof(rootEnvironment), MacWSCheckedSyscall3)) MacWSExit(118);

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
    // Stock macOS Application-type WebKit helpers (WebContent, Networking,
    // GPU, Safari.SandboxBroker) are launched with XPC_FLAGS=8019. iOS
    // xpcproxy leaves 0x0 on this setuid iPhoneOS-platform first image, so
    // stock Ventura xpc_main sees no managed receive right after exec.
    // Mach ports survive raw chroot+execve; reconstituting the Application-
    // type flags lets stock xpc_main consume the inherited one-shot right.
    // Do not bootstrap_check_in the public macosbooter name.
    int isWebKit = 0;
    for (unsigned i = 0; i < sizeof(MacWSWebKitServices) / sizeof(MacWSWebKitServices[0]); ++i) {
        if (target && MacWSStringContains(target, MacWSWebKitServices[i].target))
            isWebKit = 1;
    }
    const char *incomingFlags = MacWSEnvironmentValue(envp, "XPC_FLAGS");
    static char webkitFlags[] = "XPC_FLAGS=8019";
    const int replaceZeroFlags = isWebKit && MacWSFlagsAreZero(incomingFlags);
    const char *outgoingFlags = replaceZeroFlags ? "8019"
        : (incomingFlags && *incomingFlags ? incomingFlags : "(unset)");
        const char *program = argc > 0 && argv ? argv[0] : (const char *)0;
        const int alreadyPriv = MacWSEndsWith(program, ".priv");
        const long euid = MacWSSyscall0(MACWS_SYS_geteuid);
        const int willExecPriv = isWebKit && euid != 0 && !alreadyPriv &&
            MacWSPathHasSlash(program);
        if (isWebKit) {
            MacWSLogWebKitProxyFlags(incomingFlags, outgoingFlags,
                program, target);
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
        // XPC_FLAGS and every launchd-provided entry byte-for-byte, except a
        // zero Application-type WebKit XPC_FLAGS which is reconstituted below.
        if (MacWSHasEnvironmentKey(*entry, "DYLD_INSERT_LIBRARIES") ||
            MacWSHasEnvironmentKey(*entry, "HOME") ||
            MacWSHasEnvironmentKey(*entry, "TMPDIR") ||
            MacWSHasEnvironmentKey(*entry, "MallocNanoZone") ||
            MacWSHasEnvironmentKey(*entry, MACWS_CHROOT_ROOT_KEY))
            continue;
#if defined(MACWS_WEBKIT_PROXY_EXPERIMENT)
        if (replaceZeroFlags && MacWSHasEnvironmentKey(*entry, "XPC_FLAGS"))
            continue;
#endif
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
        if (replaceZeroFlags) {
            if (environmentCount + 1 >= MACWS_MAX_ENVIRONMENT) MacWSExit(119);
            targetEnvironment[environmentCount++] = webkitFlags;
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
