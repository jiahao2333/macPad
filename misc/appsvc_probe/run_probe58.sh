#!/bin/sh
# §58 device-side throwaway runner. Phases: pre | install | execkill | crash | bundle | post
# Never touches live libmachook, WebKit proxies, Safari, IconServices or
# com.macwsguide.webkit.* LaunchDaemons. No reboot. Probe names only.
PATH=/var/jb/usr/bin:/usr/bin:/bin:/usr/sbin:/sbin
M=/var/mobile/appsvc-probe
S58=$M/s58
RX=/var/mnt/rootfs/tmp/macws-appsvc-probe.xpc
LOG=/var/mnt/rootfs/tmp/macws-appsvc-probe.log
LIB=/var/mnt/rootfs/usr/local/lib/libmachook.dylib
LIB64=/var/mnt/rootfs/usr/local/lib/libmachook_arm64.dylib
ENT=/var/jb/usr/macOS/bin/entitlements.plist
hash_of() { ldid -arch "$2" -h "$1" 2>/dev/null | grep '^CDHash=' | cut -c8- | tr 'A-F' 'a-f'; }
state() {
  echo "libmachook arm64e=$(hash_of $LIB arm64e)"
  echo "libmachook arm64 =$(hash_of $LIB64 arm64)"
  echo "-- iconservices (launchctl) --"; launchctl list 2>/dev/null | grep -i iconservices || echo none
  echo "-- iconservices (ps) --"; ps -axo pid,user,comm | grep -i '[i]conservices' || echo none
  echo "-- webkit LDs --"; launchctl list 2>/dev/null | grep -i 'com.macwsguide.webkit' || echo "none loaded"
  echo "-- probe procs --"; ps -axo pid,stat,comm | grep -E 'probe_|appsvc|execkill' | grep -v grep || echo none
  echo "-- mem --"; sysctl -n vm.compressor_bytes_used 2>/dev/null; sysctl vm.page_free_count 2>/dev/null
}
trust() { h=$(hash_of "$1" "$2"); s=$(ldid -arch "$2" -h "$1" 2>/dev/null | grep 'CandidateCDHash sha1=' | awk -F= '{print $2}' | tr 'A-F' 'a-f' | head -1)
  echo "trust $1 $2 CDHash=$h sha1=$s"; [ -n "$h" ] && jbctl trustcache add "$h" >/dev/null 2>&1; [ -n "$s" ] && jbctl trustcache add "$s" >/dev/null 2>&1; true; }
cleanup() {
  pkill -9 -f "$M/probe_logstream" 2>/dev/null; pkill -9 -f "$M/probe_execkill" 2>/dev/null; pkill -9 -f "$M/probe_bclient" 2>/dev/null; pkill -9 -f "macws-appsvc-mclient" 2>/dev/null
  pkill -9 -f "$M/probe_proxy" 2>/dev/null; pkill -9 -f "$M/probe_client" 2>/dev/null
  pkill -9 -f 'macws-appsvc' 2>/dev/null; pkill -9 -f 'suspended-never-runs' 2>/dev/null
  pkill -9 -f 'macws-appsvcb' 2>/dev/null; true; }
case "$1" in
pre|post) state ;;
install)
  cat > $S58/ent58.plist <<'P'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>task_for_pid-allow</key><true/><key>com.apple.system-task-ports</key><true/>
<key>get-task-allow</key><true/><key>platform-application</key><true/>
<key>com.apple.private.logging.admin</key><true/><key>com.apple.private.logging.diagnostic</key><true/>
<key>com.apple.private.logging.stream</key><true/>
</dict></plist>
P
  cp -f $S58/probe_logstream $M/probe_logstream; ldid -S$ENT -M $M/probe_logstream; ldid -S$S58/ent58.plist -M $M/probe_logstream; chmod 755 $M/probe_logstream; trust $M/probe_logstream arm64e
  cp -f $S58/probe_crun $M/probe_crun; ldid -S$ENT -M $M/probe_crun; chmod 755 $M/probe_crun; trust $M/probe_crun arm64e
  cp -f $S58/macws-appsvc-probe.xpc/macws-appsvc-mclient $M/macws-appsvc-probe.xpc/macws-appsvc-mclient; ldid -S$ENT -M $M/macws-appsvc-probe.xpc/macws-appsvc-mclient; trust $M/macws-appsvc-probe.xpc/macws-appsvc-mclient arm64
  cp -f $S58/probe_execkill $M/probe_execkill; cp -f $S58/probe_bclient $M/probe_bclient
  mkdir -p $M/b/macws-appsvcb.xpc
  cp -f $S58/macws-appsvcb.xpc/macws-appsvcb $M/b/macws-appsvcb.xpc/macws-appsvcb
  cp -f $S58/macws-appsvcb.xpc/Info.plist $M/b/macws-appsvcb.xpc/Info.plist
  cp -f $S58/macws-appsvc-probe.xpc/macws-appsvc-probe $M/macws-appsvc-probe.xpc/macws-appsvc-probe
  ldid -S$ENT -M $M/probe_execkill && ldid -S$S58/ent58.plist -M $M/probe_execkill
  ldid -S$ENT -M $M/probe_bclient
  ldid -S$ENT -M $M/b/macws-appsvcb.xpc/macws-appsvcb
  ldid -S$ENT -M $M/macws-appsvc-probe.xpc/macws-appsvc-probe
  chmod 755 $M/probe_execkill $M/probe_bclient $M/b/macws-appsvcb.xpc/macws-appsvcb $M/macws-appsvc-probe.xpc/macws-appsvc-probe
  chmod 755 $M/b $M/b/macws-appsvcb.xpc; chmod 644 $M/b/macws-appsvcb.xpc/Info.plist
  trust $M/probe_execkill arm64e; trust $M/probe_bclient arm64e; trust $M/b/macws-appsvcb.xpc/macws-appsvcb arm64e
  trust $M/macws-appsvc-probe.xpc/macws-appsvc-probe arm64
  mkdir -p $RX; cp -f $M/macws-appsvc-probe.xpc/macws-appsvc-probe $RX/macws-appsvc-probe; chmod 755 $RX/macws-appsvc-probe
  cp -f $M/macws-appsvc-probe.xpc/macws-appsvc-mclient $RX/macws-appsvc-mclient; chmod 755 $RX/macws-appsvc-mclient
  echo "rootfs copy arm64 CDHash=$(hash_of $RX/macws-appsvc-probe arm64)"
  ls -la $M $M/b/macws-appsvcb.xpc $RX ;;
