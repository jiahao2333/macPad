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

6. **Safari：rootfs 里缺 App cryptex（2026-11-09）**
   `rootfs/Applications/Safari.app` 指向 `../System/Cryptexes/App/...`，但当初只解了 OS cryptex，
   `System/Volumes/Preboot/Cryptexes/App` 根本不存在，Dock 里的 Safari 一直是问号。
   修复：从 `UniversalMac_13.4_22F66_Restore.ipsw` 取出 App cryptex `096-09661-085.dmg`（BuildManifest
   `Cryptex1,AppOS`，约 13 MB / 1637 个文件），只读挂载后 tar 到
   `/var/mnt/rootfs/System/Volumes/Preboot/Cryptexes/App`，并且 **`chown -R root:wheel`**
   （否则 admission 报 "app executable must be a root-owned regular file"）。之后
   `macws_control_probe launch-path /Applications/Safari.app` → admission added 3 → `window-ready`，
   Safari 窗口（起始页 / What's New）正常显示。网页加载还没验证（OSXvnc 的键盘输入进不去）；
   上游 WebKitChrootProxy README 记录过 WebContent 会遇到 JSC Gigacage reservation failure。
   Dock 里的 Safari 图标仍是缓存的问号，点击无效；需要用控制中心的路径启动。
   回滚：`rm -rf /var/mnt/rootfs/System/Volumes/Preboot/Cryptexes/App`。

7. **System Settings 右侧 pane 空白（已解决）**
   窗口和侧边栏正常，但每个 pane extension 都在 exec 时被 iPadOS 16.5 的 Sandbox.kext 杀掉。
   **看日志的方法（iPad 上不用装任何东西）**：把 `/var/db/diagnostics`（最新的 Persist/Special tracev3、
   `timesync`、`version.plist`）和 `/var/db/uuidtext` 拷到 Mac，拼成 `x.logarchive`
   （补一个 `Info.plist`，`OSArchiveVersion=4`），再用 `/usr/bin/log show --archive x.logarchive`
   （zsh 里要写全路径，`log` 是 zsh 内建命令）。logd 有缓冲，最近几十秒的日志可能还没落盘。
   没有 USB / 无线配对时 `idevicesyslog` 连不上。
   完整链路（runningboardd + launchd + kernel）：
   ```
   runningboardd  Executing launch request for xpcservice<com.apple.Appearance-Settings.extension([anon<System Settings>:PID])>
   runningboardd  Submitting extension overlay ... ProgramArguments=/System/Library/ExtensionKit/Extensions/....appex/Contents/MacOS/...
   launchd        [.../com.macwsguide.settings-extension-carrier.<id>] Successfully spawned SettingsExtensionProxy[N]
   kernel         (Sandbox) Sandbox: hook..execve() killing com.apple.Appearance-Settings.extension[pid=N, uid=501]: (err=2) failed to set container
   launchd        exited with exit reason (namespace: 9 code: 0x8) - OS_REASON_EXEC   (EXEC_EXIT_REASON_SECURITY_POLICY)
   ```
   也就是说，MacWSCatalystLaunch（runningboardd 里的 hook）、carrier 和 setuid proxy 都正常；被杀的是
   proxy chroot 之后 `execve()` 真正的 macOS appex 这一步。原因是 Sandbox 的 execve hook 要给新映像设置
   launchd/xpcproxy 下发的 container。
   - 试验 1：在 chroot 里镜像出 carrier 的 container 路径（`rootfs/private/var/mobile/Containers/Data/Application/<UUID>`）→ 没用，已撤回。
   - 试验 2：把 Appearance 的签名 identifier 改成 carrier 的
     `com.macwsguide.settings-extension-carrier.com.apple.Appearance-Settings.extension`
     → container 这一步**通过了**，接着报
     `Sandbox: hook..execve() killing ...: (err=2) failed to set executable path`。已撤回
     （而且 `ensure_settings_extensions_runtime.sh` 下次启动 Settings 时会把 identifier 改回去）。
   - **已解决（2026-11-09 21:48 CST）**：真正的根因是 chroot 之后 Sandbox 在进程命名空间里解析
     两条**宿主机**路径，均返回 ENOENT：
     1. 可执行文件的 realpath（`/var/mnt/rootfs/System/...`）→ `failed to set executable path`
     2. 该 CS identifier 在 MobileContainerManager 里的 PluginKitPlugin 容器
        （`/var/mobile/Containers/Data/PluginKitPlugin/<UUID>`）→ `failed to set container`
     修复（`layout/usr/macOS/bin/ensure_chroot_sandbox_paths.sh`，由 `macos_gui.sh` 的
     `ensure_chroot_works` 与 `ensure_settings_extensions_runtime.sh` 调用）：
     - `ln -s / /var/mnt/rootfs/private/var/mnt/rootfs`（chroot 内 `/var/mnt/rootfs/...` 落到 `/...`）
     - 把 Settings 相关的 PluginKitPlugin 容器镜像进
       `$ROOTFS/private/var/mobile/Containers/Data/PluginKitPlugin/`
     Appearance / General / Wallet / Headphone 等 pane 进程在修复后保持存活，Appearance UI
     （Light/Dark/Auto、强调色等）正常渲染。截图：`~/.ipadinv/work/shots/settings-pane-rendered-20261109-2149.png`。
   - Option A（MacWSCatalystLaunch overlay 改 ServiceType=System / 去掉 ServiceType /
     去掉 JoinExistingSession）在 runningboardd 重启后**无效**：杀进程点是 CS identity + chroot
     路径，不是 launchd ServiceType。策略旋钮仍留在 Tweak 里作诊断，默认 0。
   - 回滚：`rm -f /var/mnt/rootfs/private/var/mnt/rootfs`；
     `rm -rf /var/mnt/rootfs/private/var/mobile/Containers/Data/PluginKitPlugin`。

