#!/bin/sh
# §59 device runner (root). Phases:
#   install          sign probe binaries on the /var/mobile vnode, trust,
#                    byte-copy the app bundle into rootfs /private/tmp,
#                    generate its overlay with the real generator
#   run [root|mobile]  in-chroot client -> real interposer -> overlay ->
#                    MacWSXPCProxy (-> .priv) -> chroot -> stock xpc_main
#   cleanup          remove the probe app, its overlay and index entry
export PATH=/var/jb/usr/bin:/usr/sbin:/usr/bin:/bin:/sbin
M=/var/mobile/appsvc-probe/s59
ENT=$M/ent59.plist
APP_CHROOT=/tmp/MacWSOverlayProbe.app
APP_HOST=/var/mnt/rootfs/private/tmp/MacWSOverlayProbe.app
SVC_REL=Contents/XPCServices/com.macwsguide.probe.overlay.xpc
LOG=/var/mnt/rootfs/private/tmp/macws-appsvc-probe.log
GEN=/var/jb/usr/macOS/bin/macws_xpc_overlay_gen.py
trust() {
  h=$(ldid -arch "$2" -h "$1" 2>/dev/null | awk -F= '/^CDHash=/{print $2; exit}')
  [ -n "$h" ] && jbctl trustcache add "$h" >/dev/null 2>&1
  echo "trust $1 $2 CDHash=$h"
}
procs() { ps -axo pid,ppid,user,stat,command | grep -a -E 'MacWSOverlayProbe|MacWSXPCProxy|macws-appsvc-probe|probe_crun' | grep -v grep || echo "no probe procs"; }
case "$1" in
install)
  [ -f $ENT ] || cp /var/mobile/appsvc-probe/s58/ent58.plist $ENT
  for f in $M/probe_crun $M/MacWSOverlayProbe.app/Contents/MacOS/MacWSOverlayProbe \
           $M/MacWSOverlayProbe.app/$SVC_REL/Contents/MacOS/macws-appsvc-probe; do
    ldid -S$ENT -M "$f"; chmod 755 "$f"
  done
  trust $M/probe_crun arm64e
  trust $M/MacWSOverlayProbe.app/Contents/MacOS/MacWSOverlayProbe arm64
  trust $M/MacWSOverlayProbe.app/$SVC_REL/Contents/MacOS/macws-appsvc-probe arm64
  rm -rf $APP_HOST; mkdir -p $APP_HOST
  cp -R $M/MacWSOverlayProbe.app/. $APP_HOST/
  chown -R root:wheel $APP_HOST
  trust $APP_HOST/Contents/MacOS/MacWSOverlayProbe arm64
  python3 $GEN $APP_CHROOT/$SVC_REL
  K=$(ls -t /var/mnt/rootfs/usr/local/var/macws-xpc-overlay | head -1)
  echo "index $K: $(cat /var/mnt/rootfs/usr/local/var/macws-xpc-overlay/$K)"
  ls -la /var/jb/usr/macOS/libexec/macws-xpc-overlay/$K/*.xpc/ ;;
run)
  rm -f $LOG; touch $LOG; chmod 666 $LOG
  : > /var/mnt/rootfs/tmp/macws-webkit-proxy.log; chmod 666 /var/mnt/rootfs/tmp/macws-webkit-proxy.log
  echo "-- before: $(date '+%H:%M:%S') --"; procs
  if [ "$2" = "mobile" ]; then
    $M/probe_crun -u501 $APP_CHROOT/Contents/MacOS/MacWSOverlayProbe > $M/ovclient.out 2>&1 &
  else
    $M/probe_crun $APP_CHROOT/Contents/MacOS/MacWSOverlayProbe > $M/ovclient.out 2>&1 &
  fi
  C=$!; sleep 3
  echo "-- during: $(date '+%H:%M:%S') --"
  ps -axo pid,ppid,uid,ruid,svuid,stat,wchan,command | grep -a -E 'MacWSXPCProxy|macws-appsvc-probe|MacWSOverlayProbe' | grep -v grep
  wait $C; echo "client_rc=$?"; sleep 4
  echo "-- after: $(date '+%H:%M:%S') --"; procs
  echo "---- probe log ----"; awk '!seen[$0]++' $LOG
  echo "---- proxy log ----"; cat /var/mnt/rootfs/tmp/macws-webkit-proxy.log
  echo "---- xpc-app log (overlay) ----"; grep -a overlay /var/mnt/rootfs/tmp/macws-xpc-app.log | tail -5
  echo "---- launchd.log ----"; grep -a "com.macwsguide.probe.overlay\|MacWSXPCProxy" /private/var/log/com.apple.xpc.launchd/launchd.log | tail -14 | cut -c1-260 ;;
cleanup)
  for p in $(ps -axo pid,command | awk '/MacWSOverlayProbe|macws-appsvc-probe|MacWSXPCProxy/ && !/awk/ {print $1}'); do kill -9 $p 2>/dev/null; done
  for K in $(ls /var/mnt/rootfs/usr/local/var/macws-xpc-overlay 2>/dev/null); do
    if grep -q "com.macwsguide.probe" /var/mnt/rootfs/usr/local/var/macws-xpc-overlay/$K; then
      rm -rf /var/jb/usr/macOS/libexec/macws-xpc-overlay/$K /var/mnt/rootfs/usr/local/var/macws-xpc-overlay/$K
      echo "removed overlay $K"
    fi
  done
  rm -rf $APP_HOST $LOG; procs ;;
*) echo "usage: $0 install|run [root|mobile]|cleanup" ;;
esac