execkill)
  cleanup; rm -f $LOG
  for mode in nochroot-suspend chroot-suspend nochroot-exec; do
    echo "===== mode $mode ====="
    $M/probe_execkill $mode > $M/execkill-$mode.out 2>&1 &
    P=$!
    sleep 4.5
    echo "-- target state (pid $P) --"; ps -o pid,stat,comm -p $P 2>/dev/null || echo "(gone)"
    kill -9 $P 2>/dev/null; sleep 0.3
    wait $P 2>/dev/null; echo "wait_rc=$?"
    sleep 6
  done
  cleanup; echo "---- log ----"; cat $LOG ;;
crash)
  cleanup; rm -f $LOG /var/mnt/rootfs/tmp/macws-appsvc-probe.name
  $M/probe_proxy > $M/proxy58.out 2>&1 &
  P=$!; sleep 3; kill -0 $P 2>/dev/null && echo alive || { wait $P; echo "proxy wait_rc=$?"; }
  cleanup; echo "---- log ----"; cat $LOG ;;
bundle)
  cleanup; rm -f $LOG
  $M/probe_logstream 14 "/usr/libexec/xpcproxy|/sbin/launchd|macws-appsvcb|amfid|sandboxd|EX_CONFIG" > $M/logstream.out 2>&1 &
  LS=$!; sleep 1.5
  BP=${2:-$M/b/macws-appsvcb.xpc}
  rm -f $M/force8019; [ "$4" = "f8019" ] && touch $M/force8019
  RLT=${5:-dispatch_main}
  sed "s#<string>dispatch_main</string>#<string>$RLT</string>#" $S58/macws-appsvcb.xpc/Info.plist > $M/b/macws-appsvcb.xpc/Info.plist
  grep -A1 RunLoopType $M/b/macws-appsvcb.xpc/Info.plist | tail -1
  if [ "$BP" != "$M/b/macws-appsvcb.xpc" ]; then
    rm -rf "$BP"; mkdir -p "$BP"; cp -f $M/b/macws-appsvcb.xpc/macws-appsvcb $M/b/macws-appsvcb.xpc/Info.plist "$BP"/
    chown -R root:wheel "$(dirname $BP)"; chmod 755 "$BP" "$BP/macws-appsvcb"
    if [ "$6" = "mobile" ]; then cp -f "$BP/macws-appsvcb" "$BP/macws-appsvcb.priv"; chown root:wheel "$BP/macws-appsvcb.priv"; chmod 4755 "$BP/macws-appsvcb.priv"; touch $LOG; chmod 666 $LOG; fi
    ls -la "$BP"
  fi
  if [ "$3" = "macnomirror" ]; then
    rm -rf /var/mnt/rootfs/private/var/jb/tmp
    $M/probe_crun /tmp/macws-appsvc-probe.xpc/macws-appsvc-mclient "$BP" > $M/bclient.out 2>&1 &
  elif [ "$3" = "mac" ]; then
    RB=/var/mnt/rootfs/private$BP; [ "${BP#/private}" != "$BP" ] && RB=/var/mnt/rootfs$BP
    RB=/var/mnt/rootfs/private${BP#/private}
    rm -rf "$RB"; mkdir -p "$RB"; cp -f "$BP"/* "$RB"/; echo "in-chroot mirror $RB"; ls -la "$RB"
    $M/probe_crun /tmp/macws-appsvc-probe.xpc/macws-appsvc-mclient "$BP" > $M/bclient.out 2>&1 &
  elif [ "$6" = "mobile" ]; then
    sudo -u mobile $M/probe_bclient "$BP" > $M/bclient.out 2>&1 &
  else
  $M/probe_bclient "$BP" > $M/bclient.out 2>&1 &
  fi
  B=$!
  sleep 1.5; echo "-- launchctl print pid/$B/com.macwsguide.probe.appsvcb (after add_bundle) --"
  launchctl print pid/$B/com.macwsguide.probe.appsvcb 2>&1 | head -70
  sleep 3; echo "-- launchctl print pid/$B/com.macwsguide.probe.appsvcb (after send) --"
  launchctl print pid/$B/com.macwsguide.probe.appsvcb 2>&1 | head -70
  wait $B; echo "bclient_rc=$?"; sleep 1
  echo "-- leftover --"; ps -axo pid,ppid,stat,user,comm | grep -E 'appsvc|xpcproxy' | grep -v grep || echo none
  wait $LS 2>/dev/null; echo "---- logstream ----"; cat $M/logstream.out | cut -c1-600 | head -150
  rm -f $M/force8019; rm -rf /var/mnt/rootfs/private/var/jb/tmp; cleanup; echo "---- log ----"; cat $LOG
  echo "---- launchd.log ----"; grep -a "appsvcb" /private/var/log/com.apple.xpc.launchd/launchd.log | tail -25 | cut -c1-300 ;;
*) echo "usage: $0 pre|install|execkill|crash|bundle|post" ;;
esac
