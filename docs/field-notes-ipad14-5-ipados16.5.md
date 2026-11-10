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

13. **chroot HTTPS / curl 证书校验（已解决，2026-11-10 00:26 CST）**
   - 现象：plain `curl https://…` 报 `Couldn't understand the server certificate format`；`--cacert` 或 LibreSSL backend 才通。
   - 根因分层：
     1. `NSURLSession` / Network.framework TLS **正常**（baidu/example/apple 均 200，校验证书未关）。
     2. `SecCertificateCreateWithData` / `SecTrustEvaluateWithError` 对同链也正常（baidu ok=1）。
     3. 旧 SecureTransport `SSLHandshake` 恒返回 `errSSLBadCert` (-9808)，即便随后 `SSLCopyPeerTrust`+`SecTrustEvaluate` 成功。
     4. 经典 Keychain API（`SecKeychainSetSearchList` / `SecTrustCopyAnchorCertificates`）在 chroot 返回 `paramErr` (-50)；macOS `trustd` 未按 authd 同款私有名拉起（iOS trustd 在跑）。
   - 修复（不关校验）：`launchdchrootexec` 默认设置 `CURL_SSL_BACKEND=openssl` + `SSL_CERT_FILE=/etc/ssl/cert.pem`（仅当未设置时）；`ensure_chroot_ssl_defaults.sh` 写入 `/etc/curlrc`（`cacert = /etc/ssl/cert.pem`）与 root `.bashrc`；由 `macos_gui.sh ensure_chroot_works` 调用。
   - 验证：无手动 export 时 `curl -sI https://www.baidu.com|example.com|www.apple.com` 均 200；NSURLSession probe 同。
   - 回滚：`/var/mobile/macws-rollback/launchdchrootexec.pre-ssl-*`、`macos_gui.sh.pre-ssl-*`、`bashrc.pre-ssl-*`。
   - 未决：SecureTransport 本身仍 -9808（仅影响 ST 客户端；GUI 走 Network.framework）。完整修 ST 需 macOS securityd/keychain/trustd 私有名栈，另开。

## 14. HTTPS defaults + Safari WebKit transport (2026-11-10 Asia/Shanghai)

### HTTPS (done — commit `a868f70`)
- Plain `curl https://www.baidu.com` in chroot returns HTTP 200 with validation ON.
- Mechanism: `launchdchrootexec` sets `SSL_CERT_FILE=/etc/ssl/cert.pem` + `CURL_SSL_BACKEND=openssl` when unset; `/etc/curlrc` / ensure script wired from `macos_gui.sh`.
- NSURLSession/GUI already OK. SecureTransport `SSLHandshake` still `errSSLBadCert` (-9808); full securityd/trustd/keychain repair remains open (do **not** disable validation).

### Safari WebKit (partial — this commit)
Root causes found while bringing up WebKitChrootProxy:
1. Fat `libmachook` DYLD_INSERT kills Safari (need thin arm64e + arm64).
2. Cryptex WebKit XPC binaries: AMFI `unsuitable CT policy 0x8` / `Unrecoverable CT signature issue` after ldid — copy **thin arm64e** into non-cryptex `/usr/local/libexec/macws-webkit/*.xpc/Contents/...` with stock Info.plist, project+native entitlements merge, trustcache.
3. Geod-style LaunchDaemon MachServices are wrong for Application-type WebKit (`NSRunLoop` / `ServiceType=Application`): exits `An XPC Service cannot be run directly.` Use DockHelper-style `xpc_add_bundle` + `xpc_connection_create(macosbooter.*)`.
4. Proxies must be `root:wheel` setuid (mobile-owned setuid → euid mobile → chroot exit 111).

Working now: Safari launches with remapped hooks; **SandboxBroker runs inside chroot** at `macws-webkit/.../SandboxBroker`. open-documents delivers `.webloc` URLs.
Still open: macOS **WebContent/Networking/GPU** do not stay up — page chrome/load incomplete. `GIGACAGE_ENABLED=0` is only a **temporary diagnostic** in the WebKit proxy env (and was on unloaded LaunchDaemon plists); real JSC Gigacage reservation fix still required. Safari mach-lookup entitlement expansion via ldid previously SIGKILL'd Safari before AppKit window — revisit carefully.

Rollback: `/var/mobile/macws-rollback/` + NOTES.txt (libmachook pre-webkit/pre-thin, Safari.pre-webkit-*, `*.pre-webkit-resign-*`, macws-webkit copies).

## 15. System-wide SecureTransport / trustd / securityd (2026-11-10)

### Root cause
iPadOS owns public `com.apple.trustd` / `com.apple.SecurityServer`. Only iOS trustd/securityd were running; Ventura Security.framework clients in the chroot talked to the wrong stack (or none). Classic Keychain APIs returned `paramErr`; SecureTransport `SSLHandshake` returned `errSSLBadCert` (-9808) even when `SecTrustEvaluate` could succeed for some chains.

### System-level fix (not per-app)
1. Publish Ventura **trustd** and **securityd** on private Mach names `com.apple.macosbooter.trustd` / `com.apple.macosbooter.SecurityServer` via `HIServicesProxy` + `MACWS_XPC_TARGET` (same shape as authd).
2. libmachook remaps `com.apple.trustd` and `com.apple.SecurityServer` for all chroot processes.
3. securityd hit AMFI `unsuitable CT policy 0x8` under freestanding chroot+exec → thin arm64e + ldid merge + trustcache (generic admit pattern; backup in macws-rollback).

### Verification
- `CURL_SSL_BACKEND=secure-transport curl -sI https://www.baidu.com` → HTTP 200, exit 0
- Same for example.com / apple.com
- Both `com.macwsguide.trustd` and `com.macwsguide.securityd` launchd jobs `state=running`

### a868f70 curl LibreSSL defaults
Kept as a **harmless fallback** when `SSL_CERT_FILE` / `CURL_SSL_BACKEND` are unset: does not disable validation, helps tools that still prefer LibreSSL PEM roots. SecureTransport no longer depends on it.

