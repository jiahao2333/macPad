#ifndef MACWS_WEBKIT_SERVICES_H
#define MACWS_WEBKIT_SERVICES_H
// Prefer stock Ventura XPC paths after macws_admit_rootfs_binary (CT policy).
// Runtime Safari XPC remaps to macosbooter.* + xpc_add_bundle (DockHelper-style).
typedef struct {
    const char *original;
    const char *privateName;
    const char *proxy;
    const char *target;
} MacWSWebKitService;
static const MacWSWebKitService MacWSWebKitServices[] = {
    {"com.apple.WebKit.WebContent", "com.apple.macosbooter.WebKit.WebContent", "WebContentProxy",
     "/System/Library/Frameworks/WebKit.framework/Versions/A/XPCServices/com.apple.WebKit.WebContent.xpc/Contents/MacOS/com.apple.WebKit.WebContent"},
    {"com.apple.WebKit.Networking", "com.apple.macosbooter.WebKit.Networking", "WebNetworkingProxy",
     "/System/Library/Frameworks/WebKit.framework/Versions/A/XPCServices/com.apple.WebKit.Networking.xpc/Contents/MacOS/com.apple.WebKit.Networking"},
    {"com.apple.WebKit.GPU", "com.apple.macosbooter.WebKit.GPU", "WebGPUProxy",
     "/System/Library/Frameworks/WebKit.framework/Versions/A/XPCServices/com.apple.WebKit.GPU.xpc/Contents/MacOS/com.apple.WebKit.GPU"},
    {"com.apple.Safari.SandboxBroker", "com.apple.macosbooter.Safari.SandboxBroker", "SafariSandboxProxy",
     "/System/Volumes/Preboot/Cryptexes/App/System/Applications/Safari.app/Contents/XPCServices/com.apple.Safari.SandboxBroker.xpc/Contents/MacOS/com.apple.Safari.SandboxBroker"},
};
#endif
