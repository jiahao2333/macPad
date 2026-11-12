#!/bin/bash
# §59 real-path probe artifacts: macOS app bundle with an embedded
# Application-type XPC service, an in-chroot client that only calls
# xpc_connection_create (no xpc_add_bundle), and the iOS chroot launcher.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/misc/appsvc_probe"
STAGE="${STAGE:-/tmp/appsvc-probe-stage59}"
IOS_SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
CC="$(xcrun --sdk iphoneos --find clang)"
MAC_SDK="$(xcrun --sdk macosx --show-sdk-path)"
MAC_CC="$(xcrun --sdk macosx --find clang)"
ENT="$ROOT/entitlements.plist"
APP="$STAGE/MacWSOverlayProbe.app/Contents"
SVC="$APP/XPCServices/com.macwsguide.probe.overlay.xpc/Contents"
SVCMI="$APP/XPCServices/com.macwsguide.probe.overlaymi.xpc/Contents"
# §60: reserved-prefix id -> generator alias; real bundle id != launchd name
SVCWK="$APP/XPCServices/com.apple.WebKit.MacWSProbe.overlaymi.xpc/Contents"
rm -rf "$STAGE"; mkdir -p "$APP/MacOS" "$SVC/MacOS" "$SVCMI/MacOS" "$SVCWK/MacOS"
MAC=(-arch arm64 -target arm64-apple-macos13 -isysroot "$MAC_SDK" -mmacosx-version-min=13.0 -fblocks -O2 -I"$SRC")
"$CC" -arch arm64e -isysroot "$IOS_SDK" -miphoneos-version-min=14.0 -O2 -I"$SRC" -o "$STAGE/probe_crun" "$SRC/probe_crun.c"
"$MAC_CC" "${MAC[@]}" -o "$APP/MacOS/MacWSOverlayProbe" "$SRC/probe_ovclient.c"
"$MAC_CC" "${MAC[@]}" -o "$SVC/MacOS/macws-appsvc-probe" "$SRC/probe_appsvc.c" -framework CoreFoundation
cp "$SRC/InfoOverlayApp.plist" "$APP/Info.plist"
cp "$SRC/InfoOverlaySvc.plist" "$SVC/Info.plist"
cp "$SRC/InfoOverlaySvcMI.plist" "$SVCMI/Info.plist"
cp "$SVC/MacOS/macws-appsvc-probe" "$SVCMI/MacOS/macws-appsvc-probe"
cp "$SRC/InfoOverlaySvcWK.plist" "$SVCWK/Info.plist"
cp "$SVC/MacOS/macws-appsvc-probe" "$SVCWK/MacOS/macws-appsvc-probe"
"$CC" -arch arm64e -isysroot "$IOS_SDK" -miphoneos-version-min=14.0 -O2 -o "$STAGE/sbcheck" "$SRC/sbcheck.c"
for f in "$STAGE/sbcheck" "$SVCWK/MacOS/macws-appsvc-probe" "$SVCMI/MacOS/macws-appsvc-probe" "$STAGE/probe_crun" "$APP/MacOS/MacWSOverlayProbe" "$SVC/MacOS/macws-appsvc-probe"; do
  ldid -S"$ENT" "$f"
done
cp "$SRC/run_probe59.sh" "$STAGE/"
find "$STAGE" -type f | sort