### Still open (TLS)
- Optionally wire bring-up into `macos_gui.sh` start path (plists are in LaunchDaemons).
- Broader CT-policy admit helper for other Apple-signed rootfs/cryptex binaries.


## 16. System-level Gigacage + generic XPC (2026-11-10 Asia/Shanghai)

User ask (t120u): system-layer fixes so new software works without per-app patches.

### Gigacage (system policy; full reservation infeasible)

Evidence on iPad14,5 chroot (probe `va_ceiling` / `mmap_align_probe`):

| alignment | max successful anonymous map |
|-----------|------------------------------|
| 32 GiB    | **31 GiB**                   |
| 16 GiB    | 47 GiB                       |
| 1 GiB     | 53 GiB                       |

Ventura JSC requests `totalSize=85899345920` (80 GiB) with `maxAlignment=34359738368` (32 GiB) → `KERN_NO_SPACE` → FATAL in `Gigacage::ensureGigacage`. **Full-size Gigacage cannot be reserved on this VA ceiling** (not a per-app bug; OS upgrade/wipe out of scope).

**Most general feasible fix shipped:** `libmachook` `getenv_new` defaults `GIGACAGE_ENABLED` to `"0"` when unset for every injected process. Explicit `GIGACAGE_ENABLED=1` or `MACWS_ALLOW_GIGACAGE=1` still opt into the FATAL path for diagnostics. Verified: stock WebContent prints `Warning: disabling gigacage because GIGACAGE_ENABLED=0!` with **no** process env set.

WebKitChrootProxy still also injects `GIGACAGE_ENABLED=0` as belt-and-suspenders until every XPC spawn path is confirmed to load `getenv_new`.

### Generic AMFI CT-policy admit

`layout/usr/macOS/bin/macws_admit_rootfs_binary.sh` — thin arm64e, ldid merge, `jbctl trustcache add`, rollback copy. Used in-place on stock WebContent/Networking/GPU/SandboxBroker (CT policy 0x8 cleared; process starts, then hits Gigacage without the system getenv default).

### Generic Application-type XPC table

- `/usr/local/etc/macws-xpc-app-services.tsv` (rootfs-visible) + `/var/jb/usr/macOS/etc/...` host copy
- `macws_xpc_register_app_service.sh` appends rows
- `libmachook` loads TSV on first lookup; `xpc_add_bundle` prefers `/usr/local/libexec/macws-xpc/<relative>` (chroot-visible) then jb Frameworks tree
- Seeded with WebKit WebContent/Networking/GPU + Safari SandboxBroker
- Hardcoded WebKit branches retained as bootstrap fallback

Proxies retargeted to **stock** admitted Mach-Os (no per-app `macws-webkit` copies as the preferred path).

### Still open (WebContent stay-up / page render)

After Safari relaunch with new libmachook + rootfs proxies + system Gigacage default:

- Safari launches; `open-documents` delivers `.webloc`
- **No** macOS `WebContent` / `Networking` / `GPU` stay up (only iOS MobileSafari WebKit XPCs + historically SandboxBroker under old `macws-webkit` path)
- Likely remaining blocker: Safari App Sandbox `mach-lookup` allowlist vs remapped `com.apple.macosbooter.WebKit.*` names (public names collide with iOS). Entitlement expansion previously SIGKILL'd Safari — needs a careful system-level sandbox allow for the private names, not per-app disables
- Screenshots: `field-artifacts/net3-*.png` / `/workspace/net3-*.png` (chrome/favorites; pages not rendering)

### a868f70 curl defaults

Kept as **harmless fallback** (`CURL_SSL_BACKEND=openssl` + `SSL_CERT_FILE` still appear in Safari env from hostd). System SecureTransport path is primary after trustd/securityd (commit `66f3d88`).

### Rollback

`/var/mobile/macws-rollback/` + `NOTES.txt`: `libmachook*.pre-gigacage-*`, `libmachook*.pre-xpcpath-*`, `*.pre-admit-*` for WebKit/SandboxBroker, prior TLS/securityd backups.



## 17. WebKit XPC stay-up diagnosis (2026-11-10 Asia/Shanghai)

Goal: Safari WebContent/Networking/GPU stay up + render example.com / baidu.com, system-level.

### Confirmed causes (logs / probes)

1. **Shared-cache XPC path gap (fixed in code):** `Metal_hooks` `MSHook` of `xpc_connection_create` bypassed `macws_xpc_connection_create_early`, so WebKit never hit `xpc_add_bundle`. Now both paths call `macws_xpc_create_application_service`.
2. **Same-name activation collides with iOS:** activating `com.apple.WebKit.WebContent` without remap attaches to **iOS** MobileSafari WebContent (`mobile` pid, `/System/Library/Frameworks/WebKit/...`), not macOS. Must use private `com.apple.macosbooter.WebKit.*`.
3. **Sandbox vs private names:** userspace `sandbox_check` was **not** observed on the hot path (empty allow log). Historical dmesg `Sandbox: Safari deny mach-lookup com.apple.WebKit.WebContent` was the **public** name when remap was missing. For private names, `sandbox_extension_issue_mach("com.apple.webkit.extension.mach", macosbooter…)` **succeeds** in Safari (logged to `/tmp/macws-sandbox-allow.log`) — system-level, no Safari re-sign.
4. **WebContentProxy SIGKILL without admit:** unsigned/untrusted proxy binary → SIGKILL. After `macws_admit_rootfs_binary.sh`, direct exec returns **118** (proxy env bootstrap without XPC) instead of kill — AMFI OK.
5. **LaunchDaemon MachServices are harmful if loaded:** bootstrapping `com.macwsguide.webkit.*` claims `macosbooter.WebKit.*`, launchd starts the `.xpc` proxy as a daemon → log `An XPC Service cannot be run directly.` and **blocks** bundle activation. Keep those plists **unloaded** (annotated KEEP UNLOADED).

### Still open