8. **Dock 问号 / 点击无反应（已解决，2026-11-09 22:20 CST）**
   - 现象：Dock 里 4 个 "?"（Safari、Keynote、Numbers、Pages），点了 iOS 侧没有任何日志。
   - 点击链路本身是好的：Dock → `LSOpenFromURLSpec`（libmachook 拦截）→ macwshostd `launch-path`
     → code admission → window-ready。Notes / Calendar / Safari 从 Dock 点击都能正常打开。
   - 根因：Ventura Dock 只在启动时解析一次 `persistent-apps` 的 tile。"?" tile 的 bundle 当时在 chroot 里
     不存在，Dock 不会对它调用 `LSOpenFromURLSpec`，所以没有任何启动请求：
     - Keynote / Numbers / Pages：Dock 默认模板里自带，但这个 rootfs 没装。
     - Safari：tile 指向 `/System/Cryptexes/App/...`，tile 建立时 App cryptex 还没装（第 6 条）。
   - 修复（通用，不按 app 写死）：`layout/usr/macOS/bin/macws_dock_tiles.py` + `macos_gui.sh` 的
     `sanitize_dock_tiles`（在 Dock 加载前，`verify_preferences_persistence` 末尾执行）：
     - 用 stock `defaults export/import com.apple.dock` 走 cfprefsd 读写。
     - 只删除 file URL 在 chroot 里解析不到的 file-tile（按 chroot 语义跟随符号链接，绝对链接从 rootfs 根重新解析）。
     - 存在的 tile 原样保留；Dock 重新加载时会重新解析它们，所以后装的 app（比如 Safari）会自己恢复。
     - 失败只记录日志，不影响启动。
     - 本次现场：删掉 Keynote/Numbers/Pages，`launchctl unload/load` Dock 后 Safari 图标恢复。
   - 新装的 app：拖进 Dock 时 bundle 已经存在，正常解析；以后被删除的 app 会在下一次会话启动时被清掉。
   - 回滚：`defaults import` 备份 `/var/mobile/macws-rollback/com.apple.dock.plist.pre-docktiles-*`；
     `macos_gui.sh` 备份 `macos_gui.sh.pre-docktiles-*`。
   - Safari 另外还有 `Sandbox: System Policy: Safari deny(1) mach-lookup com.apple.WebKit.WebContent`，
     网页内容进程起不来（上游 WebKitChrootProxy 实验已知的问题，未修）。

9. **Settings > General 行图标空白（已解决）**
   - libmachook 的 settings symbol raster 兼容层（ISGraphicSymbolResource / CUI vector glyph）原来只装在
     `System Settings` 和 `iconservicesagent` 两个进程里。General 的行图标是 pane 进程（GeneralSettings.appex）
     自己画的，所以侧边栏有图标、行图标是空白方块。
   - 修复：`macws_process_needs_settings_symbol_raster()` 按路径覆盖所有 Settings pane appex
     （`/System/Library/ExtensionKit/Extensions/*.appex` 与 `System Settings.app/Contents/PlugIns/*.appex`），
     不按 bundle 名写死。修复后 About、Software Update、Storage 等行图标正常显示；Time Machine 行和侧边栏的
     Siri & Spotlight 仍为空（不是同一路径，未修）。
   - 旁注：macOS `iconservicesagent` 在 CoreImage 需要编译 Metal/MPS kernel 时会 abort
     （`MPSKey_Create ... Unable to get MPS kernel`），launchd 会自动拉起。给它加 `MACWS_AGX_NATIVE` 没有效果，已撤回。
   - 回滚：`/var/mobile/macws-rollback/libmachook-pre-paneicons-20261109-223118`（4 个路径，见 NOTES.txt）。

10. **装新软件后 extension 还能不能用**：`ensure_chroot_sandbox_paths.sh` 改成镜像**全部** PluginKitPlugin 容器骨架，
    不再只镜像 Settings 白名单。首次运行约 35 s（741 个），之后已存在的跳过，约 0 s。新 extension 的宿主容器由
    iOS 在第一次启动时创建；下一次走 hostd 启动（会运行 ensure）就会被镜像进 chroot。

