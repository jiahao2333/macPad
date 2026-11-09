# No shebang: iPadOS AMFI rejects execve of shebang scripts in this setup.
# Invoke with: bash /var/jb/usr/macOS/bin/ensure_chroot_sandbox_paths.sh
#
# iPadOS 16.5 Sandbox.kext, at the post-chroot execve() of a macOS image,
# re-resolves two host paths inside the chroot namespace:
#   1. the process executable's real path (/var/mnt/rootfs/System/...)
#   2. any MobileContainerManager container for the image's code-signing id
# Both lookups return ENOENT unless those paths exist under the chroot root,
# which kills the process with:
#   Sandbox: hook..execve() ... (err=2) failed to set container
#   Sandbox: hook..execve() ... (err=2) failed to set executable path
# Runtime-confirmed 2026-11-09 on iPad14,5 / iPadOS 16.5 (Dopamine rootless).
# iPadOS 16.3 did not require this (upstream panes worked without it).
#
# This helper is intentionaly tiny and idempotent: every macOS GUI cold start
# and every Settings extension ensure pass can re-run it safely.

set -e

ROOTFS=/var/mnt/rootfs
HOST_PKP=/var/mobile/Containers/Data/PluginKitPlugin
CHROOT_PKP="$ROOTFS/private/var/mobile/Containers/Data/PluginKitPlugin"
ROOTFS_SELF="$ROOTFS/private/var/mnt/rootfs"

if [ ! -d "$ROOTFS/System" ]; then
    echo '[INFO] chroot sandbox paths deferred: macOS rootfs is not mounted'
    exit 0
fi

# (1) Make the host realpath /var/mnt/rootfs/... resolve after chroot(2).
# Inside the chroot, /var -> private/var, so private/var/mnt/rootfs -> /
# makes /var/mnt/rootfs/System/... land on the same vnode as /System/...
mkdir -p "$ROOTFS/private/var/mnt"
if [ -L "$ROOTFS_SELF" ]; then
    target=$(readlink "$ROOTFS_SELF" 2>/dev/null || true)
    if [ "$target" != "/" ]; then
        rm -f "$ROOTFS_SELF"
        ln -s / "$ROOTFS_SELF"
    fi
elif [ -e "$ROOTFS_SELF" ]; then
    echo "[ERROR] $ROOTFS_SELF exists and is not the expected symlink" >&2
    exit 1
else
    ln -s / "$ROOTFS_SELF"
fi

# (2) Mirror host PluginKitPlugin containers that Settings panes (and similar
# Apple settings extensions) already own. Sandbox looks the container up by
# code-signing identifier; the host path is invisible after chroot unless a
# same-named directory exists under $ROOTFS/private/var/mobile/Containers/...
mirror_one_pkp() {
    local src="$1" name dst ident
    name=$(basename "$src")
    dst="$CHROOT_PKP/$name"
    [ -d "$src" ] || return 0
    [ -f "$src/.com.apple.mobile_container_manager.metadata.plist" ] || return 0
    # Cheap identifier filter: only Apple settings-related plugin containers.
    ident=$(/var/jb/usr/bin/plutil -key MCMMetadataIdentifier \
        "$src/.com.apple.mobile_container_manager.metadata.plist" 2>/dev/null || true)
    if [ -z "$ident" ]; then
        # plutil -key may be unavailable; fall back to strings.
        ident=$(strings "$src/.com.apple.mobile_container_manager.metadata.plist" 2>/dev/null |
            grep -E '^com\.apple\.' | head -1 || true)
    fi
    case "$ident" in
        com.apple.*Settings*|com.apple.*settings*|com.apple.systempreferences.*|com.apple.*Preference*)
            ;;
        *)
            return 0
            ;;
    esac
    mkdir -p "$dst"
    for sub in Documents Library SystemData tmp; do
        mkdir -p "$dst/$sub"
    done
    cp -a "$src/.com.apple.mobile_container_manager.metadata.plist" "$dst/" 2>/dev/null || true
    chown -R mobile:mobile "$dst" 2>/dev/null || true
}

if [ -d "$HOST_PKP" ]; then
    mkdir -p "$CHROOT_PKP"
    for src in "$HOST_PKP"/*; do
        [ -d "$src" ] || continue
        mirror_one_pkp "$src"
    done
fi

echo "[INFO] chroot sandbox paths ready (rootfs self-link + settings PluginKitPlugin mirrors)"