- After remap + extension + admit + LaunchDaemon bootout, Safari still retries TSV hits (**~20–300×**) and **no** `WebContentProxy` / macOS WebContent process stays up. Page chrome only; no render.
- Next: prove `xpc_add_bundle` actually registers/activates (in-chroot probe); compare with live DockHelper registration; consider embedding proxies under a rootfs-visible Frameworks path Dock uses successfully; do **not** re-sign Safari entitlements.

### System-level pieces shipped this round

- `macws_xpc_create_application_service` + Metal_hooks wiring
- `sandbox_check*` allow for `com.apple.macosbooter.*` + `sandbox_extension_issue_mach` for WebKit extension class
- DockHelper-like proxy Info.plist (`_NSApplicationMain`, no RunningBoard Managed)
- Proxy admit-on-install expectation + rollback NOTES
- Screenshots: `field-artifacts/net4-*.png` / `/workspace/net4-*.png`

## 18. WebContent stay-up: host-visible xpc_add_bundle + reboot trust (2026-11-10 Asia/Shanghai)

### Reboots (diagnosis)

**2026-11-10 01:45:10–01:45:25 CST (full kernel reboot, unexplained).**  
`lockdownd`: `Boot session changed (reboot detected)` at 01:45:25. `shutdown.log` SIGTERM flush at 01:45:10 with macOS still up (Safari pid 46628, Settings, securityd). **No** `panic-full-*.ips` and **no** new `ResetCounter` for this event, so the cause is not written as the same mutex panic. Earlier the same day: `panic-full-2026-11-09-153246.000.ips` (`Invalid/destroyed mutex @lock_mtx.c:161`, `kernel_task`, **Compressor Info: 98% of compressed pages limit (BAD)**) plus JetsamEvent storms at 17:14 and 18:17–18:18 CST. 10:34 CST was a later **userspace** jailbreak (`Starting Up` without boot-session change). Kernel `uptime` therefore counted from 01:45, not from the morning jailbreak. Load ~41 at 10:36 was macOS coming up (~379 processes) and later fell to ~2.5.

**2026-11-10 10:57:15 CST (user-initiated full reboot).**  
`ResetCounter-2026-11-10-105715.ips`: `Boot faults: rst btn_rst,btn_seq_reset timeout,dblclick_timeout`. `lockdownd` boot session changed at 10:57:15, then jailbreak `Starting Up` at 10:58:41. No new panic-full. macOS was started the same way as the in-app button: `macos_gui.sh watchdog coexist --no-vnc --no-terminal --experimental`. WebKit LaunchDaemons stayed unloaded.

### Guardrails used for later probes

- Before each probe, require ≳ 300 MB free pages and a low compressor (`occupied` not high). Otherwise abort and back off.
- One Safari/WebKit attempt at a time; a timeout watchdog kills ballooning WebKit/Safari/probe processes.
- Never leave probe processes behind.
- Stop if a second *unexplained* reboot occurs (the 10:57 button reset does not count).

### Why WebContent/Networking/GPU did not stay up

Live comparison of DockHelper vs `xpc_add_bundle` (guarded in-chroot probe `xpc_bundle_probe2`, 2026-11-10 11:21–11:30 CST):

1. **`xpc_add_bundle` preferred a chroot-only path.**  
   `macws_register_xpc_app_proxy_bundle` tried `/usr/local/libexec/macws-xpc/...` first. That path exists *inside* the chroot (so `access()` succeeds in Safari) but **does not exist on the iOS host**. launchd/xpcproxy resolves the recorded path in the host namespace → `Connection invalid`, no process. DockHelper is registered at `/var/jb/usr/macOS/Frameworks/Dock.framework/...`, which *is* host-visible. Inside the chroot, `/var/jb/usr` is an absolute symlink loop (`Too many levels of symbolic links`), so a naive `access()` fallback never selected the working DockHelper-style path.

2. **Reboot-volatile trustcache did not include WebKit.**  
   `macos_gui.sh` `restore_cold_boot_trust` already re-registers WindowServer, Dock, libmachook, etc. after every jailbreak, but not the WebKit proxies or the admitted stock WebContent/Networking/GPU/SandboxBroker binaries. After the 10:57 reboot, host exec of the setuid `WebContentProxy` was **SIGKILL (137)**. After `jbctl trustcache add` of the existing CDHashes (no rewrite), the same exec reached the stock target and exited with the expected no-XPC `An XPC Service cannot be run directly` / abort (134).  
   `macws_admit_rootfs_binary.sh` also ends with `chmod 755`, which strips the setuid bit the proxy needs to `chroot(/var/mnt/rootfs)`.

### System-level fix

- libmachook: `macws_xpc_host_visible_proxy_path` prefers `/var/mnt/rootfs/usr/local/libexec/macws-xpc/<relative>` (visible on the host *and* in the chroot via the `/var/mnt/rootfs -> /` self-symlink), then `/var/jb/usr/macOS/Frameworks/<relative>` (DockHelper pattern). It no longer registers the chroot-only `/usr/local/libexec/...` path. Shared by the TSV registrar and the WebKit fast path.
- `macos_gui.sh` `restore_cold_boot_trust`: also restores trust for the macws-xpc WebKit tree, the jb Frameworks WebKit tree, stock WebKit XPCServices, and Safari SandboxBroker. After trust, `chmod 4755` the four proxies (both trees).
- LaunchDaemons `com.macwsguide.webkit.*` stay **unloaded**.

### Verification (probe)

After re-trust + setuid, in-chroot `xpc_add_bundle` of the jb/host-visible path spawned stock macOS WebContent and it stayed up for the 5 s window:

`/System/Library/Frameworks/WebKit.framework/Versions/A/XPCServices/com.apple.WebKit.WebContent.xpc/Contents/MacOS/com.apple.WebKit.WebContent` (root, `Ss`, ~28–29 MB RSS, ppid 1). The previous chroot-only path still produced `Connection invalid` and no process. Probe and WebContent were cleaned up; compressor stayed idle.

