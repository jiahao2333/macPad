# The XPC activation bundles are installed under /var/jb/usr. A chrooted
# client must be able to read their Info.plist at the same absolute path that
# iOS launchd will later use to start the proxy.
#
# A symlink /var/mnt/rootfs/var/jb/usr -> /var/jb/usr is valid in the *host*
# namespace (Dopamine /var/jb is the procursus tree) but is an ELOOP inside
# the chroot (rootfs /var/jb/usr -> /var/jb/usr). The previous host-side
# `test -x` of that link therefore succeeded while Safari's xpc_add_bundle
# still saw access() errno 62 and could not parse the Application-type
# plist. This script requires an in-chroot read of a canary plist.
#
# Preferred: bindfs mount of /var/jb/usr when mount_bindfs is available.
# Fallback when the kernel rejects bindfs or the helper is missing: replace
# a host-only symlink with a slim directory whose macOS/Frameworks entries
# point at the dual-namespace /var/mnt/rootfs/usr/local/libexec/macws-xpc
# copies (visible on the host and, via the rootfs self-symlink, inside the
# chroot). iOS launchd still resolves the add_bundle path string in the
# host /var/jb tree, which is what keeps the helpers up.
# Do not populate usr/lib (CydiaSubstrate must not escape through this tree).
# Keep the existing /var/mnt/rootfs/var/jb/Library link alone.
set -eu

source_dir=/var/jb/usr
target_dir=/var/mnt/rootfs/var/jb/usr
parent_dir=/var/mnt/rootfs/var/jb
rootfs=/var/mnt/rootfs
mount_tool=/var/jb/usr/local/bin/mount_bindfs
system_mount=/sbin/mount
proxy_relative=macOS/Frameworks/Dock.framework/Versions/A/XPCServices/DockHelperProxy.xpc/DockHelperProxy
webkit_canary=macOS/Frameworks/WebKit.framework/Versions/A/XPCServices/WebContentProxy.xpc/Info.plist
macws_xpc=/var/mnt/rootfs/usr/local/libexec/macws-xpc
macws_xpc_abs=/var/mnt/rootfs/usr/local/libexec/macws-xpc
hostlink_backup=/var/mnt/rootfs/var/jb/usr.pre-macws-hostlink

[ -d "$source_dir" ] && [ -x "$source_dir/$proxy_relative" ] || {
    echo "MacWS: required Dock XPC proxy is absent from $source_dir" >&2
    exit 1
}
[ -d "$parent_dir" ] || mkdir -p "$parent_dir"

# Probe readability from *inside* the macOS rootfs, not the host namespace.
# Do not raw-chroot a macOS /bin/test from iOS: AMFI SIGKILLs that image
# (runtime, iPad14,5 2026-11-10). launchdchrootexec / run_bash.sh is the
# same admission path Safari uses.
run_bash=/var/jb/usr/macOS/bin/run_bash.sh
in_chroot_readable() {
    relative=$1
    if [ -x "$run_bash" ]; then
        "$run_bash" -c "test -r /var/jb/usr/$relative"
        return $?
    fi
    return 1
}

# Runtime-confirmed on iPad14,3 / iPadOS 16.5.1 (2026-10-06): this kernel
# rejects bindfs, and the packaged mount_bindfs helper publishes the exact
# source as an absolute symlink instead. That link is a valid *host*
# exposure, but it is not sufficient for a chrooted client. Accept the
# helper's exact source link only when the canary is readable inside the
# chroot; otherwise fall through and repair it.
if [ -L "$target_dir" ]; then
    [ "$(readlink "$target_dir")" = "$source_dir" ] || {
        echo "MacWS: unexpected /var/jb/usr exposure link: $target_dir" >&2
        exit 1
    }
    [ -x "$target_dir/$proxy_relative" ] || {
        echo "MacWS: /var/jb/usr exposure link does not contain the XPC proxy" >&2
        exit 1
    }
    if in_chroot_readable "$webkit_canary" || in_chroot_readable "$proxy_relative"; then
        exit 0
    fi
