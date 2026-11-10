#!/bin/sh
# Device-side throwaway Application-type bring-up probe.
# Does not touch live libmachook, WebKit proxies, Safari, IconServices,
# or com.macwsguide.webkit.* LaunchDaemons. No reboot.
set -e
PATH=/var/jb/usr/bin:/usr/bin:/bin:/usr/sbin:/sbin
MOBILE=/var/mobile/appsvc-probe
ROOTFS_XPC=/var/mnt/rootfs/tmp/macws-appsvc-probe.xpc
LOG=/var/mnt/rootfs/tmp/macws-appsvc-probe.log
NAMEF=/var/mnt/rootfs/tmp/macws-appsvc-probe.name
LIB=/var/mnt/rootfs/usr/local/lib/libmachook.dylib
LIB64=/var/mnt/rootfs/usr/local/lib/libmachook_arm64.dylib
ENT=/var/jb/usr/macOS/bin/entitlements.plist
EXPECT_E=d774151c
EXPECT_64=6724bb1f

hash_of() {
    ldid -arch "$2" -h "$1" 2>/dev/null | grep '^CDHash=' | cut -c8- | tr '[:upper:]' '[:lower:]'
}

echo "==== preflight live net34 (read-only) ===="
H_E=$(hash_of "$LIB" arm64e)
H_64=$(hash_of "$LIB64" arm64)
echo "libmachook arm64e CDHash=$H_E"
echo "libmachook arm64  CDHash=$H_64"
case "$H_E" in
    ${EXPECT_E}*) echo "PRE_NET34_ARM64E=ok" ;;
    *) echo "PRE_NET34_ARM64E=MISMATCH expected ${EXPECT_E}… got $H_E"; exit 20 ;;
esac
case "$H_64" in
    ${EXPECT_64}*) echo "PRE_NET34_ARM64=ok" ;;
    *) echo "PRE_NET34_ARM64=MISMATCH expected ${EXPECT_64}… got $H_64"; exit 21 ;;
esac

echo "==== IconServices / WebKit LDs (must stay as-is) ===="
launchctl list 2>/dev/null | grep -i IconServices || echo "IconServices_host_list: (none or not listed here)"
ps -ax 2>/dev/null | grep -i '[I]conServices' || true
if launchctl list 2>/dev/null | grep -i 'com.macwsguide.webkit'; then
    echo "WARNING: com.macwsguide.webkit LaunchDaemons appear listed; not loading or unloading them"
fi

echo "==== install throwaway probe only ===="
mkdir -p "$MOBILE"
if [ ! -x "$MOBILE/probe_proxy" ] || [ ! -x "$MOBILE/probe_client" ] || \
   [ ! -x "$MOBILE/macws-appsvc-probe.xpc/macws-appsvc-probe" ]; then
    echo "missing staged binaries under $MOBILE"
    ls -la "$MOBILE" || true
    exit 22
fi

# Sign on the jb/mobile vnode, then byte-copy the service into rootfs tmp.
# Do not ldid the rootfs vnode (rollback SIGKILL lesson).
ldid -S"$ENT" -M "$MOBILE/probe_proxy"
ldid -S"$ENT" -M "$MOBILE/probe_client"
ldid -S"$ENT" -M "$MOBILE/macws-appsvc-probe.xpc/macws-appsvc-probe"
chmod 755 "$MOBILE/probe_proxy" "$MOBILE/probe_client" \
    "$MOBILE/macws-appsvc-probe.xpc/macws-appsvc-probe"

trust_one() {
    f="$1"
    arch="$2"
    h=$(hash_of "$f" "$arch")
    s=$(ldid -arch "$arch" -h "$f" 2>/dev/null | grep 'CandidateCDHash sha1=' | awk -F= '{print $2}' | tr '[:upper:]' '[:lower:]' | head -1)
    echo "trust $f arch=$arch CDHash=$h sha1cand=$s"
    if [ -n "$h" ]; then jbctl trustcache add "$h" || echo "trust add $h failed (may already be present)"; fi
    if [ -n "$s" ] && [ "$s" != "$h" ]; then jbctl trustcache add "$s" || true; fi
}
trust_one "$MOBILE/probe_proxy" arm64e
trust_one "$MOBILE/probe_client" arm64e
# service is a macOS arm64 in-chroot executable
trust_one "$MOBILE/macws-appsvc-probe.xpc/macws-appsvc-probe" arm64
# also try arm64e in case the staged binary is still arm64e
trust_one "$MOBILE/macws-appsvc-probe.xpc/macws-appsvc-probe" arm64e