### Verification (guarded Safari, 2026-11-10 11:36–11:44 CST)

Deployed the new thin libmachook (CDHash `9720795b…` / arm64 `b921d279…`) and `macos_gui.sh` to jb + rootfs (fresh inodes, `ldid` + `jbctl`, rollback `*.pre-net5-*`). Did **not** restart macOS; a new Safari is enough to load the new dylib.

One guarded attempt (`net5_safari.sh`: mem_guard, watchdog, only macOS Safari/WebKit, no MobileSafari):

- Safari launched (pid 4984, root, ppid `macwshostd`, ~50 MB RSS). `vmmap` showed the **new** `/usr/local/lib/libmachook.dylib`. `DYLD_INSERT_LIBRARIES` was set. Proxies exist at the host-visible path `/var/mnt/rootfs/usr/local/libexec/macws-xpc/WebKit.framework/Versions/A/XPCServices/*.xpc`.
- **No** macOS WebContent / Networking / GPU / proxy process appeared. `/tmp/macws-xpc-app.log` and `/tmp/macws-sandbox-allow.log` stayed **empty**, so Safari never entered `macws_xpc_create_application_service` (no TSV-hit). `open-documents` of `/tmp/example.webloc` probably did not start a load (Favorites-only is also what net4 captures showed).
- `screencapture -x /tmp/net5-example.png` hung ~2.5 min with no file. At that point free pages were already ~235 MB. The watchdog (40 s) had already exited, so the script was aborted: hung screencapture + the script tree were killed, then Safari, **before** opening baidu.
- Concurrently, Finder, Dock, ControlCenter, SystemUIServer, and OSXvnc-server were all in **`Ts` (SIGSTOP)** while WindowServer stayed `Ss`. That likely explains the hung capture and a Safari UI that never actually presented. They were **not** continued: free pages were already under the ~300 MB guard (compressor still idle, `occupied=36`). No second Safari attempt.
- No new panic-full; kernel uptime still counts from the 10:57 button reset (not a second unexplained reboot).
- Screenshots were **not** produced (`field-artifacts/net5-*.png` / `/workspace/net5-*.png` are not present).

### Still open (superseded)

The 11:36 CST run did not show a TSV-hit. Later runs did — see §19–§20. The path this section preferred (`/var/mnt/rootfs/usr/local/libexec/macws-xpc/...`) is **not** the path that keeps helpers up; §20 prefers the jb Frameworks path.

### Rollback

`/var/mobile/macws-rollback/` + `NOTES.txt`: `libmachook*.pre-net7-*`, `*.pre-net6-*`, `libmachook*.pre-net5-*`, `macos_gui.sh.pre-net5-*`, prior `*.pre-admit-*` / `xpc_bundle_probe2.pre-admit-*`. Re-trust of existing CDHashes does not rewrite the proxies.

## 19. Guarded Safari after GUI un-stop (2026-11-10 13:28 CST / 21:28 PT)

User-authorized one guarded Safari run even though free pages were still under the ~300 MB floor after unlock (started 345 MB, dropped to 185, recovered to 214; compressor steady). Watchdog tripped mid `launch-path` at free=148 (`wc_mb=0`). `open-documents` then started macOS Safari pid 12614 (~155 MB RSS).

Safari **did** enter `macws_xpc_create_application_service`: TSV-hits WebContent ×4, Networking ×3, SandboxBroker ×1; sandbox extensions issued OK. No macOS WebContent / Networking / GPU / proxy process stayed up. Free fell to 19 MB; the script aborted before baidu and before screencapture. No new crash / jetsam / AMFI / dmesg line explained the missing helpers. Safari 12614 and a leftover 12728 were cleaned. Free recovered 83 → 310 → 287 MB; compressor occupied stayed ~9210. LaunchDaemons `com.macwsguide.webkit.*` stayed unloaded. No second reboot (uptime still from the 10:57 CST button reset). No screenshots (`/workspace/net5-*.png` not produced). Cause of helper exit was not yet pinned, so no code commit in that window.

Safari's own ~155 MB RSS plus the watchdog floor is a real limiter for a full render test on this iPad: a launch can drop free under 180 MB mid-start even when the compressor is idle.

## 20. XPC path logging and jb-Frameworks preference (2026-11-10 14:59–15:16 CST / 22:59–23:16 PT)

### Diagnostics added (libmachook)

In the XPC Application-service create path: log the resolved absolute `xpc_add_bundle` path (`access` / errno), `add_bundle_done`, `created activate=… conn=`, and wrap the connection event handler (DYLD_INTERPOSE + `MSHookFunction` of `_xpc_connection_set_event_handler`) to log `Connection invalid` / `interrupted` with the activate service name to `/tmp/macws-xpc-app.log`. `xpc_add_bundle` is `void`; the useful result is a later XPC error or a spawned helper.

### Deploy net6 (logging only)

Thin libmachook, macOS 13 LC_BUILD_VERSION, fresh inodes, `ldid -S` ×2 + `jbctl trustcache add` (CDHash and sha1 candidate) on jb and rootfs. Did **not** restart macOS.

- arm64e CDHash `28c37f95e918741e2707fb1c9ba62deecff98a6d` (sha1 `935768a32ade484ea54c120bb9ff146126693491`)
- arm64 CDHash `466c69a5d69439e574739b3ecee48ff3efa95345` (sha1 `f780341bfcc9d7e5696db55087e1e937a8b9107b`)
- Rollback `*.pre-net6-20261110-145918`. New log strings and the host-visible format string were present in the deployed dylib.

### Same-boot probes (no Safari)

`run_bash.sh` / `launchdchrootexec` always inserts `/usr/local/lib/libmachook.dylib`. Timeout 8 s, watchdog, cleanup. Both proxy trees are the same setuid `4755` binary (CDHash `181c64a699f325458491544c309f5d397bdd5007`), same Info.plist (`CFBundleIdentifier` = `com.apple.macosbooter.WebKit.WebContent`, `ServiceType=Application`, `_MultipleInstances`).

