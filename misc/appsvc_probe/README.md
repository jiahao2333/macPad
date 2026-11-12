# Application-type bring-up probe

Throwaway, generic (not Safari-only) probe of the chroot-proxy
`mach_ports_register(SERVICE_SLOT)` + `XPC_FLAGS=8019` + `chroot` +
`execve` + stock `xpc_main` path. Binaries are not part of the PR.

- `probe_proxy.c` — iOS arm64e first image: allocate a RECEIVE right,
  register it in slot 2, bootstrap-register a unique
  `com.macwsguide.probe.appsvc.<pid>` name (never
  `macosbooter.WebKit.*`), then `chroot(/var/mnt/rootfs)` + `execve`
  the test service. A macOS image exec'd this way SIGTRAPs unless the
  live chroot `libmachook` is inserted (`MallocNanoZone=0` is not
  sufficient; reproduced with stock `/bin/echo`).
- `probe_prexpc.c` — macOS 13 arm64, libSystem only. Logs ports/flags after exec before libxpc runs, then `posix_spawn(POSIX_SPAWN_SETEXEC)` of the test service with `set_registered_ports_np` (xpcproxy apply path).
- `probe_appsvc.c` — macOS 13 arm64 test service (same arch as
  `launchservicesd` / other in-chroot custom executables). Logs
  `mach_ports_lookup` / `XPC_FLAGS`, then calls stock `xpc_main`. On a
  bootstrap-shaped dictionary, runs `dispatch_sync_f(main)` and replies.
- `probe_client.c` — iOS-host client; looks up the unique name and
  sends `message-name=bootstrap`.
- `build.sh` / `run_probe.sh` — host build and device runner. The
  runner refuses to start unless live net34 libmachook CDHashes still
  match, signs only under `/var/mobile/appsvc-probe` and byte-copies
  the service to `/var/mnt/rootfs/tmp/` (no ldid on the rootfs vnode),
  and does not launch Safari or touch the WebKit install.

See field notes §56 for the 2026-11-11 run.

## §58 additions (root cause + launchd-managed proof)

- `probe_execkill.c` — iOS arm64e. Registers a RECEIVE in slot 2 plus a
  `com.macwsguide.probe.execkill.<pid>` name, spawns a watcher that arms a
  `MACH_NOTIFY_DEAD_NAME` on the looked-up send right, then (optionally
  after `chroot`) exec's via `POSIX_SPAWN_SETEXEC|START_SUSPENDED` or plain
  `execve`. Times the right's death relative to the exec and inspects the
  suspended new task with `task_for_pid` + `mach_ports_lookup`.
- `probe_shim.c` — iOS arm64e `CFBundleExecutable` of a launchd-managed
  XPCService bundle (`InfoB.plist`). Optional setuid `*.priv` hop when
  unprivileged, then `chroot(/var/mnt/rootfs)` + `execve` of the macOS
  `probe_appsvc` with incoming `XPC_*` unchanged and live libmachook inserted.
- `probe_bclient.c` — client: `xpc_add_bundle(path, 2)` +
  `xpc_connection_create` + `bootstrap` round-trip. Built twice: iOS arm64e,
  and macOS arm64 (`-DMACWS_MAC_CLIENT`, run inside the chroot).
- `probe_crun.c` — iOS launcher: `chroot` + `execve` with the minimal chroot env.
- `probe_logstream.c` — read-only `os_activity_stream` reader. Builds against
  LLVM lldb's `ActivityStreamSPI.h` (not vendored; pass `-I`).
- `build58.sh`, `run_probe58.sh` — build / device runner
  (`pre|install|execkill|crash|bundle|post`).
