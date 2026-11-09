# 实机记录：iPad14,5 (M2, 8 GB) / iPadOS 16.5 20F66 / Dopamine rootless / macOS 13.4 22F66 rootfs

来源：2026-11-08/09 的实机安装会话。基线：DCMMC/macPad `f7bb45e`（2026-10-21）。
以下内容均为设备上实际观察到的结果；未经验证的推测会明确标注为“假设”。
参考：[DCMMC/macPad issue #1](https://github.com/DCMMC/macPad/issues/1)。

## 设备画像

- `MTLCompilerService` UUID `B5CBF457-B300-3FD0-A646-1261DA6E86B0`（SHA-256 与仓库里 20F75 的 profile 完全一致），
  SpringBoard UUID `3EF69ACA-1D05-380B-A35F-532C8DC50EDB`，`AGXMetalG14G` 227.6.3。
  因此 20F66 直接复用 20F75 的 adapter 和 0x78 AGX device-info negotiate。
- rootfs 位于 `/var/mnt/rootfs`（约 24 GiB，424295 个条目），`System/Volumes/Data` symlink 需手动设置（`ln -sfn ../..`）。

## 遇到的问题与修复（按出现顺序）

1. **chroot 首次 exec 被 SIGKILL**
   修复：用项目 entitlements 重新签名（`ldid -S/var/jb/usr/macOS/bin/entitlements.plist -M`，签两次，先复制到新 inode），
   再 `jbctl trustcache add`；同时按 macPad 的做法建立 `.jbroot` 和 `/var/jb/usr` symlink。对应 issue #1 第 9 条。

2. **缺少 `OSXvnc-server`**（即使用 `--no-vnc`，它仍作为 localhost pointer proxy 被需要）
   在 macOS 主机上从 `stweil/OSXvnc` 构建：用自带的 libjpeg-turbo 编出静态 `libturbojpeg.a`，
   用 `vncauth.c` + `d3des.c` 编出 `libvncauth.a`，8 个源文件以 `-x objective-c` 编译，补上 `getMACAddress.o`，
   `-mmacosx-version-min=13.0`，最后用 entitlements profile 签名。安装到 `/var/mnt/rootfs/usr/local/bin/OSXvnc-server`。
   对应 issue #1 第 10 条。

3. **Terminal 没通过 2 s readiness gate，导致整个桌面被拆掉**
   修复：用 entitlements profile 重新签名 Terminal。

4. **System Settings admission 报 "dependency escapes rootfs"**
   AppPluginTrust 的 `closure()` 会对每个依赖做 realpath；CydiaSubstrate 和 `@rpath` 的 `libswiftCore`
   经由 `rootfs/var/jb/{Library,usr}` symlink 解析到 `/var/jb/...`，于是“逃出” rootfs。
   - **Substrate 的修复**：把 `/var/jb/usr/lib/libellekit.dylib` 复制一份，用 `set_macos_version.py` 改成 macOS platform，
     **必须带 entitlements profile 签名**，安装到
     `System/Library/Frameworks/CydiaSubstrate.framework` 和
     `usr/local/lib/.jbroot/Library/Frameworks/CydiaSubstrate.framework`。
     只用 `ldid -S -M`（不带 entitlements）签的副本会被 AMFI 静默杀掉：SystemUIServer / ControlCenter 一启动就死，
     而且不留 `.ips` 崩溃报告。对应 issue #1 第 6 + 9 条。
   - **不要**把 `rootfs/var/jb/usr` 或 `var/jb/Library` 改成真实目录树。`ensure_jb_usr_bind.sh` 要求
     `rootfs/var/jb/usr` 只能是以下三种之一：
     1. 精确指向 `/var/jb/usr` 的 symlink（且 DockHelperProxy XPC 存在）；
     2. 已存在的 mount；
     3. **空**目录（脚本随后用 `mount_bindfs` 挂载）。
     非空目录会报 "refusing to cover nonempty ... with a bind mount"，启动中止。
   - 空目录方案依赖 `/var/jb/usr/local/bin/mount_bindfs`，macPad 不自带，也没声明为依赖，
     否则报 "MacWS: mount_bindfs is unavailable"。issue #1 的报告者装了 `bindfs-tools 1.0`。
     在 Dopamine 上目前唯一找到的候选是 `apt.cydiakk.bindfs-rootless` 0.5.5-1（<https://apt.cydiakk.com/>），**尚未审查**。
   - 当前可用状态：symlink → `/var/jb/usr`；System Settings 的 libswift admission 问题**已在 2026-11-09 解决**（不需要 bindfs，见下）。
   - **2026-11-09 修复 1（admission）**：真正触发失败的是 `GeneralSettings.appex/Contents/Frameworks` 里那份
     libmachook 的 `LC_RPATH`：第一个 rpath 是 `/var/jb/Library/Frameworks`，`closure()` 在这个逃出 rootfs 的
     **候选**上直接 raise，而 dyld 会继续尝试下一个 rpath（`@loader_path/.jbroot/...`）。
     `macws_macho_dependencies.py` 现在对 `@rpath/` 候选里逃出 rootfs 的条目 `continue`（与 dyld 一致），
     显式绝对路径 / `@loader_path` / `@executable_path` 依赖仍然 fail closed。测试：
     `misc/test_macho_dependencies.py::test_escaping_rpath_entry_is_skipped_like_dyld`。
   - **2026-11-09 修复 2（启动即 SIGILL / SIGSEGV，exit-status=11）**：admission 通过后 System Settings 在
     `main` 调用 `CGSSessionCopyCurrentSessionProperties` 时崩溃（pc 落在 libLAPACK / MPSNeuralNetwork 的
     `udf #0` 区域）。原因：Ventura 的 `SLSSessionCopyCurrentSessionProperties` 是一条 `b` thunk，ElleKit 对
     “开头已经是 `b`”的函数只改写 imm26，导致 2.6 GB 外的替换函数被截断成一个 ±128 MB 内的随机跳转
     （实测 compat=0x102e45ecc → 跳到 0x1b2e45ecc，低 28 位相同）。libmachook 现在先跟随同 image 内的 `b` thunk，
     hook 真正的函数体 `SLSCopyCurrentSessionPropertiesInternalBridge`（hook 后为正常的 `ldr x16; br x16`）。
     `MACWS_CGSESSION_HOOK_DIAG=1` 会打印 hook 前后的指令。System Settings 必须拿到 `kCGSessionLoginDoneKey=true`，
     否则 `main` 直接返回，所以这个 hook 不能简单去掉。
   - **仍未解决**：System Settings 窗口和侧边栏能正常显示，但右侧 pane 是空的；macwshostd 报
     `system-settings content result=missing-settings-extension`（30 s 内没有任何 `com.apple.Settings.extension.ui`
     appex 进程被 ExtensionKit 拉起，日志里从未出现过 `settings-extension-ready`）。下一步查 ExtensionKit /
     ViewBridgeChrootProxy 的启动链。

5. **postinst**：新增 `ensure_cydia_substrate_rootfs()`——只在 Substrate 缺失（或不是带 entitlements 的 macOS-platform 签名）时创建，
   用 entitlements 签名，永远不改写 `var/jb/Library` 或 `var/jb/usr`。

## 当前状态

用 `macos_gui.sh watchdog coexist --no-vnc --no-terminal --experimental` 启动 GUI：Finder、Dock、
菜单栏（SystemUIServer）、ControlCenter 均可起来。

## 未解决：WindowServer 内存 → jetsam respring 循环（主要阻塞点）

- JetsamEvent 16:24 (+0800)：WindowServer rpages 约 12.7 GB；17:14：约 18.1 GB（1,160,368 × 16 KB pages），
  而设备只有 8 GB。所有记录的 reason 都是 `vm-compressor-space-shortage`。
  杀进程顺序：先 SpringBoard + backboardd（iOS 出现 respring 转圈），然后 WindowServer 本身，然后 SB/backboardd 再被反复杀（storm）。
  同时 Notes.app 约 860 MB 常驻。
- 会话早期 WindowServer 的 task_info RSS 约 131–142 MB，所以增长发生在后期，且在 plain-texture lease pool 之外。
- `MACWS-MEMORY-REAP`（`libmachook/Metal_hooks.x` 约 13452–13545 + 约 13700 的分配时 eviction）只修剪
  plain-texture IOSurface lease pool（idle budget 64 MB，busy LRU cap 256 MB，15 s timer，受 retain count 约束）。
  pool 峰值约 191 MB，整个会话一共释放约 286 MB。owned-scanout LRU 128 MB（约 12344–12416）和
  FINAL-COMPOSITE 4 个 slot（`MacWSFinalCompositePublisher.m` 约 64–110）都有上界。会话内有 85 次 final-composite replay 请求。
- 源码注释（约 13120）记录过一次更早的问题：memoryless 被错放到 IOSurface 上，涨到 "46+ GB -> 5120 MB WS watermark"；
  现在 memoryless 已改为走 native（约 13220+）。
  **假设（未验证）**：另一类 SkyLight/CoreAnimation intermediates 或 Private Metal 资源在 lease pool 之外按帧/按 replay 累积。
- watchdog 的 memory guard 按设计是关闭的（见 AGENTS.md）。
- 画布 2732×2048，coexist 模式。

详细的代码层分析见 [`windowserver-memory-investigation.md`](windowserver-memory-investigation.md)。
