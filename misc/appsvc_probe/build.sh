#!/bin/bash
# Host-side build for the throwaway Application-type bring-up probe.
# Produces iOS arm64e proxy+client and a genuine macOS 13 arm64 test service
# (same arch as launchservicesd / other in-chroot custom executables).
# Does not install anything on the device.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/misc/appsvc_probe"
STAGE="${STAGE:-/tmp/appsvc-probe-stage}"
IOS_SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
ENT="$ROOT/entitlements.plist"
MIN="${MIN:-14.0}"
CC="$(xcrun --sdk iphoneos --find clang)"

rm -rf "$STAGE"
mkdir -p "$STAGE/macws-appsvc-probe.xpc"

COMMON=(-arch arm64e -isysroot "$IOS_SDK" -miphoneos-version-min="$MIN" -O2 -g
        -I"$SRC" -fobjc-arc)

echo "==> compile probe_proxy (iOS host first image)"
"$CC" "${COMMON[@]}" -fno-objc-arc -o "$STAGE/probe_proxy" "$SRC/probe_proxy.c"

echo "==> compile probe_client (iOS host)"
"$CC" "${COMMON[@]}" -fno-objc-arc -o "$STAGE/probe_client" "$SRC/probe_client.c"

echo "==> compile macws-appsvc-probe (genuine macOS 13 arm64 (in-chroot custom bins are arm64, not arm64e))"
MAC_SDK="$(xcrun --sdk macosx --show-sdk-path)"
MAC_CC="$(xcrun --sdk macosx --find clang)"
"$MAC_CC" -arch arm64 -target arm64-apple-macos13 -isysroot "$MAC_SDK" \
  -mmacosx-version-min=13.0 -fblocks -O2 -g -I"$SRC" \
  -o "$STAGE/macws-appsvc-probe.xpc/macws-appsvc-probe" \
  "$SRC/probe_appsvc.c" -framework CoreFoundation -framework System
echo "==> compile macws-appsvc-prexpc (macOS 13 arm64, libSystem only)"
"$MAC_CC" -arch arm64 -target arm64-apple-macos13 -isysroot "$MAC_SDK" \
  -mmacosx-version-min=13.0 -O2 -g -I"$SRC" \
  -o "$STAGE/macws-appsvc-probe.xpc/macws-appsvc-prexpc" \
  "$SRC/probe_prexpc.c"
cp "$SRC/Info.plist" "$STAGE/macws-appsvc-probe.xpc/Info.plist"
# Already a macOS 13 LC_BUILD_VERSION; still run the patcher so the
# min version is exactly 13.0 if the SDK stamped something else.
python3 "$ROOT/misc/set_macos_version.py" \
  "$STAGE/macws-appsvc-probe.xpc/macws-appsvc-probe" \
  "$STAGE/macws-appsvc-probe.xpc/macws-appsvc-prexpc" || true
python3 "$ROOT/misc/set_macos_version.py" \
  "$STAGE/macws-appsvc-probe.xpc/macws-appsvc-prexpc" || true

echo "==> ldid + ad-hoc codesign"
# iOS bins: entitlements, no macOS version patch.
ldid -S"$ENT" "$STAGE/probe_proxy"
ldid -S"$ENT" "$STAGE/probe_client"
ldid -S"$ENT" "$STAGE/macws-appsvc-probe.xpc/macws-appsvc-probe"
ldid -S"$ENT" "$STAGE/macws-appsvc-probe.xpc/macws-appsvc-prexpc"
codesign -f -s - "$STAGE/probe_proxy"
codesign -f -s - "$STAGE/probe_client"
codesign -f -s - "$STAGE/macws-appsvc-probe.xpc/macws-appsvc-probe"
codesign -f -s - "$STAGE/macws-appsvc-probe.xpc/macws-appsvc-prexpc"

cp "$SRC/run_probe.sh" "$STAGE/run_probe.sh"
chmod +x "$STAGE/run_probe.sh"
echo "==> stage ready: $STAGE"
file "$STAGE/probe_proxy" "$STAGE/probe_client" \
  "$STAGE/macws-appsvc-probe.xpc/macws-appsvc-probe" \
  "$STAGE/macws-appsvc-probe.xpc/macws-appsvc-prexpc"
ls -la "$STAGE" "$STAGE/macws-appsvc-probe.xpc"
