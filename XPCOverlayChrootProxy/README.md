# Generic Application-type XPC overlay proxy (field notes §58/§59)

`MacWSXPCProxy` is the iOS `CFBundleExecutable` of every generated overlay
bundle under `/var/jb/usr/macOS/libexec/macws-xpc-overlay/<key>/<leaf>.xpc`.
launchd/xpcproxy start it for a job in the *client's* process domain
(`xpc_add_bundle(path, 2)`, registered by libmachook's
`macws_xpc_overlay_activate`). It execs its setuid `MacWSXPCProxy.priv`
sibling when unprivileged, chroots into `/var/mnt/rootfs`, and execs the stock
macOS executable named by the root-owned `MacWSTarget` symlink, passing all
`XPC_*` through unchanged. Stock `xpc_main` then checks in with iOS launchd by
pid.

Overlays and the in-chroot index (`/usr/local/var/macws-xpc-overlay/<key>`)
are generated on the host by `layout/usr/macOS/bin/macws_xpc_overlay_gen.py`.
`<key>` is FNV-1a-64 of the bundle's chroot realpath.

Install (field deploy, not part of the aggregate package yet):
`MacWSXPCProxy` 0755 and a same-bytes `MacWSXPCProxy.priv` 4755, root:wheel,
in `/var/jb/usr/macOS/libexec`, CDHash added to the trust cache.

Kill switch: create `/var/mnt/rootfs/usr/local/etc/macws-xpc-overlay.disable`.
Rollback: delete the overlay tree and index, remove the two proxy files.
