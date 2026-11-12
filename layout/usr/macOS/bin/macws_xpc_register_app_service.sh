#!/bin/bash
# Register an Application-type macOS XPC service for chroot (system-level).
set -euo pipefail
TSV="${MACWS_XPC_TSV:-/var/jb/usr/macOS/etc/macws-xpc-app-services.tsv}"
ORIG=""; PRIV=""; PROXY=""; ADMIT=""
while [ $# -gt 0 ]; do
  case "$1" in
    --original) ORIG="$2"; shift 2;;
    --private) PRIV="$2"; shift 2;;
    --proxy-bundle) PROXY="$2"; shift 2;;
    --admit) ADMIT="$2"; shift 2;;
    *) echo "unknown $1" >&2; exit 2;;
  esac
done
[ -n "$ORIG" ] && [ -n "$PRIV" ] && [ -n "$PROXY" ] || {
  echo "need --original --private --proxy-bundle" >&2; exit 2; }
mkdir -p "$(dirname "$TSV")"
rel="$PROXY"
case "$PROXY" in
  /var/jb/usr/macOS/Frameworks/*) rel="${PROXY#/var/jb/usr/macOS/Frameworks/}";;
esac
if [ -f "$TSV" ] && grep -q "^${ORIG}	" "$TSV" 2>/dev/null; then
  grep -v "^${ORIG}	" "$TSV" > "${TSV}.new" || true
  mv "${TSV}.new" "$TSV"
fi
printf '%s\t%s\t%s\n' "$ORIG" "$PRIV" "$rel" >> "$TSV"
echo "registered $ORIG -> $PRIV ($rel)"
if [ -n "$ADMIT" ]; then
  /var/jb/usr/macOS/bin/macws_admit_rootfs_binary.sh "$ADMIT" "$ORIG"
fi
