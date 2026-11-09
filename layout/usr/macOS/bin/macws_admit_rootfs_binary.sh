#!/bin/bash
# System-level AMFI CT-policy admit for Ventura rootfs/cryptex Mach-Os that
# fail freestanding chroot+exec with "unsuitable CT policy 0x8".
# Usage: macws_admit_rootfs_binary.sh /absolute/rootfs/path [identifier]
# - Backs up original to /var/mobile/macws-rollback/
# - Prefers thin arm64e when fat
# - ldid merges project entitlements + native entitlements
# - jbctl trustcache add
set -euo pipefail
PATH=/var/jb/usr/bin:/usr/bin:/bin:/usr/sbin:/sbin
ENT="${MACWS_ENTITLEMENTS:-/var/jb/usr/macOS/bin/entitlements.plist}"
ROLLBACK="${MACWS_ROLLBACK:-/var/mobile/macws-rollback}"
bin="${1:?path}"
id="${2:-}"
[ -f "$bin" ] || { echo "missing $bin" >&2; exit 1; }
[ -f "$ENT" ] || { echo "missing $ENT" >&2; exit 1; }
mkdir -p "$ROLLBACK" /tmp/macws-admit
ts=$(date +%Y%m%d-%H%M%S)
base=$(basename "$bin")
cp -a "$bin" "$ROLLBACK/${base}.pre-admit-$ts"
tmp="/tmp/macws-admit/${base}.$$"
if lipo -info "$bin" 2>&1 | grep -q 'Architectures in the fat'; then
  lipo "$bin" -thin arm64e -output "$tmp" 2>/dev/null || lipo "$bin" -thin arm64 -output "$tmp"
else
  cp -a "$bin" "$tmp"
fi
ldid -e "$tmp" > "/tmp/macws-admit/${base}.native.xml" || true
if [ -z "$id" ]; then
  id=$(ldid -h "$tmp" 2>/dev/null | sed -n 's/^Identifier=//p' | head -1)
  id="${id:-$base}"
fi
ldid -I"$id" -S"$ENT" -M "$tmp"
if [ -s "/tmp/macws-admit/${base}.native.xml" ]; then
  ldid -I"$id" -S"/tmp/macws-admit/${base}.native.xml" -M "$tmp"
  ldid -I"$id" -S"/tmp/macws-admit/${base}.native.xml" -M "$tmp"
fi
rm -f "$bin"
cp -a "$tmp" "$bin"
chmod 755 "$bin"
h=$(ldid -h "$bin" 2>/dev/null | sed -n 's/.*CDHash=\([0-9a-f]*\).*/\1/p' | head -1)
s1=$(ldid -h "$bin" 2>/dev/null | sed -n 's/.*CandidateCDHash sha1=\([0-9a-f]*\).*/\1/p' | head -1)
jbctl trustcache add "$h"
[ -n "$s1" ] && jbctl trustcache add "$s1"
echo "admitted $bin id=$id cdhash=$h backup=$ROLLBACK/${base}.pre-admit-$ts"
echo "admitted $bin cdhash=$h backup=${base}.pre-admit-$ts" >> "$ROLLBACK/NOTES.txt"