rm -rf "$ROOTFS_XPC"
mkdir -p "$ROOTFS_XPC"
cp -f "$MOBILE/macws-appsvc-probe.xpc/macws-appsvc-probe" "$ROOTFS_XPC/macws-appsvc-probe"
cp -f "$MOBILE/macws-appsvc-probe.xpc/Info.plist" "$ROOTFS_XPC/Info.plist"
chmod 755 "$ROOTFS_XPC/macws-appsvc-probe"
rm -f "$LOG" "$NAMEF" "$MOBILE/proxy.out" "$MOBILE/client.out" "$MOBILE/proxy.pid"

echo "==== start proxy (background) then client ===="
pkill -f '/var/mobile/appsvc-probe/probe_proxy' 2>/dev/null || true
pkill -f '/var/mobile/appsvc-probe/probe_client' 2>/dev/null || true
pkill -f 'macws-appsvc-probe' 2>/dev/null || true
sleep 0.2

nohup "$MOBILE/probe_proxy" >"$MOBILE/proxy.out" 2>&1 &
echo $! > "$MOBILE/proxy.pid"
PROXY_PID=$(cat "$MOBILE/proxy.pid")
echo "proxy_pid=$PROXY_PID"
sleep 1
if kill -0 "$PROXY_PID" 2>/dev/null; then
    echo "proxy_alive=yes after 1s"
else
    set +e
    wait "$PROXY_PID"
    wr=$?
    set -e
    echo "proxy_alive=no after 1s wait_rc=$wr"
    if [ "$wr" -ge 128 ]; then echo "proxy_signal=$((wr-128))"; fi
fi
if [ ! -f "$NAMEF" ]; then
    echo "name file not yet present; waiting"
    sleep 1
fi
echo "---- name file ----"
cat "$NAMEF" 2>/dev/null || echo "(missing)"
echo "---- log so far ----"
cat "$LOG" 2>/dev/null || echo "(no log yet)"
echo "---- proxy.out ----"
cat "$MOBILE/proxy.out" 2>/dev/null || true

set +e
"$MOBILE/probe_client" >"$MOBILE/client.out" 2>&1
CLI_RC=$?
set -e
echo "client_rc=$CLI_RC"
echo "---- client.out ----"
cat "$MOBILE/client.out" 2>/dev/null || true

sleep 2
echo "---- full probe log ----"
cat "$LOG" 2>/dev/null || echo "(no log)"

echo "---- leftover probe processes ----"
ps -ax | grep -E 'probe_proxy|probe_client|macws-appsvc-probe' | grep -v grep || echo "(none)"

if kill -0 "$PROXY_PID" 2>/dev/null; then
    echo "proxy_alive=yes before cleanup"
else
    echo "proxy_alive=no before cleanup"
fi
kill "$PROXY_PID" 2>/dev/null || true
pkill -f '/var/mobile/appsvc-probe/probe_proxy' 2>/dev/null || true
pkill -f '/var/mobile/appsvc-probe/probe_client' 2>/dev/null || true
pkill -f '/tmp/macws-appsvc-probe.xpc/macws-appsvc-probe' 2>/dev/null || true
sleep 0.3
kill -9 "$PROXY_PID" 2>/dev/null || true

echo "==== postflight live net34 unchanged ===="
H2_E=$(hash_of "$LIB" arm64e)
H2_64=$(hash_of "$LIB64" arm64)
echo "libmachook arm64e CDHash=$H2_E"
echo "libmachook arm64  CDHash=$H2_64"
if [ "$H2_E" != "$H_E" ] || [ "$H2_64" != "$H_64" ]; then
    echo "POST_NET34=CHANGED — unexpected; did not intend to touch libmachook"
    exit 23
fi
echo "POST_NET34=unchanged"
echo "DONE client_rc=$CLI_RC"
exit 0
