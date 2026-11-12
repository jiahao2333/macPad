#!/bin/bash
# §58 throwaway probes: execkill (kernel exec semantics), bundle-launched
# Application-type service via iOS launchd/xpcproxy (shim + client), and the
# crash-annotation-aware macOS test service. Builds into $STAGE only.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/misc/appsvc_probe"
STAGE="${STAGE:-/tmp/appsvc-probe-stage58}"
IOS_SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
CC="$(xcrun --sdk iphoneos --find clang)"
MAC_SDK="$(xcrun --sdk macosx --show-sdk-path)"
MAC_CC="$(xcrun --sdk macosx --find clang)"
ENT="$ROOT/entitlements.plist"
rm -rf "$STAGE"; mkdir -p "$STAGE/macws-appsvcb.xpc" "$STAGE/macws-appsvc-probe.xpc"
IOS=(-arch arm64e -isysroot "$IOS_SDK" -miphoneos-version-min=14.0 -O2 -g -I"$SRC")
for p in probe_execkill probe_bclient probe_crun; do
  [ -f "$SRC/$p.c" ] && "$CC" "${IOS[@]}" -o "$STAGE/$p" "$SRC/$p.c"
done
if [ -f "$SRC/probe_shim.c" ]; then
  "$CC" "${IOS[@]}" -o "$STAGE/macws-appsvcb.xpc/macws-appsvcb" "$SRC/probe_shim.c"
  cp "$SRC/InfoB.plist" "$STAGE/macws-appsvcb.xpc/Info.plist"
fi
"$MAC_CC" -arch arm64 -target arm64-apple-macos13 -isysroot "$MAC_SDK" \
  -mmacosx-version-min=13.0 -fblocks -O2 -g -I"$SRC" \
  -o "$STAGE/macws-appsvc-probe.xpc/macws-appsvc-probe" \
  "$SRC/probe_appsvc.c" -framework CoreFoundation
cp "$SRC/Info.plist" "$STAGE/macws-appsvc-probe.xpc/Info.plist"
# Same client source built as a macOS 13 arm64 image for the in-chroot client test.
"$MAC_CC" -arch arm64 -target arm64-apple-macos13 -isysroot "$MAC_SDK" \
  -mmacosx-version-min=13.0 -fblocks -O2 -g -I"$SRC" -DMACWS_MAC_CLIENT=1 \
  -o "$STAGE/macws-appsvc-probe.xpc/macws-appsvc-mclient" "$SRC/probe_bclient.c"
for f in "$STAGE"/probe_* "$STAGE"/macws-appsvcb.xpc/macws-appsvcb "$STAGE"/macws-appsvc-probe.xpc/macws-appsvc-probe "$STAGE"/macws-appsvc-probe.xpc/macws-appsvc-mclient; do
  [ -f "$f" ] || continue
  ldid -S"$ENT" "$f"
done
cp "$SRC/run_probe58.sh" "$STAGE/" 2>/dev/null || true
ls -la "$STAGE" "$STAGE"/*.xpc
