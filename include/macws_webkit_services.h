#ifndef MACWS_WEBKIT_SERVICES_H
#define MACWS_WEBKIT_SERVICES_H
// Runtime Safari XPC trace selects these exact services. Native macOS bundle
// identifiers must not resolve to an iPadOS WebKit process/version. Preserve
// XPC per-instance activation and the one-shot bootstrap context through the
// same freestanding first-image transition as DockHelper.
typedef struct {
    const char *original;
    const char *privateName;
    const char *proxy;
    const char *target;
} MacWSWebKitService;
static const MacWSWebKitService MacWSWebKitServices[] = {
    {"com.apple.WebKit.WebContent", "com.apple.macosbooter.WebKit.WebContent", "WebContentProxy",
     "/usr/local/libexec/macws-webkit/com.apple.WebKit.WebContent.xpc/Contents/MacOS/com.apple.WebKit.WebContent"},
    {"com.apple.WebKit.Networking", "com.apple.macosbooter.WebKit.Networking", "WebNetworkingProxy",
     "/usr/local/libexec/macws-webkit/com.apple.WebKit.Networking.xpc/Contents/MacOS/com.apple.WebKit.Networking"},
    {"com.apple.WebKit.GPU", "com.apple.macosbooter.WebKit.GPU", "WebGPUProxy",
     "/usr/local/libexec/macws-webkit/com.apple.WebKit.GPU.xpc/Contents/MacOS/com.apple.WebKit.GPU"},
    {"com.apple.Safari.SandboxBroker", "com.apple.macosbooter.Safari.SandboxBroker", "SafariSandboxProxy",
     "/usr/local/libexec/macws-webkit/com.apple.Safari.SandboxBroker.xpc/Contents/MacOS/com.apple.Safari.SandboxBroker"},
};
#endif