1. **Path Safari's hook actually registered** (preferred by the 8b53f0a resolver because in-chroot `access()` succeeds):  
   `/var/mnt/rootfs/usr/local/libexec/macws-xpc/WebKit.framework/Versions/A/XPCServices/WebContentProxy.xpc`  
   + activate `com.apple.macosbooter.WebKit.WebContent`.  
   `access_R_X=0`, `xpc_add_bundle` returned, `conn` non-NULL, then **`xpc_error: Connection interrupted`**. No WebContent or `WebContentProxy` process appeared in 1 s samples. `/tmp/macws-xpc-app.log` recorded `app-xpc name=com.apple.macosbooter.WebKit.WebContent` twice (the create hook only TSV-hits / hardcodes the public name `com.apple.WebKit.WebContent`, so a private-name probe does not log `add_bundle tag=`). free 156 → 119 MB; compressor occupied stayed 8785. No new crash, jetsam, or AMFI report.

2. **DockHelper-style jb path** (in-chroot `access_R_X=-1` because `/var/jb` is a symlink loop — this is expected and must not be used as a gate):  
   `/var/jb/usr/macOS/Frameworks/WebKit.framework/Versions/A/XPCServices/WebContentProxy.xpc`  
   + the same activate name.  
   **Stock macOS WebContent spawned and stayed up** until the memory watchdog: pid 21371, ppid 1, `Ss`, ~29 MB RSS, path `/System/Library/Frameworks/WebKit.framework/Versions/A/XPCServices/com.apple.WebKit.WebContent.xpc/Contents/MacOS/com.apple.WebKit.WebContent`. No `xpc_error` while it was up. Started at free=100 MB; watchdog killed the helper at free=59 MB. Cleaned; no leftovers. compressor occupied stayed 8770.

So the helper-stay-up failure after Safari's TSV-hits is explained by the **path recorded for host launchd/xpcproxy**, not by missing trust, stripped setuid, or an unlogged crash. `/var/mnt/rootfs/usr/local/libexec/macws-xpc/...` is visible in both namespaces but does not keep a helper up; `/var/jb/usr/macOS/Frameworks/...` does, matching DockHelper.

### System-level fix

`macws_xpc_host_visible_proxy_path` now always registers `/var/jb/usr/macOS/Frameworks/<relative>` (shared by the TSV registrar and the WebKit fast path). It does **not** use in-chroot `access()` as a gate and still never registers the chroot-only `/usr/local/libexec/...` path. TSV relatives are all under `WebKit.framework/Versions/A/XPCServices/*.xpc`, which exist in the jb Frameworks tree.

### Deploy net7 (path preference + logging)

- arm64e CDHash `24b0fbf91195cbe5b451b3fd07322cbf5567792f` (sha1 `0d8f3ab33d516594a8190f445fa24f445b951475`)
- arm64 CDHash `7f4d7d650ccc8c91a5bf11301d206488dbfdde05` (sha1 `d807b8069c768e9742a8454ca8af5914f64ac351`)
- Rollback `*.pre-net7-*`. Deployed dylib contains `/var/jb/usr/macOS/Frameworks/%s` (and no longer the `macws-xpc/%s` format string). No macOS restart.

### Skipped

A hook-path probe with the public name `com.apple.WebKit.WebContent` (to log `add_bundle tag=` of the new jb path through `macws_xpc_create_application_service`) was aborted at free=81 MB. Guarded Safari (example.com / baidu, `/workspace/net5-*.png`) was skipped: free settled ~91–94 MB for 10+ minutes with compressor occupied steady at ~8768–8770, well under the ~300 MB floor. A later lighter render test (minimal `WKWebView` helper, or the existing 8 s `xpc_bundle_probe2` which already proved stay-up on the jb path) is cheaper than Safari's ~155 MB plus the watchdog floor.

No second unexplained reboot (uptime still from the 10:57 CST button reset). `com.macwsguide.webkit.*` LaunchDaemons stayed unloaded. No leftover probe/WebContent processes.

## 21. Safari helper crash is InitializeWebKit2 off-main, not Gigacage (2026-11-10 15:35–16:23 CST / 23:35 PT Nov 9 – 00:23 PT Nov 10)

User waived the free-page gate ([pt132u](pt132u)); watchdog is compressor spike (`comp_occ > start+25000` or `>80000`), jetsam of WindowServer / macwshostd / SpringBoard, or SSH drop only.

### Leading env hypothesis — false

Helpers spawned through the setuid ViewBridgeChrootProxy already get libmachook's required environment. The production proxy (WebKit targets, `MACWS_WEBKIT_PROXY_EXPERIMENT`) injects `DYLD_INSERT_LIBRARIES=/usr/local/lib/libmachook.dylib` and `GIGACAGE_ENABLED=0` (strings present in the deployed thin arm64e binary, CDHash `181c64a699f325458491544c309f5d397bdd5007`, setuid 4755). A 15:52 CST idle `xpc_bundle_probe2` WebContent (pid 27409, `Ss`, ~29 MB) had libmachook mapped (`TEXT` at `0x102ae8000`) and stayed up the full window; compressor unchanged. `sysctl kern.procargs2` could not dump the child env from the chroot, but the mapping is enough. Full 80 GiB / 32 GiB-align Gigacage remains VA-infeasible on this iPad; the getenv default and explicit proxy env are already in place for that.

### Decoded crash (net7, 15:35 CST / 23:35 PT Nov 9)

Safari-driven WebContent and Networking died at `EXC_BREAKPOINT` `0x1bde2d9ac` (WebKit base `0x1bddd2000` + `0x5b9ac`). The fully symbolicated report (`com.apple.WebKit.WebContent-2026-11-10-153522.ips`):

