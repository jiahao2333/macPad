#!/bin/bash
# Reproduce the WebKit/Safari overlay XPC route on a fresh install
# (field notes §59-§62). Run as root from postinst.sh through bash:
#   bash macws_webkit_overlay_install.sh            install / refresh
#   bash macws_webkit_overlay_install.sh --remove   delete only these overlays
#
# 1. MacWSXPCProxy (XPCOverlayChrootProxy, packaged at
#    /var/jb/usr/macOS/libexec) stays root:wheel 0755: iOS xpcproxy refuses
#    setugid XPC service images and only deposits the one-shot receive right
#    into a plain image. MacWSXPCProxy.priv is a byte copy, root:wheel 4755;
#    the stub re-execs it to get chroot(2) rights.
# 2. macws_xpc_overlay_gen.py is run for the WebKit helper bundles and the
#    Safari SandboxBroker ONLY (never --all): one launchd-managed Application
#    bundle per helper under /var/jb/usr/macOS/libexec/macws-xpc-overlay/<key>
#    plus the chroot index /var/mnt/rootfs/usr/local/var/macws-xpc-overlay/<key>.
#    com.apple.WebKit.* identifiers are reserved by iOS, so the generator
#    registers them as com.macwsguide.xpc.com.apple.WebKit.*.
# 3. The proxy CDHash is admitted (reboot-volatile; macos_gui.sh re-admits it
#    on every GUI start through macws_macos_trust_services.sh --start).
# Never loads the legacy com.macwsguide.webkit.* LaunchDaemons.
PATH=/var/jb/usr/bin:/usr/bin:/bin:/usr/sbin:/sbin
ROOTFS=${MACWS_ROOTFS:-/var/mnt/rootfs}
PROXY=/var/jb/usr/macOS/libexec/MacWSXPCProxy
PRIV="$PROXY.priv"
GEN=/var/jb/usr/macOS/bin/macws_xpc_overlay_gen.py
OVERLAY=/var/jb/usr/macOS/libexec/macws-xpc-overlay
INDEX="$ROOTFS/usr/local/var/macws-xpc-overlay"
W=/System/Library/Frameworks/WebKit.framework/Versions/A/XPCServices
S=/Applications/Safari.app/Contents/XPCServices
BUNDLES="$W/com.apple.WebKit.WebContent.xpc $W/com.apple.WebKit.Networking.xpc $W/com.apple.WebKit.GPU.xpc $S/com.apple.Safari.SandboxBroker.xpc"

if [ "${1:-}" = "--remove" ]; then
    for f in "$INDEX"/*; do
        [ -f "$f" ] || continue
        case "$(cut -f1 "$f")" in
        com.apple.WebKit.*|com.apple.Safari.SandboxBroker)
            k=${f##*/}
            rm -rf "$OVERLAY/$k" "$f"
            echo "[INFO] removed WebKit overlay $k"
            ;;
        esac
    done
    exit 0
fi

[ -f "$PROXY" ] || { echo "[WARN] $PROXY not installed; WebKit overlay route skipped"; exit 0; }
[ -f "$GEN" ] || { echo "[WARN] $GEN missing; WebKit overlay route skipped"; exit 0; }
[ -d "$ROOTFS$W" ] || { echo "[WARN] rootfs WebKit XPCServices missing; WebKit overlay route skipped"; exit 0; }

chown root:wheel "$PROXY" && chmod 0755 "$PROXY" || exit 1
if [ ! -f "$PRIV" ] || ! cmp -s "$PROXY" "$PRIV"; then
    tmp="$PRIV.new.$$"
    cp "$PROXY" "$tmp" && chown root:wheel "$tmp" && chmod 4755 "$tmp" &&
        mv -f "$tmp" "$PRIV" || { rm -f "$tmp"; echo "[ERROR] could not create $PRIV"; exit 1; }
    echo "[INFO] refreshed $PRIV (byte copy of MacWSXPCProxy)"
fi
chown root:wheel "$PRIV" && chmod 4755 "$PRIV" || exit 1

for arch in arm64e arm64; do
    h=$(ldid -arch "$arch" -h "$PROXY" 2>/dev/null | sed -n 's/^CDHash=\([0-9a-f]*\).*/\1/p' | head -1)
    [ -n "$h" ] && jbctl trustcache add "$h" >/dev/null 2>&1 && echo "[INFO] MacWSXPCProxy $arch CDHash $h admitted"
done

# Hard links in the overlay bundles keep the inode they were created from;
# regenerate so they follow a reinstalled proxy/.priv.
# shellcheck disable=SC2086
/var/jb/usr/bin/python3 "$GEN" $BUNDLES || { echo "[ERROR] macws_xpc_overlay_gen.py failed"; exit 1; }
for d in "$OVERLAY"/*/*.xpc; do
    [ -d "$d" ] || continue
    if ! cmp -s "$d/MacWSXPCProxy" "$PROXY" || [ ! -u "$d/MacWSXPCProxy.priv" ] || [ -u "$d/MacWSXPCProxy" ]; then
        echo "[ERROR] overlay $d does not match MacWSXPCProxy 0755/.priv 4755"
        exit 1
    fi
done
echo "[INFO] WebKit/Safari overlay route ready"
