#!/bin/bash
# macOS trustd + securityd for TLS inside the Ventura chroot (field notes
# §61.6, §62). Without them every HTTPS load in Safari/WebKit fails with
# "Failed to talk to trustd" / -1202 / -9807.
#
# Invoke through bash (this jailbreak's AMFI refuses shebang execve):
#   bash macws_macos_trust_services.sh --start
#       Every GUI start (macos_gui.sh). Re-admits the reboot-volatile CDHashes
#       of the already-signed rootfs trustd/securityd and of their launch
#       images (HIServicesProxy, MacWSXPCProxy), computed at runtime from the
#       files on disk, then bootstraps com.macwsguide.{trustd,securityd} into
#       the system domain unless they already run. Never re-signs anything.
#   bash macws_macos_trust_services.sh --install
#       postinst.sh. Gives a stock rootfs trustd/securityd the project
#       signature once (macws_admit_rootfs_binary.sh: backup in
#       /var/mobile/macws-rollback, thin arm64e, project + native
#       entitlements), then admits the CDHashes. Does not touch launchd.
#   bash macws_macos_trust_services.sh --status
#       Read-only report.
# Exit status: 0 = everything ready, 1 = something failed (already logged).
# Callers treat failure as non-fatal: the desktop still starts, only TLS breaks.
PATH=/var/jb/usr/bin:/usr/bin:/bin:/usr/sbin:/sbin
ROOTFS=${MACWS_ROOTFS:-/var/mnt/rootfs}
DAEMONS=/var/jb/usr/macOS/LaunchDaemons
ADMIT=/var/jb/usr/macOS/bin/macws_admit_rootfs_binary.sh
LOG=${MACWS_TRUST_SERVICES_LOG:-/var/mobile/macws-trust-services.log}
MARKER='<key>com.apple.private.graphics-restart-no-kill</key>'
HISERVICES_PROXY=/var/jb/usr/macOS/Frameworks/HIServices.framework/Versions/A/XPCServices/HIServicesProxy.xpc/HIServicesProxy
XPC_PROXY=/var/jb/usr/macOS/libexec/MacWSXPCProxy
# label:chroot target (must equal MACWS_XPC_TARGET in the LaunchDaemon plist)
SERVICES="com.macwsguide.trustd:/usr/libexec/trustd com.macwsguide.securityd:/usr/sbin/securityd"
WAIT_SECONDS=${MACWS_TRUST_WAIT:-10}

mode=${1:---start}
status=0
TC=""

log() {
    local line
    line="[$(date '+%Y-%m-%d %H:%M:%S %Z')] macos-trust: $*"
    printf '%s\n' "$line"
    printf '%s\n' "$line" >> "$LOG" 2>/dev/null || true
}

refresh_trustcache() {
    TC=$(jbctl trustcache info 2>/dev/null | tr '[:upper:]' '[:lower:]')
}

# arm64e/arm64 CDHashes of one Mach-O (fat or thin), read from the file now.
cdhashes_of() {
    local path="$1" arch h
    for arch in arm64e arm64; do
        h=$(ldid -arch "$arch" -h "$path" 2>/dev/null | sed -n 's/^CDHash=\([0-9a-f]*\).*/\1/p' | head -1)
        [ -n "$h" ] && printf '%s %s\n' "$arch" "$h"
    done
}

admit_hashes() {
    local path="$1" arch h found=0
    [ -f "$path" ] || { log "skip trust: missing $path"; return 0; }
    while read -r arch h; do
        [ -n "$h" ] || continue
        found=1
        if printf '%s\n' "$TC" | grep -qi "$h"; then
            log "trust present $arch $h $path"
        elif jbctl trustcache add "$h" >/dev/null 2>&1; then
            TC="$TC
$h"
            log "trust added   $arch $h $path"
        else
            log "ERROR: jbctl trustcache add $h failed for $path"
            status=1
        fi
    done <<HASHES
$(cdhashes_of "$path")
HASHES
    if [ "$found" -eq 0 ]; then
        log "ERROR: no arm64/arm64e CodeDirectory in $path"
        status=1
    fi
}

project_signed() {
    ldid -e "$1" 2>/dev/null | grep -Fq "$MARKER"
}

job_pid() {
    launchctl list "$1" 2>/dev/null |
        sed -n 's/^[[:space:]]*"PID" = \([0-9][0-9]*\);.*/\1/p' | head -1
}