- `WTFCrashWithInfo(int, char const*, char const*, int)` at imageOffset 375212, symbolLocation 20
- `WebKit::runInitializationCode(void*)` at 2342788, symbolLocation 92
- `std::call_once` (libc++)
- `WebKit::InitializeWebKit2()` at 126680, symbolLocation 84
- `WebContentServiceInitializer` at 125852, symbolLocation 196
- `WebKit::XPCServiceEventHandler` block, then `_dispatch_sync_f_slow` / `_xpc_connection_call_event_handler`

Registers: `x0=51` is the **source line**, not a Gigacage constant. Ventura WebKit-7615.1.26.11.5 `WebKit2InitializeCocoa.mm:51` is `RELEASE_ASSERT_WITH_MESSAGE([NSThread isMainThread], "InitializeWebKit2 should be called on the main thread")`. `JSC::initialize()` (Gigacage) is line 58, after this assert, so the net7 crash never reached it. `x1`/`x2` point at nearby `__TEXT` strings (IPS mis-attributed them to a `ShareablePixelBuffer` `__PRETTY_FUNCTION__`). One-frame `PAC_EXCEPTION` reports are the same BRK, not a distinct arm64 vs arm64e entitlement problem (deployed proxy and libmachook are thin arm64e).

The idle probe stays up because its empty XPC message never enters `InitializeWebKit2`. Safari's first real bootstrap message does, off the main thread. Stock Apple-linked helpers have `linkedOnOrAfterSDKWithBehavior(InitializeWebKit2MainThreadAssertion)`, so `InitializeWebKit2` calls `runInitializationCode()` immediately on the calling thread instead of `WorkQueue::main().dispatchSync`. Apple's xpcproxy delivers Application-type (`RunLoopType=_NSApplicationMain`) first events on the main NSRunLoop; our setuid chroot proxy `execve`s the stock image after xpcproxy has already treated the proxy as the service, so the first event can arrive on a libdispatch queue.

Offsets in UUID `72317905-49DA-3A07-A8C1-D93B91F653CB` (image base in the crash and in live helpers was `0x1bddd2000`):

- `InitializeWebKit2` start = 126680 − 84 = **126596**
- `runInitializationCode` start = 2342788 − 92 = 2342696
- `WTFCrashWithInfo` start = 375212 − 20 = 375192
- `WebContentServiceInitializer` start = 125852 − 196 = 125656

### System-level hook