11. **iconservicesagent 反复崩溃：CoreImage 拼接 + MPS（已解决，2026-11-09 23:50 CST）**
   - 现象：未缓存的图标（带浮雕/阴影效果）一渲染，agent 日志先出现
     `Metal library creation failed: This library format is not supported on this platform`，
     接着 `MPSLibrary::MPSKey_Create ... Unable to get MPS kernel F11H. Compiler encountered an internal error`，
     SIGABRT，launchd 每 10 s 重启。
   - 根因 1（拼接）：CoreImage 用 `newLibraryWithStitchedDescriptorSPI:`（DAG 请求，discriminator 0xe）。
     `MTLCompilerBypassOSCheck` 里给 stitch 提供 Catalyst target context 的 hook 只认
     libGPUCompilerImpl UUID `41d2f061…`；本机 20F66 是 `d499f499-45eb-388b-8f93-f6a1982fd35f`，
     日志 `Catalyst DAG target context: unsupported compiler ABI; original retained`，于是返回 iOS MTLB（01 00 / 82），被 macOS Metal 拒收。
     dlsym 探测导出符号 `getDefaultTargetTriple(Optional<PlatformType>)` 的序言与原 profile 完全一致 → 加入白名单。
   - 根因 2（MPS）：CoreImage 把模糊节点交给 `MPSGaussianBlur`，它从 metal2metal MPSImage companion 特化（0xf）。
     诊断版 tweak 截获被 `%{private}` 隐藏的 LLVM 错误：`Unknown attribute kind (71) (Producer: 'LLVM16.0.0' Reader: 'LLVM ')`。
     设备上装着的 companion 是 `342738…`（16.2 的原生输出，与 upstream LLVM16 字节一致），16.5 的 AIR reader 不认；
     用本机 Apple libLLVM（macws-llvm-as）重新生成得 `8803da…`。由于允许列表里任何一个哈希都会走"已安装"快速路径，
     旧产物不会自动重建 → `ensure_metal2metal_compat.sh` 在 20F66 上只接受 `8803da…`。
   - 结果：拼接和 MPS 都开着（没有禁用任何 CI 功能）；清空 icon store + agent Metal cache 后整套图标重建，
     agent pid 稳定、0 条 MPS/format 错误；Siri & Spotlight 图标正常。Time Machine 行图标仍空白（另一条路径）。
   - 诊断：`/tmp/macws_mtlcompiler_diagnostics` 时 tweak 额外把 `LLVM ERROR` 的字符串参数写进 `mtl_compiler_patch.log`；
     libmachook `MACWS_RUNTIME_DIAGNOSTICS` 时记录 `newLibraryWithDAG` 结果。生产路径不变。
   - iOS 上 `com.apple.system.logging.plist` 的 `Enable-Private-Data` 不生效（重启 logd 也不行），已删除恢复原状。

12. **联网探测（2026-11-10 00:05 CST）**
   - **chroot CLI**
     - DNS：chroot 内 `curl` 解析 `example.com` / `www.baidu.com` / `www.apple.com` 正常（无需 SOCKS；本机未起 1082）。
     - HTTP：`curl http://example.com` → 200。
     - HTTPS：默认 SecureTransport 报 `SSL certificate problem: Couldn't understand the server certificate format`（Keychain/Security 在 chroot 不可用，与 AGENTS 一致）。
       用系统 bundle 即可：`curl --cacert /etc/ssl/cert.pem https://…` → `example.com` / `baidu.com` HTTP/2 或 1.1 200。
     - 注意：`run_bash.sh` 经 ssh/expect 传参时，远端 shell 会拆分未整体加引号的 `;`；整段命令需作为**一个** argv。
   - **Safari GUI**
     - 窗口与起始页正常：Favorites / Privacy Report / Siri Suggestions / iCloud「来自 Mac Pro」标签同步都在（说明部分网络与账户通路可用）。
     - 用 `macws_control_probe open-documents /Applications/Safari.app /tmp/*.webloc` 能把 URL 交给 Safari（`open-web-url` 只走 VS Code，不适用）。
     - VNC 键盘仍进不去地址栏（已知）。
     - **网页仍打不开**：dmesg 实机复现
       `System Policy: Safari(PID) deny(1) mach-lookup com.apple.WebKit.WebContent`
       与 `…Networking`。进程表里没有 macOS WebContent；只有 iOS MobileSafari 的 WebKit XPC。
     - 仓库里 `WebKitChrootProxy/` 是未合入的实验（README：曾拉起原生 WebContent 后撞上 JSC Gigacage）。
       **下一步真修**：按 DockHelper/GeodProxy 同一 freestanding chroot-proxy 路径启用并接线；不要靠关沙盒糊弄。Gigacage 是后续问题。
   - **Weather GUI**：侧边栏多个国内城市温度/晴雨已刷新（网络数据到达）；右侧详情仍是 `--`（未完全可用）。`com.apple.geod`（macOS）在跑，GeodProxy 已安装。
   - 截图：`~/.ipadinv/work/shots/net-safari-start.png`、`net-weather-front.png`（亦在 box `/workspace/net-*.png`）。