fi
[ -d "$target_dir" ] || mkdir -p "$target_dir"
canonical_target=$(realpath "$target_dir")
canonical_parent=$(realpath "$parent_dir")
[ -x "$system_mount" ] || {
    echo "MacWS: system mount utility is unavailable: $system_mount" >&2
    exit 1
}

# Dopamine's /var/jb is a moving preboot symlink.  mount(8) therefore prints
# the resolved procursus source (for example
# /private/preboot/.../procursus/usr), not the stable /var/jb/usr spelling
# used by this script.  The mountpoint is the invariant: require an actual
# mount at the canonical target, then verify the exact packaged proxy below.
# Runtime-confirmed on iPad14,4 / iPadOS 16.2 (2026-09-29): the valid bind was
# printed as `.../procursus/usr on .../private/var/jb/usr (bindfs, ...)`; the
# old source-qualified grep missed it and falsely rejected the populated
# mountpoint as an unsafe directory.
if "$system_mount" | grep -Fq " on $canonical_target (" ||
   "$system_mount" | grep -Fq " on $canonical_parent ("; then
    [ -x "$target_dir/$proxy_relative" ] || {
        echo "MacWS: existing /var/jb bind does not expose the XPC proxy" >&2
        exit 1
    }
    if in_chroot_readable "$webkit_canary" || in_chroot_readable "$proxy_relative"; then
        exit 0
    fi
    echo "MacWS: existing /var/jb bind is not readable inside the chroot" >&2
    exit 1
fi

# Fallback: dual-namespace macws-xpc symlinks, used when bindfs is unavailable
# or the host-only /var/jb/usr symlink ELOOP's inside the chroot.
install_macws_xpc_framework_links() {
    [ -d "$macws_xpc" ] || {
        echo "MacWS: macws-xpc tree is absent: $macws_xpc" >&2
        return 1
    }
    mkdir -p "$target_dir/macOS/Frameworks"
    found=0
    for fw in "$macws_xpc"/*.framework; do
        [ -d "$fw" ] || continue
        name=${fw##*/}
        ln -sfn "$macws_xpc_abs/$name" "$target_dir/macOS/Frameworks/$name"
        found=1
    done
    [ "$found" -eq 1 ] || {
        echo "MacWS: no frameworks in $macws_xpc" >&2
        return 1
    }
    return 0
}

if in_chroot_readable "$webkit_canary"; then
    exit 0
fi

# The host-only symlink cannot be bind-mounted over; move it aside first.
if [ -L "$target_dir" ]; then
    if [ -L "$hostlink_backup" ] || [ -e "$hostlink_backup" ]; then
        rm "$target_dir"
    else
        mv "$target_dir" "$hostlink_backup"
    fi
    mkdir -p "$target_dir"
fi

if [ -n "$(ls -A "$target_dir" 2>/dev/null || true)" ]; then
    # Already a slim tree (or something unexpected). Try to (re)link frameworks
    # without covering a bind-mount over a nonempty directory.
    if [ -d "$target_dir/macOS/Frameworks" ] || [ -z "$(ls -A "$target_dir" 2>/dev/null || true)" ]; then
        install_macws_xpc_framework_links || exit 1
        if in_chroot_readable "$webkit_canary"; then
            exit 0
        fi
    fi
    echo "MacWS: refusing to cover nonempty $target_dir with a bind mount" >&2
    exit 1
fi

if [ -x "$mount_tool" ]; then
    "$mount_tool" "$source_dir" "$target_dir" || exit 1
    [ -x "$target_dir/$proxy_relative" ] || {
        echo "MacWS: XPC proxy is not visible after mounting $target_dir" >&2
        exit 1
    }
    if in_chroot_readable "$webkit_canary" || in_chroot_readable "$proxy_relative"; then
        exit 0
    fi
    echo "MacWS: mount_bindfs did not make /var/jb/usr readable inside the chroot" >&2
    exit 1
fi

# No bindfs (typical on this device). Populate the empty directory with the
# dual-namespace macws-xpc framework links.
install_macws_xpc_framework_links || exit 1
if in_chroot_readable "$webkit_canary"; then
    exit 0
fi
echo "MacWS: in-chroot WebKit canary is still unreadable after exposing macws-xpc" >&2
exit 1