ensure_job() {
    local label="$1" target="$2" plist pid waited=0 out
    plist="$DAEMONS/$label.plist"
    [ -f "$plist" ] || { log "ERROR: missing $plist"; status=1; return; }
    grep -Fq "<string>$target</string>" "$plist" ||
        log "WARN: $plist does not name MACWS_XPC_TARGET $target"
    pid=$(job_pid "$label")
    if [ -n "$pid" ] && [ "$pid" -gt 0 ] 2>/dev/null; then
        log "$label already running pid=$pid (skip)"
        return
    fi
    if launchctl list "$label" >/dev/null 2>&1; then
        # Loaded without a process (e.g. it died on a stale trustcache before
        # this boot's admission). Drop the stale job, then bootstrap fresh.
        log "$label loaded without pid; removing stale job"
        launchctl remove "$label" >/dev/null 2>&1 || true
        sleep 1
    fi
    # Root must target the system domain explicitly: a plain `launchctl load`
    # from an SSH shell can land in user/501, where the job dies with
    # OS_REASON_EXEC (field notes §61.6).
    if ! out=$(launchctl bootstrap system "$plist" 2>&1); then
        log "launchctl bootstrap system $label: ${out:-failed}; trying load"
        launchctl load "$plist" >/dev/null 2>&1 || true
    fi
    while [ "$waited" -lt "$WAIT_SECONDS" ]; do
        pid=$(job_pid "$label")
        [ -n "$pid" ] && [ "$pid" -gt 0 ] 2>/dev/null && break
        sleep 1
        waited=$((waited + 1))
    done
    if [ -n "$pid" ] && [ "$pid" -gt 0 ] 2>/dev/null; then
        log "$label started pid=$pid after ${waited}s"
    else
        log "ERROR: $label has no pid after ${WAIT_SECONDS}s (see /var/jb/var/mobile/${label#com.macwsguide.}-macos.log)"
        status=1
    fi
}

do_trust() {
    local entry target
    refresh_trustcache
    [ -n "$TC" ] || log "WARN: jbctl trustcache info returned nothing"
    for entry in $SERVICES; do
        target=${entry#*:}
        if [ ! -f "$ROOTFS$target" ]; then
            log "ERROR: rootfs target missing: $ROOTFS$target"
            status=1
            continue
        fi
        if ! project_signed "$ROOTFS$target"; then
            log "ERROR: $ROOTFS$target lacks the project signature; run postinst.sh (or this script with --install)"
            status=1
            continue
        fi
        admit_hashes "$ROOTFS$target"
    done
    admit_hashes "$HISERVICES_PROXY"
    [ ! -f "$XPC_PROXY" ] || admit_hashes "$XPC_PROXY"
}

case "$mode" in
--start)
    log "start (rootfs=$ROOTFS)"
    do_trust
    for entry in $SERVICES; do
        ensure_job "${entry%%:*}" "${entry#*:}"
    done
    ;;
--install)
    log "install (rootfs=$ROOTFS)"
    for entry in $SERVICES; do
        target="$ROOTFS${entry#*:}"
        [ -f "$target" ] || { log "skip install: missing $target"; continue; }
        if project_signed "$target"; then
            log "already project-signed: $target"
        elif [ -f "$ADMIT" ] && bash "$ADMIT" "$target"; then
            log "admitted (re-signed, backup in /var/mobile/macws-rollback): $target"
        else
            log "ERROR: macws_admit_rootfs_binary.sh failed for $target"
            status=1
        fi
    done
    do_trust
    ;;
--status)
    refresh_trustcache
    for entry in $SERVICES; do
        target="$ROOTFS${entry#*:}"
        signed=no; project_signed "$target" && signed=yes
        echo "${entry%%:*} target=$target project_signed=$signed pid=$(job_pid "${entry%%:*}")"
        cdhashes_of "$target" | while read -r arch h; do
            printf '  %s %s trusted=%s\n' "$arch" "$h" \
                "$(printf '%s\n' "$TC" | grep -qi "$h" && echo yes || echo no)"
        done
    done
    exit 0
    ;;
*)
    echo "usage: bash $0 [--start|--install|--status]" >&2
    exit 2
    ;;
esac
[ "$status" -eq 0 ] && log "done: ready" || log "done: with errors"
exit "$status"