`libmachook` hooks `WebKit::InitializeWebKit2` and, when `!pthread_main_np()`, `dispatch_sync`s to the GCD main queue (same policy as WebKit's old-SDK fallback). `MSFindSymbol` / `dlsym` of `__ZN6WebKit16InitializeWebKit2Ev` miss: the symbol is not in the shared-cache export trie, and `dladdr` on a cache image does not report `.../WebKit.framework/Versions/A/WebKit`. The installer therefore walks dyld images, matches that UUID, checks the first instruction at +126596 is `PACIBSP` (`0xd503237f`), and `MSHookFunction`s that address. The hook is installed only in `com.apple.WebKit.WebContent`, `Networking`, `GPU`, and `com.apple.Safari.SandboxBroker`.

### Field runs (one Safari attempt at a time; previous cleaned)

Watchdog never tripped on compressor or core processes. `com.macwsguide.webkit.*` LaunchDaemons stayed unloaded. No leftover helpers. No second unexplained reboot (uptime still from the 10:57 CST button reset). Did not restart macOS; new processes load the new dylib. Rollback trees: `*.pre-net8-*`, `*.pre-net8b-*`, `*.pre-net8c-*`, `*.pre-net8d-*`.

1. **net8** (~15:57–16:00 CST / 23:57–00:00 PT): path-based installer. arm64e CDHash `48e00431b5a9633c918505837e7e1ed7ebc8d7a4`. `/tmp/macws-webkit-init.log` empty — hook never installed. Safari presented; example.com / baidu URLs in the bar; Favorites start page, no web content. Fewer XPC errors than net7; no new `.ips`.
2. **net8b** (~16:03–16:05 CST / 00:03–00:05 PT): symbol search on every image. arm64e CDHash `cdfc9c1f20e230501c84741b83a8f12f53127397`. Only "symbol not found at ctor" in non-WebKit processes; hook still not installed. Same Favorites-only result. Shots: `field-artifacts/net8b-*.png`.
3. **net8c** (~16:13–16:14 CST / 00:13–00:14 PT): first UUID-offset hook, not yet scoped. arm64e CDHash `d6d4061238f556bb339dbc2a9cdc4db0263ed5ce`. Hook installed in 3 short-lived pids. `macws_control_probe launch-path` reported no AppKit window in 30 s; captures are the desktop only (`field-artifacts/net8c-*.png`). Finder / Dock / WindowServer stayed `Ss`; compressor ~9942. Hooking Safari or the control probe likely blocked window publish, so the hook was then scoped to helpers only.
4. **net8d** (~16:21–16:23 CST / 00:21–00:23 PT): helper-scoped UUID-offset hook, log includes `argv0`. arm64e CDHash `9c0dbaac21659ebe8f1a9ebe1cae8524db62d5e3` (sha1 `0082b6513332843172fecc326819868b2773de42`), arm64 `bdb41467db2dcbc5141aee0b156055208cdb1db0`. Fresh inodes 44183 / 23172901. Safari pid 32403 launched (~204 MB); both weblocs were received. The hook **did** install in the stock WebContent, Networking, and SandboxBroker helpers (153 lines in `/tmp/macws-webkit-init.log`). It **never** logged `InitializeWebKit2 off main; dispatch_sync`, so the main-thread bounce did not run. Helpers respawned in a tight loop (~14 MB, etime ~1 s at snapshots). After example and after baidu, only Safari plus two fresh WebContent processes were visible; Networking did not stay up. XPC `Connection interrupted` / `invalid` counts were high. No new WebKit `.ips` (the 15:35 reports remain the last). Pages still did not render.

Screenshots of the net8d attempt (Favorites start page, URL bar `https://example.com` then `https://www.baidu.com`, menu clock 12:22 PT): `/workspace/net8-example.png`, `/workspace/net8-baidu.png`, and `field-artifacts/net8-*.png`. Logs: `field-artifacts/net8d-safari.out`, `field-artifacts/net8d-webkit-init.log`. Mid-capture `free_mb` dipped to 54–56 with `comp_occ` 10007–10009 (start 9940); after cleanup `CORE_OK`.

### Still open

The crash reason and the system-level hook site are solid; pages are not rendering yet. Either `MSHookFunction` of the ptrauth-signed cache offset is a bad trampoline (so the first call to `InitializeWebKit2` dies before `hooked_InitializeWebKit2` can log a bounce — other shared-cache hooks in this file pass an unsigned `header+offset`), or the helpers now die on a later init path without writing a report. Next: try the unsigned QuartzCore-style address, and/or make the proxy / XPC path deliver the first Application-type event on the main NSRunLoop so stock `InitializeWebKit2` is already on main.

## 22. XPC main-queue delivery keeps Safari helpers up (2026-11-10 16:34–16:36 CST / 00:34–00:36 PT)

### Why the net8d hook never logged a bounce

net8d installed `InitializeWebKit2` at UUID+126596 but `hooked_InitializeWebKit2` never ran. Two checks in this build:

1. **`MSHookFunction` needs the unsigned `header+offset`**, matching the QuartzCore / OSXvnc shared-cache offset hooks in `mac_hooks.m`. A `ptrauth_sign_unauthenticated` pointer can make Ellekit/Substrate treat PAC bits as part of the VA. This build passes the unsigned address.
2. Even with the unsigned address, **the hook still does not intercept**. Install logs `hooked InitializeWebKit2 via offset-unsigned sym=0x1bddf0e84 orig=0x0` and `hooked runInitializationCode raw=0x1be00df28 orig=0x0` (PACIBSP prologues matched at both crash-derived offsets). There were **zero** `InitializeWebKit2 call` / `runInitializationCode call` / `xpc event off main` lines, so neither hooked function nor the off-main XPC wrapper body ran. `orig=0x0` means `MSHookFunction` did not produce a trampoline for these shared-cache text addresses; the real functions stay unhooked.

The instruction at `WebContentServiceInitializer+196` (offset 125852) is `0xd10103a0` (`sub`), then `0x94088239` (`BL` at 125856) to `0x1be011484`, which is **neither** `InitializeWebKit2` (`0x1bddf0e84`) nor `runInitializationCode` (`0x1be00df28`). So 125852 is not the BL that enters init; the IPS `symbolLocation` is a return-address or nearby instruction, not a direct BL at that exact offset. The crash stack still names `InitializeWebKit2` / `runInitializationCode` at those function starts; the hook sites themselves are the right functions, but shared-cache hooking of them is not reliable here.

### System-level alternative (this is what changed the run)

In `macws_xpc_wrap_app_event_handler`, for WebContent / Networking / GPU / SandboxBroker only: `xpc_connection_set_target_queue(connection, dispatch_get_main_queue())`, and if a handler still runs off-main, `dispatch_sync` to the main queue. That is the Application-type policy Apple's xpcproxy would have applied before our setuid chroot proxy `execve`'d the stock image. No "xpc event off main" log means either the target-queue change already delivered events on main, or the helper's `set_event_handler` path did not go through this wrap (the stay-up result below argues the former).

### Guarded Safari (one attempt)

Deploy net9, no macOS restart. arm64e CDHash `3d692dc0936f4ade31f6d10023f33986f01651ff` (sha1 `a37c1ca904c6501e4efcf7dd43aae91f11d56b26`), arm64 `bf746a243a0f47b8ba4e95aff247cf06a33ab8cf`. Fresh inodes 44191 / 23173352. Rollback `*.pre-net9-20261110-163407`.

Safari pid 34430 launched (~329 MB) and received both weblocs. **Helpers stayed up for the whole window** (unlike net7/net8d respawn loops): one Networking (pid 34450, `Ss`, ~28 MB, etime 28 s after example / 45 s after baidu) plus a dozen WebContent processes (`Ss`, ~28 MB, etimes 15–45 s). **Zero** `Connection interrupted` / `Connection invalid`. TSV-hits: WebContent ×12 then ×14, Networking ×1, SandboxBroker ×1, jb Frameworks path, `access=-1 errno=62` as expected. **No GPU process** appeared. No new WebKit `.ips` (last remain the 15:35 CST net7 reports). Hook log: 16 helper pids installed both hooks, never a call. Mid-capture `free_mb` 26–48, `comp_occ` 9950 → 12518 (not a watchdog spike). After cleanup `CORE_OK`, no leftover macOS Safari/helpers, LaunchDaemons unloaded, uptime still from the 10:57 CST button reset.

Screenshots (menu clock 12:35 PT): `/workspace/net9-example.png`, `/workspace/net9-baidu.png`, `field-artifacts/net9-*.png`. This is the first run that **left the Favorites start page**. The URL bar shows `example.com` / `baidu.com` with a loading indicator; baidu grew a "B" favicon tab. The content area stayed **blank white** — navigation and networking started, compositing/paint did not finish. Logs: `field-artifacts/net9-safari.out`, `field-artifacts/net9-webkit-init.log`.

### Still open

Function-offset `MSHookFunction` on this WebKit shared-cache text is not a reliable intercept (`orig=0`). The XPC main-queue path is the working system-level fix for the isMainThread assert and keeps helpers up. Next is why the page stays blank with a loading bar while WebContent/Networking are `Ss` — the missing GPU process is the leading lead.

## 23. CARenderServer XPC rewrite; GPU never requested; WebContent never hosts a CAContext (2026-11-10 16:51–16:59 CST / 00:51–00:59 PT)

### GPU process is never requested (not a launch failure)

Safari defaults have no `UseGPUProcessForDOMRendering`, `UseGPUProcessForMedia`, or `UseGPUProcessForCanvas` keys. The TSV and the setuid `WebGPUProxy` already exist (flat layout `WebGPUProxy.xpc/WebGPUProxy`, not `Contents/MacOS`; same family as `WebContentProxy`, mode 4755). Across net9 / net10 / net10b there was no TSV-hit for `com.apple.WebKit.GPU`, no `xpc_connection_create` of that name, and no GPU process. For a static page such as example.com, in-process DOM rendering is the expected Ventura path, so a missing GPU process is not by itself why the content area stays blank.

### CARenderServer table drift

iOS launchd never publishes `com.apple.CARenderServer`. WindowServer checks in `com.apple.macosbooter.CARenderServer` (confirmed live on the WindowServer / `UIKitApplication:com.macwsguide.windowserver` process). Clients must rewrite the bootstrap / XPC name; without a remote `CAContext`, window *content* never reaches WindowServer (AppKit chrome still paints via CGS).

Two rewrite tables had drifted:

- `macws_private_bootstrap_service_name` (`DYLD_INTERPOSE` of `bootstrap_look_up`, in `mac_hooks.m`) already remapped `com.apple.CARenderServer`.
- `macws_private_chroot_service_name` (shared-cache `MSHook` of `xpc_connection_create_mach_service`, in `Metal_hooks.x`) did not.

QuartzCore often looks the name up through libxpc in the shared cache, so the interpose alone is not enough.

### net10 abort: do not export the remapper

The first net10 build removed `static` from `macws_private_bootstrap_service_name` so `Metal_hooks.x` could call it, and added a shared-cache `MSHook` of `bootstrap_look_up`. libmachook is built with hidden visibility. The result was `dyld: warning could not apply interposing tuples`, `symbol not found in flat namespace '_macws_private_bootstrap_service_name'`, and Abort trap 6 in every libmachook process, including `run_bash.sh`. Safari launched into that abort; captures timed out; no screenshots. arm64e CDHash `8f133c690ca5672277c0c69b1436834213690eaa` (sha1 `320edab6c3c20229295c9db551dd066058fc0685`). Rollback `*.pre-net10-20261110-165218`. That dylib must not be left deployed. The remapper is `static` again; the shared-cache `bootstrap_look_up` hook that depended on the export was removed.

### System-level fix (net10b)

Duplicate the CARender rewrite into `macws_private_chroot_service_name`. Do not export the bootstrap remapper. Paint-boundary logs (`/tmp/macws-webkit-paint.log`, chroot path `/var/mnt/rootfs/tmp/macws-webkit-paint.log`) record `CAContext remoteContextWithOptions:` / `contextWithCGSConnection:options:`, `CALayerHost setContextId:`, and `bootstrap_look_up` / `xpc_connection_create_mach_service` of CARenderServer.

Deploy net10b, no macOS restart. Pre-Safari smoke of the dylib was `SMOKE_OK`. arm64e CDHash `cffee750f072127a4fb0cb648357f7abe9c0868e` (sha1 `b027e89499f34b6bd3c74b55b66a9439f1b8e1d7`), arm64 `b9638f5dad1ee8cf7db3ff9e2d12b492abc12d06` (sha1 `15635f36cffe4754c309c5385eb54e5505572a6d`). Fresh inodes 44208 / 23174388. Size 1477120. Rollback `*.pre-net10b-20261110-165707`.

### Guarded Safari (one attempt, 16:57–16:59 CST / 00:57–00:59 PT)

Safari pid 38235 launched (~360–361 MB) and received both weblocs; both captures returned `CAPTURE_OK`. Helpers stayed up (Networking 38250 plus a dozen WebContent, `Ss`, ~28 MB, etime 15–42 s). Zero `Connection interrupted` / `Connection invalid`. No GPU process. No new WebKit `.ips` (the last remain the 15:35 CST net7 reports). After cleanup `CORE_OK`, LaunchDaemons unloaded, no leftover macOS Safari/helpers, uptime still from the 10:57 CST button reset. Mid-run `free_mb` 30, `comp_occ` 12036 → 17274 (delta 5238, under the compressor emergency).

Paint log (8339 B):

- Safari `bootstrap_look_up 'com.apple.CARenderServer' -> 'com.apple.macosbooter.CARenderServer' -> 0 port=0x3a0b`.
- Safari `remoteContextWithOptions` produced `contextId=4279728157`, plus several `contextWithCGSConnection:110135` with nonzero ids.
- `screencapture`, QuickLookUI, and `openAndSavePanelService` also rewrote CARender and created remote contexts, so the XPC-table sync is a real system-level fix, not a Safari-only path.
- WebContent pids logged only `CAContext hooks ctx=… host=…` (the installer ran). Zero WebContent `remoteContextWithOptions` / `contextWithCGSConnection`, zero `xpc_connection_create_mach_service` CARender lines, and **zero** `CALayerHost setContextId:` in Safari. RemoteLayerTree hosting never attached.

Screenshots (menu clock 12:57 / 12:58 PT): `/workspace/net10-example.png`, `/workspace/net10-baidu.png`, `field-artifacts/net10-*.png`. The URL bar shows `example.com` / `baidu.com` with a loading indicator still drawn; baidu grew a "B" favicon on an Untitled tab; a Start Page tab remains. The content area is still **blank white**, the same visual result as net9.

### Still open

The CARender XPC-table sync is the correct generic fix for clients that look up `com.apple.CARenderServer` through libxpc, but it is not sufficient by itself for WebContent paint. Safari can talk to CARenderServer and create CAContexts; WebContent never creates a CAContext, and Safari never calls `setContextId` on `CALayerHost`. Leading next hypotheses: WebContent uses an unhooked CAContext / C API to create the remote context; a drawing-area or GPU-process expectation prevents in-process context creation even though Safari never requests the GPU process; or an IOSurface / sandbox extension on the WebContent side still blocks backing stores (`IOSurfaceCreate_new` already exists and only decompresses compressed surfaces; no WebContent-specific IOSurface failure was logged this run). A per-app env disable of GPU or compositing was not used: GPU was never requested, and compositing is the path that still needs to work.
