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

## 24. WebContent paint logging; CGS MSHook and ctor dispatch_after are unsafe (2026-11-10 17:43–18:04 CST)

### Context

The Mac Pro returned at 17:43 CST (`OK了`). The iPad was still up from the 10:57 CST button reset (about 7 h, no second reboot). `CORE_OK` (WindowServer, macwshostd, SpringBoard). `com.macwsguide.webkit.*` LaunchDaemons stayed unloaded. Compressor emergency only: `comp_occ` held around 16488–16492 (well under start+25000 / 80000). Finder, Dock, SystemUIServer, ControlCenter, and OSXvnc were `Ts` at 17:46 CST; the Safari script SIGCONT'd only those already-stopped macOS chrome processes and never SIGSTOP'd anyone. After that they stayed `Ss`. Nested `/var/jb/usr/bin/sudo -n` is required for deploy and rollback (the outer expect `sudo -S` no longer elevates). Stale hung expect sessions were left alone.

The net10b diagnosis still stood: Safari can create CAContexts, GPU is never requested, WebContent never hosts a remote `CAContext`, and Safari never `setContextId`. This turn added WebContent-only paint logging and a generic CARender port recovery, then stopped after two guarded Safari attempts showed the new hooks themselves were unsafe.

### net11 first deploy — Bus error in every process (17:52 CST)

The first build installed CGS / SLS / `CARemoteLayerServer` / `CALayer setGeometryFlipped:` hooks from the libmachook constructor in every process, plus WebContent logging of CAContext options / `kCAContextPortNumber`, `bootstrap_look_up` results, sandbox mach-lookup denials, XPC event begin/end (InitializeWebProcess / CreateWebPage ride that connection), and a drawing-area constructor breadcrumb. Generic recovery: if WebContent `remoteContextWithOptions` sees `portNumber` 0, a nil context, or `contextId` 0, look up `com.apple.macosbooter.CARenderServer` and retry. No new exported symbols (`task_get_bootstrap_port` is a macro, not an `extern`).

Device arm64e CDHash `6f1acc3033dd778069d56b2116d7d3c976e3d543` (sha1 `73dc0c3113530258d77fa71e380aa18decce03c2`). Rollback `*.pre-net11-20261110-175218` (a copy of working net10b). Smoke via `run_bash.sh -c 'echo SMOKE_OK'` died in `launchdchrootexec` with `Bus error: 10`. Immediate rollback to net10b CDHash `cffee750f072127a4fb0cb648357f7abe9c0868e` (sha1 `b027e89499f34b6bd3c74b55b66a9439f1b8e1d7`), arm64 `b9638f5dad1ee8cf7db3ff9e2d12b492abc12d06`. Restored-dylib smoke was `SMOKE_OK` at 17:54:54 CST. That crashing dylib must not be left deployed.

### net11b — hooks scoped to Safari/WebContent; Safari SIGBUS (17:54–17:58 CST)

`CGSNewConnection` was dropped (private ABI, unused after the scoping). The new CGS / `CARemoteLayerServer` / `setGeometryFlipped:` hooks were restricted to Safari and WebContent. The net10b global CAContext / CALayerHost hooks stayed global. No new exports.

Deploy 17:54:48 CST, no macOS restart. Device arm64e CDHash `24c947bee6902299eb865bd6e4d4808fce51e93c` (sha1 `69e0f3a98a67c379e8a1dabeeef96e4af14e56a1`), arm64 `75531a078cec8f18829e13803dd6fecc96633d39` (sha1 `b9aa0475e849cd857b8a1d85b3f18ed818b8620e`). Size 1479488. Fresh inodes 44238 / 23284267. Rollback `*.pre-net11b-20261110-175448`. Smoke `SMOKE_OK` at 17:54:54 CST.

Guarded Safari started 17:55:07 CST after SIGCONT of the already-`T` chrome. `macws_control_probe launch-path` reported `custom-path 在发布 AppKit 窗口前被信号 10 终止`. Safari never published a window; no helpers; both captures timed out; no screenshots. The paint log showed three short-lived Safari pids, each:

`CAContext hooks … deny=0x0 CGSMain=0xfd50000104c68000 SLSMain=0x0`

`CGSSetDenyWindowServerConnections` and `SLSMainConnectionID` were not found. `CGSMainConnectionID` was hooked (PAC-signed orig). bash and screencapture did not install those hooks (`CGSMain=0x0`) and did not crash. This is the same Bus error class as the global net11 install, now isolated to Safari, which calls `CGSMainConnectionID` while publishing an AppKit window. Immediate rollback to `*.pre-net11b-20261110-175448` (net10b). Smoke `SMOKE_OK` at 17:58:50 CST.

### System-level response

Do not `MSHookFunction` SkyLight `CGS*` / `SLS*` symbols. Observe `CGSMainConnectionID` / `SLSMainConnectionID` / `CGSSetDeny_sym` through `dlsym` only, from WebContent heartbeats. `CARemoteLayerServer` and `setGeometryFlipped:` IMPs stay WebContent-only. Safari's `[CARemoteLayerServer sharedServer].serverPort` is logged without replacing methods. The WebContent CARender port-retry in `remoteContextWithOptions` is unchanged (generic, no per-app env, no new exports).

### net11c — Safari stays up but never publishes an AppKit window (17:59–18:03 CST)

Deploy 17:59:42 CST, no macOS restart. Device arm64e CDHash `fb1ed24c51d6020571a4892d5e84508899850086` (sha1 `68162ad4803950fe269978e7615f55da7ef8bc32`), arm64 `ba48b70cb27b047518eb3433f49bfb3fac5d5bf0` (sha1 `1cfc9f3b1bb3fc017d24904b358c39602b8db077`). Size 1479856. Fresh inodes 44251 / 23297618. Rollback `*.pre-net11c-20261110-175942`. Smoke `SMOKE_OK` at 17:59:48 CST.

This was the allowed follow-up after the SIGBUS fix, once the net11b run had been cleaned up. Guarded Safari 17:59:52–18:03 CST: pid 44257 stayed up (~51 MB, `S`) for the whole window, but the probe reported `custom-path 已启动，但 30 秒内没有发布可捕获的 AppKit 窗口`, and document events were never confirmed. Zero WebContent / Networking / GPU helpers. XPC and init logs were empty. The paint log had only the constructor line `wc=0 safari=1` plus the bash/screencapture installer lines. The delayed 2 s `dispatch_after(..., dispatch_get_main_queue(), …)` that was supposed to log Safari `serverPort` **never ran**, so the main queue was idle or stuck. No `remoteContextWithOptions`, no `setContextId`. Both captures timed out. No `/workspace/net11-example.png` or `/workspace/net11-baidu.png`. Mid-run `free_mb` 46–84, `comp_occ` 16489 → 16442. After cleanup `CORE_OK`, LaunchDaemons unloaded, no leftover macOS Safari/helpers, uptime still from the 10:57 CST button reset. GUI left `Ss` (SIGCONT only).

Rolled back immediately to `*.pre-net11c-20261110-175942` (net10b `cffee750…` / `b9638f5d…`, size 1477120). Smoke `SMOKE_OK` at 18:04:10 CST. net11c must not be left deployed: unlike net10b, Safari never published a window.

### Source left in the tree (not deployed)

Constructor `dispatch_after` on the main queue is removed for both Safari and WebContent. Safari `serverPort` is logged from the first `remoteContextWithOptions` (unhooked read). WebContent heartbeats run from the first two XPC events. The tree compiles; that dylib was not deployed or Safari-tested (stop after the net11c hang; do not stack a third Safari run).

Logs: `field-artifacts/net11-safari.out` (net11c run), `field-artifacts/net11-webkit-paint.log`. Currently deployed is net10b.

### Still open

WebContent still never creates a CAContext (the net10b diagnosis stands; net11 / net11b / net11c never reached a paint-capable Safari+WebContent session). The next deploy should be the no-ctor-`dispatch_after` tree, with one guarded Safari only after smoke. Remaining leads are unchanged: `InitializeWebProcess` / `CreateWebPage` or the drawing-area constructor never completing, or a dropped compositing send-right (the port-retry is in the tree and untested).

## 25. 296254e logging build still blocks Safari window publish (2026-11-10 18:06–18:10 CST)

### Context

Health at 18:06:14 CST: up 7:09 from the 10:57 CST button reset (no second reboot). `CORE_OK`. WebKit LaunchDaemons unloaded. `comp_occ=16421` (under the emergency gate). GUI already `Ss` (SIGCONT from the earlier net11b/net11c runs; nothing was SIGSTOP'd). Deployed dylib was net10b. Stale hung expect sessions were left alone.

This turn deployed the already-committed 296254e logging tree: no SkyLight / CGS / SLS `MSHookFunction`, no constructor `dispatch_after` on the main queue, Safari `serverPort` logged from the first `remoteContextWithOptions`, WebContent heartbeats from the first two XPC events, CARender port-retry if `portNumber` is 0, no new exported symbols.

### net11d deploy and smoke

Rebuilt from `296254e0f6ad0bfca7e7fc4883343232cc3bdc29`, no macOS restart. Deploy 18:06:55 CST. Device arm64e CDHash `c79a42d13a091e676482da3bb8fb79cb6bece0c9` (sha1 `a013d749ebbd6bef362c63338a7246667b14e0d2`), arm64 `4c91502c1d54f683e53092152ff9caf21f4766a9` (sha1 `51e56d3e98ccfb914a5027f4c1704a67e6d2fce0`). Size 1479344. Fresh inodes 44262 / 23302177. Rollback `*.pre-net11d-20261110-180655` (a copy of working net10b). Smoke via `run_bash.sh` / `launchdchrootexec` was `SMOKE_OK` at 18:07:02 CST.

### Guarded Safari (one attempt, 18:07:07–18:10 CST)

Safari pid 46558 stayed up (~51 MB, `S`) for the whole window, but `macws_control_probe launch-path` reported `custom-path 已启动，但 30 秒内没有发布可捕获的 AppKit 窗口`. Both webloc opens failed with `目标应用没有确认 AppKit 文稿事件`. Zero WebContent / Networking / GPU helpers. XPC and init logs were empty. The paint log had only the constructor line `wc=0 safari=1` plus the bash/screencapture installer lines: no `remoteContextWithOptions`, so the unhooked Safari `serverPort` log never ran; no drawing-area breadcrumb; no XPC event begin/end; no `setContextId`. Both captures timed out. No `/workspace/net11-example.png` or `/workspace/net11-baidu.png`. Mid-run `free_mb` 42–45, `comp_occ` 16421 → 16409. After cleanup `CORE_OK`, LaunchDaemons unloaded, no leftover macOS Safari/helpers, uptime still from the 10:57 CST button reset.

This is the same hang as net11c, so removing constructor `dispatch_after` was not sufficient. The ctor itself finished (the installer line was written). Relative to net10b, which did publish a Safari window and start helpers, the remaining 296254e delta that still runs in Safari is the enhanced global `CAContext` hook body (portNumber / key logging and a WebContent-only retry) plus `setAllowsCGSConnections:`. Those hook *bodies* were never entered (no `remoteContextWithOptions` line), so the hang is earlier than the first CAContext call — either installing the extra IMPs, other 296254e ctor work, or leftover session state after the net11b SIGBUS / net11c hang. net10b Safari was not re-run this turn (stop after one failed attempt).

### Rollback

Rolled back immediately to `*.pre-net11d-20261110-180655` (net10b arm64e CDHash `cffee750f072127a4fb0cb648357f7abe9c0868e`, sha1 `b027e89499f34b6bd3c74b55b66a9439f1b8e1d7`, arm64 `b9638f5dad1ee8cf7db3ff9e2d12b492abc12d06`). Size 1477120. Smoke `SMOKE_OK` at 18:10:33 CST. net11d must not be left deployed. No second variant was tried.

Logs: `field-artifacts/net11d-safari.out`, `field-artifacts/net11d-webkit-paint.log`. Currently deployed is net10b.

### Paint-path conclusion

Not reached. InitializeWebProcess / CreateWebPage were not observed, the drawing-area constructor did not run, Safari never logged `CARemoteLayerServer.serverPort`, WebContent never created a CAContext, and the compositing port could not be checked. The net10b diagnosis (WebContent never hosts a remote layer) still stands, untested by this run.

### Still open

Next logging build has to stay out of Safari's window-publish path until a net10b-style window + helper launch is reproduced. Options: keep new hooks strictly out of the Safari process (install-time and method IMPs), or first confirm that net10b Safari still publishes a window after the net11b/net11c incidents. Do not reintroduce SkyLight MSHooks or constructor main-queue `dispatch_after`.

## 26. net10b Safari recheck: session no longer publishes AppKit windows (2026-11-10 18:11–18:16 CST)

### Context

Health at 18:11:29 CST: up 7:14 from the 10:57 CST button reset (no second reboot). `CORE_OK`. WebKit LaunchDaemons unloaded. `comp_occ=16408` (under the emergency gate). GUI already `Ss`. No leftover Safari. Deployed dylib confirmed net10b arm64e CDHash `cffee750f072127a4fb0cb648357f7abe9c0868e` (sha1 `b027e89499f34b6bd3c74b55b66a9439f1b8e1d7`), size 1477120, rootfs inode 23302283. Stale hung expect sessions were left alone. No new dylib was deployed this turn.

The previous two logging builds (net11c, net11d) had left Safari running at ~51 MB with no capturable window. The open question was whether that hang was the 296254e logging delta or leftover session state after the net11b `CGSMainConnectionID` SIGBUS and those hangs.

### Guarded Safari on net10b (one attempt, 18:11:44–18:14 CST)

Same driver as earlier runs, artifact names `net10b-recheck-*`. Safari pid 48615 stayed up (~51 MB, `S`) for the whole window — the same size as the hung net11c/net11d processes, not the ~360 MB of the successful 16:57 CST net10b run. `macws_control_probe launch-path` reported `custom-path 已启动，但 30 秒内没有发布可捕获的 AppKit 窗口`. Both webloc opens failed with `目标应用没有确认 AppKit 文稿事件`. Zero WebContent / Networking / GPU helpers. XPC log empty. The paint log used the net10b installer format (`CAContext hooks ctx=… host=…` only, no `wc=` / `safari=` flags): Safari, bash, and screencapture installed hooks; there was no `remoteContextWithOptions` and no `setContextId`. Both captures timed out. After cleanup `CORE_OK`, LaunchDaemons unloaded, no leftover macOS Safari/helpers, `comp_occ` 16408 → 16406, uptime still from 10:57 CST.

`/workspace/net10b-recheck-example.png` and `/workspace/net10b-recheck-baidu.png` were not created.

### Leftover-session diagnosis (no new dylib, no second Safari)

After cleanup:

- Boot-time chrome is still healthy: WindowServer 1575 `Ss` (~154 MB, up 7 h), Finder 1742 `Ss` (~114 MB), Dock / SystemUIServer / ControlCenter / OSXvnc all `Ss` with normal RSS. `macws_control_probe` reports `windowserver=yes busy=no`. So WindowServer and already-published windows are not dead.
- A desktop `screencapture -x /tmp/net10b-recheck-desktop.png` at 18:15:15 CST **timed out at 20 s**. Capture of the existing desktop is itself wedged, not only Safari's missing window.
- A single non-Safari probe of Activity Monitor (`/System/Applications/Utilities/Activity Monitor.app`) at 18:15:36 CST reproduced the same failure: pid 50563 started (~35 MB, `S`) but `activity-monitor 已启动，但 30 秒内没有发布可捕获的 AppKit 窗口`. It was SIGTERM/SIGKILL'd; none left. No new `.ips`.

New AppKit processes start but stay small and never publish a capturable window. That is session-wide, not Safari-specific, and it is why net11c/net11d were not a sufficient test of the logging dylib. The likely wedge is WindowServer / CGS window-publish and screencapture after net11b hooked `CGSMainConnectionID` in Safari and then SIGBUS'd mid-window-publish (17:55 CST), followed by hung Safari attempts. Existing windows from before that event remain; new ones and new captures do not.

### What was not done

No new libmachook was deployed. Step 2 WebContent-only logging was not started. WindowServer / macOS were not restarted (no full device reboot without confirmation). `CORE_OK` still holds; the 10:57 CST uptime is unchanged.

### Paint-path conclusion

Not reached. The session can no longer publish a new capturable AppKit window or complete `screencapture`, so InitializeWebProcess / CreateWebPage / drawing-area / WebContent CAContext / compositing port could not be observed. The 16:57 CST net10b diagnosis (WebContent never hosts a remote layer; content stayed white after leaving Favorites) is not re-verified in this session.

### Still open

A WindowServer or macOS-session restart (or a user-confirmed device reboot) is the likely way to recover window publish, after which net10b Safari should be rechecked before any new logging dylib. Do not reintroduce SkyLight MSHooks, extra Safari CAContext IMPs, or constructor main-queue `dispatch_after`.

## 27. WindowServer restart came back; final composite is still empty (2026-11-10 18:20–18:25 CST)

### Context

The user approved restarting only WindowServer. The iPad was not rebooted, 停止 macOS was not used, and SpringBoard / runningboardd were not restarted. net10b stayed deployed (arm64e CDHash `cffee750f072127a4fb0cb648357f7abe9c0868e`, sha1 `b027e89499f34b6bd3c74b55b66a9439f1b8e1d7`, size 1477120, rootfs inode 23302283). No new logging dylib.

Health at 18:20:43 CST: up 7:23 from the 10:57 CST button reset. `CORE_OK`. WebKit LaunchDaemons unloaded. `comp_occ=16905` (under the emergency gate). GUI was `Ss`. WindowServer was pid 1575 (ppid 1, `Ss`, ~154 MB), macwshostd 416, SpringBoard 391, runningboardd 389.

### Step 1 — WindowServer only

The job has no `KeepAlive`; it is `UIKitApplication:com.macwsguide.windowserver` (`OnDemand=true`), started through `launchdchrootexec` into the SkyLight `WindowServer -virtualonly -daemon`. The project's normal replace path in `macos_gui.sh` is: retire the old pid, then `launchctl start` that label, and wait for a new pid.

At 18:21:50 CST: SIGTERM of 1575 did not exit in 8 s, so that pid alone was SIGKILL'd. Then `launchctl start 'UIKitApplication:com.macwsguide.windowserver'` (rc=0). A new SkyLight WindowServer pid 51054 was `Ss` after 2 s (ppid 1, ~75 MB). SpringBoard 391, macwshostd 416, and runningboardd 389 were unchanged. `WS_RESTART_OK` at 18:22:01 CST. `comp_occ` 16967 → 15231, `free_mb` 15 → 227. Job list after: PID 51054, `LastExitStatus=9`, and `com.apple.macosbooter.CARenderServer` still in `MachServices`.

`macos_gui.sh start` / `stop` / `rebuild-desktop-session` were not used (start/stop can involve SpringBoard/backboardd; rebuild would have killed the new WindowServer again).

### Step 2 — chrome came back; capture and new windows did not

Killing WindowServer dropped Finder, Dock, SystemUIServer, ControlCenter, and OSXvnc (expected). At 18:23:24 CST those five jobs were `launchctl load`'d from `/var/jb/usr/macOS/gui-launchd/com.macwsguide.{finder-desktop,dock,systemuiserver,controlcenter,osxvnc}.plist`. All five were live within 1 s. WindowServer 51054 / hostd 416 / SpringBoard 391 unchanged. By 18:23:46 CST all five were `Ss` (Finder ~72 MB, Dock ~27 MB, SystemUIServer ~38 MB, ControlCenter ~47 MB, OSXvnc ~26 MB).

A desktop `screencapture -x /tmp/ws-restart-desktop.png` at 18:23:46 CST **timed out at 20 s**. Activity Monitor at 18:24:07 CST started (pid 51587, ~35 MB, `S`) but `activity-monitor 已启动，但 30 秒内没有发布可捕获的 AppKit 窗口`. It was SIGTERM/SIGKILL'd; none left. `/workspace/ws-restart-desktop.png` was not created. Safari was not started.

`macwsinputd` 51354 and `macwsdisplayd` 51356 were already a new generation (etime ~2:45 at 18:25, i.e. they respawned around 18:22:18, just after the new WindowServer). They are not leftover 7-hour stubs. `macwsallocd` 435 is still the boot-age process.

The new WindowServer's own memdiag (ticks 34–37 in `/var/jb/var/mobile/WindowServer.err`) shows a live process with **no final composite**: `owned_scanout=0`, `composite_stage_contexts=0`, `composite_pending=0`, `shadow_surfaces=0`, `disp_tracked=0`, `lease_live=0`, `iosurf=0/0`, `phys_footprint=29.1MB` / resident ~85 MB. That is why screencapture times out and the probe cannot see a capturable AppKit window even though Finder and Dock are `Ss`.

`CORE_OK` still holds, LaunchDaemons unloaded, `comp_occ` later 8816, `free_mb` 241, uptime still from 10:57 CST.

### What was not done

No Safari run. No new libmachook. No device reboot. No 停止 macOS. No SpringBoard / runningboardd restart. `macos_gui.sh repair-desktop` (which can detect an unhealthy final compositor and request a session rebuild) and `rebuild-desktop-session` were not run.

### Still open

WindowServer alone is not enough to restore a capturable session in this state. The remaining wedge is an empty final composite / scanout on the new generation, despite new input/displayd and relaunched Aqua chrome. The project's next in-session recovery, if approved, is `macos_gui.sh repair-desktop` and, if that reports an unhealthy compositor, the controlled `rebuild-desktop-session` path — not a full device reboot and not 停止 macOS. Re-check desktop capture and Activity Monitor before any Safari or logging dylib.

## 28. iPad already unlocked; capturable windows still wedged (2026-11-10 18:27–18:31 CST)

### Context

The user asked whether uncapturable windows were caused by a locked device/session, and approved unlocking first. No `repair-desktop` / `rebuild-desktop-session`, no device reboot, no 停止 macOS, no new dylib. net10b stayed deployed (arm64e CDHash `cffee750f072127a4fb0cb648357f7abe9c0868e`). The existing `vnckeepalive.py` process (already running) was left in place. Passcode and VNC secrets were not printed or committed.

Health at 18:27:58 CST: up 7:30 from the 10:57 CST button reset. `CORE_OK`. WebKit LaunchDaemons unloaded. `comp_occ=8794`. WindowServer 51054, Finder/Dock/SystemUIServer/ControlCenter/OSXvnc all `Ss`. `trollvncserver` on the device was live (pid 7354).

### Lock state

A TrollVNC framebuffer shot at 18:29:43 CST (2732×2048) showed the macPad host UI and iPad home dock, not an iOS lock screen. The session was **already unlocked**. The swipe-up + HID passcode path was therefore not used (typing a passcode into the visible host UI would have been the wrong target). macOS login/screensaver could not be confirmed via OSXvnc (`localhost:5900`); the iOS-side display was clearly not locked.

### Capture / new-window recheck

Desktop `screencapture` at 18:30:09 CST timed out at 20 s. `/workspace/ws-unlock-desktop.png` was not created. Activity Monitor at 18:30:30 CST started (pid 51738, ~35 MB, `S`) but `activity-monitor 已启动，但 30 秒内没有发布可捕获的 AppKit 窗口`. It was cleaned up; no leftovers. `CORE_OK`, LaunchDaemons unloaded, WindowServer 51054 still `Ss`, `comp_occ=8793`. Safari was not started.

### Conclusion

The uncapturable-window / empty final-composite problem is **not** explained by an iOS lock screen. The diagnosis in §27 still stands. Desktop repair / session rebuild remains a later option if approved; it was not run this turn.

## 29. User unlock restored desktop capture; net10b Safari still has no visible window (2026-11-10 19:13–19:16 CST)

### Context

The user unlocked the session and reported that windows were visible. `macos_gui.sh repair-desktop` had been approved earlier but was **not started**; the follow-up steering cancelled it, and `rebuild-desktop-session` was not run either. No device reboot, no 停止 macOS, no new dylib. net10b stayed deployed: `/var/mnt/rootfs/usr/local/lib/libmachook.dylib`, inode 23302283, size 1477120, mtime Nov 10 18:10 CST (the post-net11d rollback), arm64e CDHash `cffee750f072127a4fb0cb648357f7abe9c0868e` (sha1 `b027e89499f34b6bd3c74b55b66a9439f1b8e1d7`).

Health at 19:13:14 CST / 03:13:15 PST: up 8:16 from the 10:57 CST / 18:57 PST Nov 9 button reset. `CORE_OK`. WindowServer 51054 `Ss` (~168 MB, etime 51:16). macwshostd 416, SpringBoard 391. Finder 51453 / Dock 51460 / SystemUIServer 51468 / ControlCenter 51476 / OSXvnc 51484 all `Ss`. `macwsinputd` 51354 / `macwsdisplayd` 51356. WebKit LaunchDaemons unloaded. No leftover Safari. `comp_occ=11329`, `free_mb=256`.

### Step 1 — capture and Activity Monitor recovered

`/var/mobile/net10b-recheck/ws-postunlock-verify.sh` at 19:13:42–19:13:48 CST / 03:13:42–03:13:49 PST:

- Desktop `screencapture` succeeded almost immediately (`DESK_CAPTURE_OK`): `/var/mnt/rootfs/tmp/ws-repair-desktop.png` and `/var/mobile/ws-repair-desktop.png`, 9,188,051 bytes.
- Activity Monitor: `ok=yes launched-pid=51885 message=已启动 activity-monitor，AppKit 窗口已进入 DisplayStream 列表`, RSS 116384 (healthy, not the hung ~35 MB), etime 4 s. It was then SIGKILL'd; none left.
- `CORE_OK`, WindowServer 51054 still `Ss`, LaunchDaemons unloaded.

The PNG was pulled to `/workspace/ws-unlock-desktop.png` (clearer name: repair was skipped) and also saved as `/workspace/ws-repair-desktop.png`. Visual check: a real Aqua desktop — Finder menu bar (File / Edit / View / Go / Window / Help), the forest-lake wallpaper, the Data icon, and the Dock. Menu-bar clock 03:13. This is the first successful chroot desktop capture since the session wedge in §26–§28, and it happened after the user's unlock, without repair-desktop or a session rebuild.

### Step 2 — one guarded net10b Safari (example.com, then baidu)

`/var/mobile/net10b-recheck/safari.sh` started at 19:14:14 CST / 03:14:14 PST (wrapper pid 51928). `comp_occ` 11318 → 11832 after example → 12197 after baidu (under the emergency gate). Finder / Dock / SystemUIServer / ControlCenter / OSXvnc were `Ss` and were not SIGSTOP'd.

- Launch-path: `ok=no launched-pid=0 message=custom-path 已启动，但 30 秒内没有发布可捕获的 AppKit 窗口`.
- Open `example.com`: `ok=yes launched-pid=52091 message=已由目标应用接收 1 个文稿`. `CAPTURE_OK` `/var/mobile/net10b-recheck-example.png` (9,143,716 bytes) at 19:14 CST.
- Open baidu: `ok=yes launched-pid=52334 message=已由目标应用接收 1 个文稿`. `CAPTURE_OK` `/var/mobile/net10b-recheck-baidu.png` (9,144,022 bytes) at 19:15 CST.
- Script `PROCS after example` and `PROCS after baidu` listings were empty: Safari / WebContent / Networking had already exited by the time `ps` ran. No new Safari or WebContent `.ips` was written for this run.
- Cleanup: no leftover Safari/WebKit processes, `CORE_OK`, macOS WebKit LaunchDaemons unloaded, `MEM_AFTER` `free_mb=186` `comp_occ=12197`. WindowServer 51054 still `Ss` (~169 MB) at 19:16:42 CST.

Both captures and `/workspace/net10b-recheck-example.png` / `/workspace/net10b-recheck-baidu.png` were pulled and inspected. They show the same Finder desktop as the unlock shot (menu bar still Finder, wallpaper, Dock). Menu-bar clocks are 03:14 and 03:15, so the captures are live — but **no Safari window is on screen**.

### Paint log

`/var/mobile/net10b-recheck-webkit-paint.log` (also under field-artifacts). Three Safari processes ran:

- 51989 (initial launch-path): `bootstrap_look_up 'com.apple.CARenderServer' -> 'com.apple.macosbooter.CARenderServer' -> 0 port=0x2d0b`; `CAContext remoteContextWithOptions` → contextId 2895057297; two `contextWithCGSConnection:118035`.
- 52091 (example.com): same CARenderServer rewrite, port=0x2e0b; `remoteContextWithOptions` → contextId 4163271898; nine `contextWithCGSConnection:118315`.
- 52334 (baidu): rewrite, port=0x2d0b; `remoteContextWithOptions` → contextId 2786964797; nine `contextWithCGSConnection:95495`.

WindowServer 51054 `CALayerHost setContextId` logged many of those non-zero context IDs. 31 WebContent processes, 3 Networking, and 3 SafariSandboxBroker started and only logged `CAContext hooks` (hook install). **No WebContent `remoteContextWithOptions` or `contextWithCGSConnection`.** GPU was not requested.

So Safari can still talk to CARenderServer and create CAContexts, and helpers do start, but WebContent still never hosts a layer — and unlike the 16:57 CST run, the Safari UI itself never appeared in the chroot screenshot (the launch-path probe also never saw a capturable AppKit window, even while 51989 was creating CAContexts during the 30 s wait).

### What was not done

No `repair-desktop` or `rebuild-desktop-session`. No new libmachook (296254e was not deployed). No device reboot and no 停止 macOS. The 16:57 CST Favorites → blank-white visual result could not be re-checked, because no Safari window was on screen.

### Paint-path conclusion

Session-level capture and new-window publish are recovered (desktop + Activity Monitor). net10b Safari still does not present a visible, capturable window in this session; WebContent still never creates a CAContext. Do not deploy the logging dylib until a Safari window is actually on screen again.

### Still open

Why Safari's CAContexts and helper processes do not yield a DisplayStream/AppKit window that `screencapture` can see, and why those processes are already gone by the post-open `ps`. Next recheck of net10b Safari (or a logging deploy) should wait until that window is visible; do not reintroduce SkyLight MSHooks, extra Safari CAContext IMPs, or constructor main-queue `dispatch_after`.

## 30. Missing macOS IconServices caused the Safari SIGABRT; window is visible again, content still white (2026-11-10 19:23–19:30 CST)

### Context

After the user's unlock, desktop capture and Activity Monitor window publish worked (§29), but the 19:14 CST net10b Safari run left only the Finder desktop in `/workspace/net10b-recheck-*.png`. net10b stayed deployed (inode 23302283, size 1477120, mtime Nov 10 18:10 CST, arm64e CDHash `cffee750f072127a4fb0cb648357f7abe9c0868e`). No new dylib, no reboot, no 停止 macOS, no `repair-desktop` / `rebuild-desktop-session`.

### Why there was no on-screen Safari window

The 19:14 launch-path message (`custom-path 已启动，但 30 秒内没有发布可捕获的 AppKit 窗口`) was a mis-report. Safari pid 51989 did publish an AppInput endpoint and hostd sent `application-reopen`, then died about 3 s after launch with `exit-status=6`. `RequestApplicationReopen` keeps that waitpid status local, so `LaunchRootExecutable` still saw `exitStatus=-1` and printed the 30-second timeout string. The reaper then logged `supervisor:path-or-process-absent`.

Open-documents does not require a visible window (only the AppInput endpoint plus an ack). It therefore started fresh pids 52091 and 52334, both of which accepted the `.webloc` and then died with `waitpid:signal-6` (~8 s later). The script's empty `PROCS` listings were because those processes were already gone, not because the Cryptexes Safari path evaded the matcher. Helpers died with the Safari abort; they did not disappear independently.

No new Safari/WebContent `.ips` was written in DiagnosticReports or CrashReporter for this abort (chroot ReportCrash appears not to have captured it). The evidence is stdout in `/var/mobile/Library/Logs/CustomApp.host.log` (O_TRUNC per launch, so only the last pid remains): Safari 52334 ended with

`Assertion failed: (result), function NSISIconImageRepGetCGImage, file NSISIconImageRep.m, line 102.`

That is SIGABRT in IconServices' AppKit image-rep path while building chrome (tabs / file icons), not a missing DisplayStream space or an off-screen/minimized window. Metrics sidecars for those pids were never left behind (`current-generation=0`). Activity Monitor at 19:13 CST had published a visible window (RSS 116384) because it does not take this Safari icon assertion path.

`CGWindowList` / spaces were not needed as the primary explanation: Safari never stayed alive long enough to occupy a space.

### Missing macOS IconServices after the WindowServer-only recover

`macos_gui.sh` starts Ventura `iconservicesd` (uid 240, `com.macwsguide.iconservicesd`, Mach name `com.apple.macosbooter.iconservices.store`) and `iconservicesagent runAsRoot` (`com.macwsguide.iconservicesagent`, `com.apple.macosbooter.iconservices`). libmachook already remaps `com.apple.iconservices` / `.store` to those names. Both jobs were started at 11:03 CST (logs under `/var/jb/var/mobile/iconservices*.log`) and were **not loaded** by 19:23 CST: `launchctl print` said the services were absent, and the logs had not been written since 11:03. The 18:22 WindowServer recover only `launchctl load`'d Finder / Dock / SystemUIServer / ControlCenter / OSXvnc, not IconServices.

The only live IconServices process was iPadOS `com.apple.iconservices.iconservicesagent` pid 1724 (boot-age, uid `_iconservices`). Safari's remapped lookup therefore had no macOS listener, icon rasterization returned nothing, and `NSISIconImageRepGetCGImage` aborted.

Comparison with the 16:57 CST success (pid 38235, ~360 MB, DisplayStream window, Favorites then example.com / baidu with a white content area, cleaned with SIGTERM): that run was in the original 11:03 session while the macOS IconServices jobs still existed. Even then, 16:52–16:53 CST had three prior SIGABRT launches (pids 36668, 36740, 37269) before 38235 published `window-ready` at generation 1. So the abort can also race when the daemons are up; after 18:22 they were simply gone, and every 19:14 attempt aborted.

### Step — reload existing IconServices jobs, then one guarded Safari

At 19:27:15 CST / 03:27:15 PST the existing plists were `launchctl load`'d (store tree already uid 240 and marker `wrapgl-trust-v1`; no cache wipe). Both jobs came up immediately:

- `iconservicesd` pid 53067, uid 240, `Ss`, ~25 MB, last exit never
- `iconservicesagent runAsRoot` pid 53071, root, `Ss`, ~27 MB, last exit never

iOS agent 1724 was left alone. No new hook dylib.

One guarded net13 Safari (`/var/mobile/net13/safari.sh`, started 19:29:03 CST / 03:29:03 PST via nested sudo so chroot I/O worked; open-url was withheld unless launch-path returned `ok=yes`):

- 19:29:07 CST / 03:29:07 PST: launch pid 53285. Endpoint-driven reopen sent. At 03:29:18, `application-reopen result=window-ready previous-generation=0 current-generation=1` and `launch-app window-ready path=DisplayStream`. RSS was already ~179 MB at +11 s, then ~389 MB (398528 KB) once pages were opened — the healthy ~360 MB class, not the hung ~51 MB class.
- The probe line was `ok=yes launched-pid=53285 message=不支持的操作`; hostd's window-ready record is the authoritative witness. Same pid reused for both documents (`appkit-accepted`).
- CustomApp.log had the usual empty-NSURL and Metal-library-format warnings, plus "Spell server connection invalidated", but **no** `NSISIconImageRepGetCGImage` assertion.
- 18 WebContent + Networking + SandboxBroker stayed up (WebContent only logged `CAContext hooks`). Safari did CARenderServer rewrite (port `0x3d0b`), `remoteContextWithOptions`, and nine `contextWithCGSConnection`. GPU was not requested.
- Cleanup was SIGTERM (`waitpid:signal-15`). No leftovers. `CORE_OK`. LaunchDaemons unloaded. `comp_occ` 12171 → 20926, under the emergency gate. IconServices jobs still running afterwards.

### Screenshots

`/workspace/net13-example.png` (861,269 bytes, 2732×2048, clock 03:29): a real Safari window is frontmost (Safari menu bar), the address bar shows `example.com`, two "Untitled" tabs, and a **blank white content area**. `/workspace/net13-baidu.png` (859,398 bytes): the same window after the baidu open; a third tab "B Untitled" appeared; the content area is still white.

This reproduces the 16:57 CST paint result: Safari leaves the start/Favorites chrome, the URL is accepted, and WebContent still never creates a CAContext, so the page body stays white.

### What was not done

296254e / net11* was not deployed. No SkyLight/CGS/SLS MSHooks and no extra Safari CAContext IMPs. No device reboot and no 停止 macOS.

### Paint-path conclusion

The missing on-screen window in §29 was Safari aborting in IconServices after the post-WindowServer recover left `com.macwsguide.iconservicesd` / `iconservicesagent` unloaded. With those existing jobs loaded again, net10b Safari publishes a capturable window. The remaining WebKit issue is unchanged: WebContent never hosts a remote layer, so example.com and baidu stay white after leaving Favorites. A WebContent-only logging dylib (296254e) is now a reasonable next step, because there is once again a visible Safari window to observe.

## 31. WebContent-only logging (net14): window still publishes, content still white, no CAContext in WebContent (2026-11-10 19:36–19:41 CST)

### Context

After §30 reloaded the existing macOS IconServices jobs, net10b Safari published a capturable window again (pid 53285, `/workspace/net13-*.png`), but pages still left Favorites / Start Page with a white content area and WebContent still never called `remoteContextWithOptions`. A WebContent-only logging pass was now allowed because a visible Safari window existed. Hard limits this turn: no SkyLight / CGS / SLS hooks; no extra CAContext IMPs in Safari and no change to Safari's window-publish / `remoteContextWithOptions` path just to log `serverPort`; no constructor main-queue `dispatch_after`; no new exported libmachook remapper symbols; keep IconServices loaded; no reboot and no 停止 macOS; backup, smoke with `launchdchrootexec`, and roll back immediately to net10b (CDHash `cffee750f072127a4fb0cb648357f7abe9c0868e`) if smoke failed or Safari again failed to publish a window, with no second variant; one guarded Safari run only after smoke passed.

296254e (`fix(ipad14.5): log WebContent paint path without SkyLight MSHooks`) was already on the branch but was not a compliant deploy: it called `macws_safari_log_remote_port()` from Safari's `remoteContextWithOptions` hook, installed `setAllowsCGSConnections:` globally (an extra CAContext IMP in Safari), and retried WebContent `remoteContextWithOptions` with a looked-up CARenderServer port. net11d had deployed that tree as-is and was rolled back when the window failed to publish. This turn stripped those Safari-path and retry behaviors and left observational logging on existing hook points plus WebContent-only paths after those processes exist.

### Deploy and smoke

Built thin slices on the Mac Pro (Theos), `misc/set_macos_version.py` (LC_BUILD_VERSION macOS 13.0), `/usr/local/Cellar/ldid/2.1.5_2/bin/ldid -S` twice, `codesign -f -s -`, then ldid twice again. `_macws_private_bootstrap_service_name` is not exported (168 T symbols per slice).

- arm64e `libmachook.dylib`: 1,479,040 B, CDHash `c1b7b43906bad65e33ba3d832f97e509aafb0bd0`, CandidateCDHash sha1 `bf2937513363df4b89d9f08e8e8bf9972fea04ac`, file sha1 `dfa42390b0d41e343c5064649cbcaea0d6f0e5c4`
- arm64 `libmachook_arm64.dylib`: 1,486,352 B, CDHash `4b8997065d588e1a789ae06ea69181d9379b2f3e`, CandidateCDHash sha1 `54219030b7859b601027f55635abeccf9b1af511`, file sha1 `24d691fef0a9c3999c730a04b58df17a6436724a`

`jbctl trustcache add` accepted the 40-character CDHashes and sha1 candidates; the 64-character `CandidateCDHashFull sha256` values failed as usual and were ignored.

Deploy at 19:36:37 CST / 03:36:37 PST via nested `/var/jb/usr/bin/sudo -n sh /var/mobile/net14/deploy.sh`. Live net10b (inode 23302283, 1,477,120 B, mtime Nov 10 18:10 CST) was backed up as rollback tag **`pre-net14-20261110-193637`** (rootfs sidecar inode 23352808 and `/var/mobile/macws-rollback/libmachook.dylib.pre-net14-20261110-193637`). Fresh inodes: rootfs `libmachook.dylib` 23352813 and `libmachook_arm64.dylib` 23352814, also copied to `/var/jb/usr/macOS/lib/`.

Smoke at 19:36:54 CST / 03:36:54 PST: `run_bash.sh` / `launchdchrootexec` inserted `/usr/local/lib/libmachook.dylib` (arch=arm64e) and printed `SMOKE_OK` with rc=0. No Bus error. IconServices 53067 / 53071 stayed `Ss`. `CORE_OK`. Because smoke passed, rollback was not used.

### One guarded Safari run

`/var/mobile/net14/safari.sh` started 19:40:04 CST / 03:40:04 PST via nested sudo (chroot I/O). Open-url was withheld unless launch-path returned `ok=yes`. IconServices jobs were still loaded beforehand.

- 19:40:08.413 CST / 03:40:08.413 PST: launch-path accepted custom-path pid **54348**. Endpoint-driven reopen sent immediately.
- 19:40:18.766 CST / 03:40:18.766 PST: `application-reopen result=window-ready previous-generation=0 current-generation=1` and `launch-app window-ready path=DisplayStream`. Probe line was `ok=yes launched-pid=54348 message=不支持的操作`; hostd's window-ready record is the authoritative witness (`LAUNCH_WINDOW_OK`).
- 19:40:21.629 CST / 03:40:21.629 PST: example.com `.webloc` `appkit-accepted`. 19:40:38.458 CST / 03:40:38.458 PST: baidu.com likewise, same pid.
- Safari RSS grew to ~413 MB (423440 KB) — the healthy class, not the hung ~51 MB class. At the baidu diag, 20 WebContent processes plus Networking 54363 and SandboxBroker were still `Ss`, but every WebContent RSS stayed ~28 MB (28704–28848 KB) for the whole run, including the two helpers spawned around the baidu open (54755, 54758). GPU was not requested.
- CustomApp.host.log had the usual empty-NSURL, Metal-library-format, openAndSavePanelService, and spell-server warnings; **no** `NSISIconImageRepGetCGImage` assertion.
- Cleanup was SIGTERM at 19:40:55.080 CST / 03:40:55.080 PST (`waitpid:signal-15`). No leftover Safari/WebContent (only the unrelated iOS WebContent pid 703). `CORE_OK`. LaunchDaemons unloaded. `comp_occ` 20811 → 20773, under the emergency gate. IconServices 53067 / 53071 still `Ss` afterwards. Live dylib remained net14 (inode 23352813).

### Screenshots

`/workspace/net14-example.png` (859,650 bytes, 2732×2048, menu-bar clock 03:40): Safari is frontmost (Safari menu bar), the address bar shows `example.com`, tabs are "Start Page" and "E Untitled", and the content area is **blank white**. `/workspace/net14-baidu.png` (862,343 bytes): the same window after the baidu open; the address bar shows `baidu.com`, a third tab "B Untitled" appeared, and the content area is still white.

This matches the 16:57 CST and net13 visual result: the UI leaves Favorites / Start Page, the URL is accepted, and the page body never paints.

### Paint / IPC logs

Appended logs (chroot `/tmp`, also copied under `/var/mobile/net14/`):

- `/tmp/macws-webkit-paint.log` (215 lines in the post-run full copy)
- `/tmp/macws-webkit-init.log` (110 lines)
- `/tmp/macws-xpc-app.log` (108 lines)
- `/tmp/macws-sandbox-allow.log` (24 lines)

Safari 54348: `bootstrap_look_up 'com.apple.CARenderServer' -> 'com.apple.macosbooter.CARenderServer' -> 0 port=0x3b0b` succeeded; ten `CAContext contextWithCGSConnection:102159` calls produced contextIds that WindowServer 51054 later hosted via `CALayerHost setContextId` (chrome). One Safari `CAContext remoteContextWithOptions -> contextId=638302839 portNumber=0 keys=0` (empty options dict). That contextId was **never** hosted by any `setContextId`. `screencapture` used the same empty-options `remoteContextWithOptions` shape; this is not treated as the WebContent compositing-port handoff. CARemoteLayerServer IMPs were not installed in Safari (WebContent-only), so Safari `serverPort` was not logged, per the guardrail.

WebContent (20 pids): each logged only `CAContext hooks ... wc=1` and `WebContent paint hooks installed; no ctor dispatch_after`. **No** `CAContext remoteContextWithOptions`, **no** dropped-port line, **no** `CALayer setGeometryFlipped` drawing-area constructor breadcrumb, **no** `CARemoteLayerServer sharedServer` / `serverPort`, **no** `setAllowsCGSConnections`, **no** `sandbox_check DENY` in the paint log. Networking 54363 only logged hook install.

The XPC event wrapper in `macws_xpc_wrap_app_event_handler` (intended to log `xpc event begin/end` on the connection that `InitializeWebProcess` / `CreateWebPage` ride, plus the first two WebContent heartbeats) never fired in any helper. Safari did create the remapped services: 20× `created activate=com.apple.macosbooter.WebKit.WebContent`, plus Networking and SandboxBroker, and `sandbox-allow.log` recorded the matching `com.apple.webkit.extension.mach` extensions. `xpc_add_bundle` of the host-visible proxy paths still returns `access=-1 errno=62` (ELOOP on in-chroot `/var/jb`, previously known); `add_bundle_done` still ran and the helpers did spawn, so this is not a new spawn failure. No `xpc_error` lines.

`macws-webkit-init.log` only shows hook *installation* in each helper (`InitializeWebKit2 raw=...`, `hooked InitializeWebKit2 via offset-unsigned ... orig=0x0`, same for `runInitializationCode`). There is no `InitializeWebKit2 call` / `orig returned` and no `runInitializationCode call`. That is the net8d pattern already commented in `mac_hooks.m`: an unsigned shared-cache offset hook can log install with `orig=0x0` while the real function keeps running unhooked. Absence of those call lines is therefore **not** proof that `InitializeWebKit2` never ran. The independent, installed WebContent breadcrumbs (drawing-area `setGeometryFlipped`, `remoteContextWithOptions`, XPC event begin) still never appeared, and RSS never left the ~28 MB prewarm class.

### What was not done, and why there is no fix deploy this turn

The logs do **not** show a dropped compositing port reaching WebContent, because WebContent never called `remoteContextWithOptions` at all. The prepared port-retry is therefore not justified and was left disabled. No per-app env hacks, no second dylib variant, and no second Safari run. net14 stays deployed (observational only; window publish still works). Rollback copies remain at `pre-net14-20261110-193637` (net10b CDHash `cffee750f072127a4fb0cb648357f7abe9c0868e`). No SkyLight hooks, no extra Safari CAContext IMPs, no constructor `dispatch_after`, no remapper export, no reboot, no 停止 macOS. IconServices jobs were left loaded.

### Paint-path conclusion

Safari chrome still talks to CARenderServer and publishes a capturable AppKit window. example.com and baidu.com are accepted, but the content area stays white because WebContent never constructs a TiledCoreAnimation drawing area and never creates a CAContext. The new logging narrows the gap to *before* layer hosting: UI-process connections to `com.apple.macosbooter.WebKit.WebContent` are created and helpers stay up, yet no XPC events (and no drawing-area / remote-context breadcrumbs) were observed inside WebContent. Next work should follow that IPC path, not a CAContext port retry, and should not reintroduce Safari-path CAContext IMPs or constructor main-queue `dispatch_after`.

## 32. WebKit xpc_main Mach-listener fallback orphans Safari's connection; after exec, XPC_FLAGS is 0x0 (2026-11-10 19:52–19:54 CST)

### Context

net14 (§31, commit 35fb9b2, arm64e CDHash `c1b7b43906bad65e33ba3d832f97e509aafb0bd0`) stayed deployed with a visible Safari window and ~20 living WebContent helpers, but no `InitializeWebProcess` / `CreateWebPage` / drawing-area / CAContext / compositing-port logs inside WebContent. Safari created 20 `com.apple.macosbooter.WebKit.WebContent` connections. The next step was that IPC path, not a CAContext port-retry.

Hard limits unchanged: IconServices kept loaded; no reboot and no 停止 macOS; no SkyLight / CGS / SLS hooks; no extra Safari CAContext IMPs; no constructor main-queue `dispatch_after`; no new exported remapper symbols. Backup and smoke before deploy. If smoke failed or Safari lost its visible window, roll back immediately to net14 (`c1b7b439…`) — the last build with a visible Safari window — and stop, with no second variant.

### What the source showed

`macws_xpc_main` treated WebContent / Networking / GPU / SandboxBroker like HIServices: when `XPC_SERVICE_NAME` was `com.apple.macosbooter.WebKit.*` (or the older `com.macwsguide.webkit.*` names) and the helper ran as root, it created a **new** launchd Mach listener on that name and called `dispatch_main()`, never reaching stock `xpc_main`.

That conflicts with the Application-type design already documented for these proxies (`ServiceType=Application`, `RunLoopType=_NSApplicationMain`, `_MultipleInstances`, CFBundleIdentifier `com.apple.macosbooter.WebKit.WebContent`). `xpc_add_bundle` is supposed to let xpcproxy hand a one-shot receive right through `XPC_FLAGS` / `XPC_*`, and `ViewBridgeChrootProxy` is written to copy those variables across chroot+exec. A second `bootstrap_check_in` of the same name cannot inherit the port Safari already holds. That would leave helpers sitting with no peer — matching the net14 symptoms (helpers `Ss` at ~28 MB, no XPC events inside WebContent, no `xpc_error` on the UI-process connections).

The net9 interpretation (helpers staying up meant events already arrived on the main queue) was not supported once net14 logged `xpc event begin` on that wrap and saw zero hits.

### System-level change (net15)

Skip the HIServices-style Mach-listener fallback for WebKit helpers. Call stock `xpc_main` so it can consume an inherited xpcproxy connection, and before the service's connection handler runs, call `xpc_connection_set_target_queue(conn, dispatch_get_main_queue())` from this dylib (not a shared-cache interpose) so later `InitializeWebProcess` / `CreateWebPage` messages meet WebKit's isMainThread assertion without hooking `InitializeWebKit2` and without a constructor `dispatch_after`. Log the chosen path (`service`, `XPC_FLAGS`, uid) and each accepted connection to `/tmp/macws-webkit-init.log`. No new exported symbols.

Built on the Mac Pro with Theos `FINALPACKAGE=1 STRIP=0 OPTFLAG=-O2`, then `misc/set_macos_version.py` (LC_BUILD_VERSION macOS 13.0), ldid `-S` twice, `codesign -f -s -`, ldid twice again. `_macws_private_bootstrap_service_name` is not exported (168 T symbols per slice).

- arm64e `libmachook.dylib`: 1,479,232 B, CDHash `86f08ac4b4fa875ec61799ce7e991a0576b8b57b`, CandidateCDHash sha1 `2b0e9195c0f12b00f4d0b76c0c42a32e0692c8d0`, file sha1 `7825185d2df86448f5a52eafb823d1ff5d3df414`
- arm64 `libmachook_arm64.dylib`: 1,486,528 B, CDHash `865811c9ba318d6a56c30c46ebc121428381af62`, CandidateCDHash sha1 `9f94dcbcc8b11b6b20c30a652cbbd6ec7f2c088a`, file sha1 `999fc72fcad5d64255e597c3f7c1f414c3c53dc2`

### Deploy, smoke, one guarded Safari

Deploy at 19:52:16 CST / 03:52:16 PST via nested `/var/jb/usr/bin/sudo -n sh /var/mobile/net15/deploy.sh`. Live net14 (inode 23352813, CDHash `c1b7b43906bad65e33ba3d832f97e509aafb0bd0`) was backed up as rollback tag **`pre-net15-20261110-195216`**. Fresh inodes: rootfs `libmachook.dylib` 23377884 and `libmachook_arm64.dylib` 23377885, also copied to `/var/jb/usr/macOS/lib/`. 40-character CDHashes and sha1 candidates were trusted; 64-character `CandidateCDHashFull sha256` values failed as usual.

Smoke at 19:52:21 CST / 03:52:21 PST: `run_bash.sh` / `launchdchrootexec` inserted `/usr/local/lib/libmachook.dylib` (arch=arm64e) and printed `SMOKE_OK` with rc=0. IconServices 53067 / 53071 stayed `Ss`. `CORE_OK`. Rollback was not used.

One guarded Safari (`/var/mobile/net15/safari.sh`, started 19:52:27 CST / 03:52:27 PST via nested sudo; paint/init/xpc/sandbox logs truncated first; open-url withheld unless launch-path returned `ok=yes`):

- 19:52:31.950 CST / 03:52:31.950 PST: launch-path accepted custom-path pid **55509**.
- 19:52:43.726 CST / 03:52:43.726 PST: `application-reopen result=window-ready previous-generation=0 current-generation=1` and `launch-app window-ready path=DisplayStream`. Probe line was `ok=yes … message=不支持的操作`; hostd's window-ready record is authoritative (`LAUNCH_WINDOW_OK`).
- 19:52:46.748 CST / 03:52:46.748 PST: example.com `appkit-accepted`. 19:53:03.764 CST / 03:53:03.764 PST: baidu.com, same pid.
- Safari RSS ~445 MB (455552 KB). About 20 WebContent helpers plus Networking stayed `Ss`, but every WebContent RSS stayed ~27 MB (27552–27760 KB). GPU was not requested. No `NSISIconImageRepGetCGImage` assertion.
- Cleanup SIGTERM at 19:53:20.297 CST / 03:53:20.297 PST (`waitpid:signal-15`). No leftover Safari/WebContent (only iOS WebContent pid 703). `CORE_OK`. `comp_occ` 20699 → 20707, under the emergency gate. IconServices still `Ss`. Live dylib remained net15 (inode 23377884). The visible window was not lost, so net14 rollback was not used.

### Screenshots

`/workspace/net15-example.png` (856,009 bytes, 2732×2048, menu-bar clock 03:53): Safari is frontmost, the address bar shows `example.com`, tabs are "Start Page" and "E Untitled", and the content area is **blank white**. `/workspace/net15-baidu.png` (858,678 bytes): after the baidu open, the address bar shows `baidu.com`, a third tab "B Untitled" appeared, and the content area is still white. Same visual result as net13 / net14.

### IPC result — the assumed inherited context is not there

Safari still created the remapped services (22× `created activate=com.apple.macosbooter.WebKit.WebContent`, plus Networking and SandboxBroker). `xpc_add_bundle` still returns `access=-1 errno=62` (ELOOP); `add_bundle_done` still ran and helpers spawned. No `xpc_error`. Sandbox extensions for `com.apple.macosbooter.WebKit.*` were issued. Helpers are the right kind of process (`com.apple.WebKit.WebContent`, root, `pthread_main=1` at `xpc_main`).

Every helper logged:

`xpc_main WebKit inherited path service=com.apple.macosbooter.WebKit.WebContent flags=0x0 uid=0 euid=0 (no Mach-listener fallback)`

(`XPC_FLAGS` is present and equal to `0x0`, not unset). **Zero** `xpc_main accepted connection=` lines. Stock `xpc_main` therefore never delivered a peer to the service handler. There were still no `xpc event begin/end`, no `InitializeWebKit2 call` / `runInitializationCode call` (install still `orig=0x0`, same net8d caveat), no drawing-area `setGeometryFlipped`, no WebContent `remoteContextWithOptions` or dropped-port log, and no `sandbox_check DENY`. Safari chrome still created CGS CAContexts and looked up CARenderServer (`port=0x3c0b`).

So the Mach-listener fallback was the wrong shape, but switching to stock `xpc_main` cannot help until a real xpcproxy receive right exists after chroot+exec. With `XPC_FLAGS=0x0` there is nothing for stock `xpc_main` to consume, and Safari's already-created client connections have no in-process peer. That is why UI-process messages never show up inside WebContent. The `set_target_queue(main)` bounce and the shared-cache `InitializeWebKit2` hook were not reached. This is not a per-app env issue; the generic gap is how the Application-type launch context (or `XPC_FLAGS`) is lost across the setuid chroot proxy.

### What was not done

No second dylib variant. net15 stays deployed (window publish still works; the change is the WebKit `xpc_main` path plus path/flags logging). Rollback copies remain at `pre-net15-20261110-195216` (net14 CDHash `c1b7b43906bad65e33ba3d832f97e509aafb0bd0`); net10b (`cffee750f072127a4fb0cb648357f7abe9c0868e`) is still on the device as an older rollback. No SkyLight hooks, no extra Safari CAContext IMPs, no constructor `dispatch_after`, no remapper export, no reboot, no 停止 macOS. IconServices jobs were left loaded.

### Paint / IPC conclusion

Safari still publishes a capturable window and accepts example.com / baidu.com, but the content area stays white because WebContent never gets a peer XPC connection, so it never runs a drawing-area constructor or creates a CAContext. The earlier Mach-listener fallback could not attach to Safari's existing send right. net15 confirmed that after the proxy `execve`s stock WebContent, `XPC_SERVICE_NAME` is the expected `com.apple.macosbooter.WebKit.WebContent` but `XPC_FLAGS=0x0`, and stock `xpc_main` never accepts a connection. Next work should restore a real xpcproxy (or equivalent) receive right across the chroot+exec transition, not retry CAContext ports and not reintroduce a competing Mach listener on the same name.

## 33. WebKit proxy reconstitutes XPC_FLAGS=8019, but the child still sees 0x0 and has no peer (2026-11-10 20:07–20:12 CST / 04:07–04:12 PST)

### What was tried

net15 left stock `xpc_main` on the inherited path and showed that after the setuid chroot proxy `execve`s the stock image, `XPC_FLAGS` is the string `0x0`. On a real Mac, Application-type WebKit helpers (WebContent, Networking, GPU, Safari.SandboxBroker) run with `XPC_FLAGS=8019` for their entire lifetime; stock `xpc_main` does not clear that value. The proxy already copied `XPC_*` byte-for-byte, so the zero flags were already present on the first image (iOS xpcproxy → setuid iPhoneOS-platform proxy).

net16 is a generic, proxy-side reconstitution: for Application-type WebKit targets only, if incoming `XPC_FLAGS` is unset or zero (`0`, `0x0`, `0x00`), replace it with `8019` in the `execve` environment so Ventura `xpc_main` can consume an inherited one-shot receive right. Mach ports and fds still survive the raw chroot+exec; the change does not `bootstrap_check_in` the public `macosbooter.WebKit.*` name, does not add a CAContext port-retry, and does not touch libmachook (still net15). The proxy logs incoming vs outgoing flags to `/tmp/macws-webkit-proxy.log` (host path `/var/mnt/rootfs/tmp/...`) with raw syscalls. Unit tests cover zero-flag replacement, preservation of a real `8019`, and leaving non-WebKit `0x0` alone.

### Deploy

Backup tag `pre-net16-20261110-201123` (20:11:23 CST / 04:11:23 PST) saved the live net15-era proxies in both trees (`/var/jb/usr/macOS/Frameworks/WebKit.framework/...` and `/var/mnt/rootfs/usr/local/libexec/macws-xpc/WebKit.framework/...`) plus `/var/mobile/macws-rollback/proxies.pre-net16-20261110-201123/`. Those backups restore:

- WebContentProxy CDHash `181c64a699f325458491544c309f5d397bdd5007`
- WebNetworkingProxy `d1e70795621022ea4a141c89e522339ee24f5250`
- WebGPUProxy `0f8b2ea9772122eb5e6777dd6d9ff504503bb21b`
- SafariSandboxProxy `e8b38f8989b73ef63119a70b3ecefcde64004885`

libmachook was not replaced (rootfs inode 23377884, arm64e CDHash `86f08ac4b4fa875ec61799ce7e991a0576b8b57b`, sha1 candidate `2b0e9195c0f12b00f4d0b76c0c42a32e0692c8d0`). New thin arm64e proxies are setuid 4755, 85760 B, fresh inodes (jb WebContent 44288 / rootfs 23409031), trusted with 40-char sha1 and sha256 CDHashes (64-char CandidateCDHashFull skipped). Deployed CDHashes:

- WebContentProxy `5d3a9cec32167ad654d0796efaf141cbab6345a9` (sha1 `f5d11cd39fd652532a43d42378f9d4f3e5850598`)
- WebNetworkingProxy `e9c0baa3d66e16d4ded68e2937f0449aaaf6798b` (sha1 `035e94ddcf41460e368a06d54c3bfc91e8c1a17f`)
- WebGPUProxy `b9d40e10881077c7ba82a5a7a3b8f1f5d5427220` (sha1 `088d1378cadae3c944add5abba18e1bbb6bbc5ef`)
- SafariSandboxProxy `6503dc2e9a3cebea1195a4dc9596e37c03d0ec6c` (sha1 `50bbda54244f871bcc3bbcabeb7f9604927fad83`)

Smoke at 20:11:29 CST / 04:11:29 PST: `run_bash.sh` printed `SMOKE_OK` (rc=0). IconServices jobs 53067 (uid 240) and 53071 (`runAsRoot`) stayed loaded; iOS agent 1724 was left alone. No reboot, no 停止 macOS.

### Safari run (one guarded attempt)

20:11:45–20:12 CST / 04:11:45–04:12 PST, Safari pid 56937. `launch-path` returned `ok=yes` (probe message `不支持的操作`, same as earlier runs); the script treated that as `LAUNCH_WINDOW_OK` and opened documents. example.com and baidu.com were both accepted (`已由目标应用接收 1 个文稿`). Safari RSS grew to ~471 MB (481760–482032 KB), the healthy visible-window class. 24 WebContent helpers plus Networking and SandboxBroker stayed `Ss` at ~27 MB each; GPU was not requested. `comp_occ` 20554 → 20793 (no watchdog trip). Cleanup left CORE_OK (WindowServer 51054, macwshostd 416, SpringBoard 391), WebKit LaunchDaemons unloaded, no leftover macOS Safari/WebContent. Screenshots: `/workspace/net16-example.png` and `/workspace/net16-baidu.png` (2732×2048, clock 04:12 PST), Safari in front, URLs example.com / baidu.com, Start Page + E Untitled (+ B Untitled on baidu), content area still white. Because the window stayed capturable, net16 was **not** rolled back.

### Field result

The proxy rewrite fired as intended: 24 WebContent + 1 Networking + 1 SandboxBroker lines of `proxy XPC_FLAGS incoming=0x0 outgoing=8019` (confirms iOS xpcproxy already hands the first image `0x0`, and that we replaced it on the `execve` env). Inside the stock helpers, libmachook's inherited-path log still showed `flags=0x0` on all 25 `xpc_main` entries (24 WebContent + 1 Networking). **Zero** `xpc_main accepted connection=` lines. No `xpc event begin/end`, no `InitializeWebKit2 call` / `runInitializationCode call` (install still `orig=0x0`), no drawing-area `setGeometryFlipped`, no WebContent `remoteContextWithOptions` or dropped-port log. Safari created 24 `com.apple.macosbooter.WebKit.WebContent` connections with no `xpc_error`, `add_bundle` still `access=-1 errno=62` then `add_bundle_done` on the jb Frameworks path. Chrome still created CGS CAContexts (`contextWithCGSConnection`, WindowServer `setContextId` for those ids). The five `remoteContextWithOptions` lines were Safari, openAndSavePanel, QuickLookUIService, and the two screencapture processes — not WebContent.

`getenv_new` only defaults `GIGACAGE_ENABLED`; it does not rewrite `XPC_FLAGS`. On a real Mac, live WebContent keeps `XPC_FLAGS=8019` in its environment, so stock `xpc_main` is not what clears the value. The leading hypothesis is that iOS xpcproxy's authoritative launch context is a packed blob (inherited fd / port) that still carries `XPC_FLAGS=0x0` and no Application-type receive right; Ventura libxpc unpacks that blob after exec and overwrites the classic environ we set. Setting `8019` on the `execve` vector alone is therefore not enough to reconstitute the receive right.

### What was not done

No second variant. net16 proxies stay deployed (window publish still works). Rollback copies remain at `pre-net16-20261110-201123` (net15-era proxies, WebContent `181c64a6…`); libmachook rollback `pre-net15-20261110-195216` / net14 `c1b7b439…` and net10b `cffee750…` are still on the device. No SkyLight hooks, no extra Safari CAContext IMPs, no constructor `dispatch_after`, no remapper export, no competing Mach listener, no CAContext port-retry, no reboot, no 停止 macOS. IconServices jobs were left loaded.

### Paint / IPC conclusion

Safari still publishes a capturable window and accepts example.com / baidu.com, but the content area stays white because WebContent never accepts a peer XPC connection, so it never runs InitializeWebProcess / CreateWebPage, never constructs a drawing area, and never creates a CAContext. net16 confirmed that iOS xpcproxy already starts the setuid proxy with `XPC_FLAGS=0x0`, that rewriting the classic `execve` environment to the stock Application-type value `8019` does not make that value (or a receive right) appear in the child at `xpc_main`, and that stock `xpc_main` therefore still never accepts a connection. Next work should find and fix the packed launch context (or otherwise put a real xpcproxy receive right into the task) across chroot+exec, not retry CAContext ports and not reintroduce a competing Mach listener on the same name.

## 34. Child has no inherited receive right: registered ports are a send in slot 0 only (2026-11-10 20:18–20:22 CST / 04:18–04:22 PST)

### What was tried

net16 showed that putting `XPC_FLAGS=8019` on the proxy's classic `execve` environment did not make that value appear at `xpc_main` (still `0x0`, zero accepted connections). The leading hypothesis was that iOS xpcproxy's packed launch context (binprefs / registered ports / fileports) overwrote the flags and perhaps still carried a receive right. Darwin xpcproxy stashes the one-shot service port with `posix_spawnattr_set_registered_ports_np` (consumed in the child via `mach_ports_lookup`); libxpc also uses `__xpc_spawnattr_binprefs_pack/unpack`. The three-slot `mach_ports_register` layout is `NAME_SERVER_SLOT=0`, `ENVIRONMENT_SLOT=1`, `SERVICE_SLOT=2`.

net17 keeps the net16 proxies and changes only libmachook: immediately before stock `xpc_main`, inventory `mach_ports_lookup`, inherited fds 0–31, and the bootstrap special port, then `setenv("XPC_FLAGS","8019")` if the flags still look zero, and call stock `xpc_main` (with the existing main-queue connection wrapper). No public `macosbooter` check-in, no CAContext port-retry, no new exported symbols (still 168 T symbols).

### Deploy

Backup tag `pre-net17-20261110-202135` (20:21:35 CST / 04:21:35 PST) saved live net15 libmachook from both trees into `/var/mobile/macws-rollback/libmachook.dylib.pre-net17-20261110-202135` (CDHash `86f08ac4b4fa875ec61799ce7e991a0576b8b57b`, sha1 `2b0e9195c0f12b00f4d0b76c0c42a32e0692c8d0`). New thin arm64e `libmachook.dylib` is 1,479,408 B, fresh rootfs inode 23425648, CDHash `94fa9ceb9ac4f45f1ccb975690d72afe412f83d2` (sha1 `353be580e6f9f5c2b2c56bd749d05456f65e4008`); arm64 slice CDHash `64fe840477160515868ac1b0b20a5f6737928eb4` (sha1 `b6fdf3ff7d1e9fae7a3847a2431f0e6d08b03f2f`). WebKit proxies stayed on net16 (WebContentProxy `5d3a9cec32167ad654d0796efaf141cbab6345a9`, 4755). Smoke at 20:21:40 CST / 04:21:40 PST: `SMOKE_OK` rc=0. IconServices 53067/53071 stayed loaded; iOS agent 1724 was left alone. No reboot, no 停止 macOS.

### Safari run (one guarded attempt)

20:21:47 CST / 04:21:47 PST onward, Safari pid 58039. `LAUNCH_WINDOW_OK`, example.com and baidu.com accepted. Safari RSS grew to ~496 MB (507728 KB). `comp_occ` 20701 → 20638 (no watchdog). Cleanup CORE_OK (WindowServer 51054, macwshostd 416, SpringBoard 391), WebKit LaunchDaemons unloaded. Screenshots: `/workspace/net17-example.png` and `/workspace/net17-baidu.png` (2732×2048, clock 04:22 PST), Safari in front, URLs example.com / baidu.com, Start Page + E/B Untitled, content still white. Because the window stayed capturable, net17 was **not** rolled back.

### Field result

The net16 proxy still logged `incoming=0x0 outgoing=8019` (26 WebContent + Networking + SandboxBroker). In the stock child, `mach_ports_lookup` succeeded on every helper (`kr=0`, `count=3`): slot 0 held a **send** right (`typ=0x10000` = `MACH_PORT_TYPE_SEND`, names such as `0x1503` / `0x1403`), and slots 1 and 2 (`ENVIRONMENT_SLOT`, `SERVICE_SLOT`) were `MACH_PORT_NULL`. `recv_rights=0` on all 27 helpers. Inherited fds were only 0, 1, and 2 — no leftover packed-context fileport. Bootstrap special port was present (`0x807`). After the pre-`xpc_main` `setenv`, stock `xpc_main` logged `flags=8019` (the earlier `was=8019` on the reapply line is not a reliable pre-image: `setenv` can invalidate the old `getenv` pointer; the post-reapply `inherited path flags=8019` line is authoritative). **Zero** `xpc_main accepted connection=` lines, no `xpc event`, no `InitializeWebProcess` / `CreateWebPage`, no drawing-area, no WebContent `remoteContextWithOptions`. Safari created 26 `com.apple.macosbooter.WebKit.WebContent` connections with no `xpc_error`; `add_bundle` still `errno=62` then `add_bundle_done` on the jb Frameworks path.

So flags were not the remaining blocker once stock `xpc_main` actually saw `8019`. iOS xpcproxy never deposited an Application-type receive right into the task (no `SERVICE_SLOT` port, no extra fd). A send right in `NAME_SERVER_SLOT` plus a bootstrap special port is not enough for `xpc_main` to accept Safari's already-held peer.

### What was not done

No second variant. net17 libmachook and net16 proxies stay deployed (window publish still works). Rollback copies remain at `pre-net17-20261110-202135` (net15 dylib `86f08ac4…`) and `pre-net16-20261110-201123` (net15-era proxies, WebContent `181c64a6…`). No SkyLight hooks, no extra Safari CAContext IMPs, no constructor `dispatch_after`, no remapper export, no competing Mach listener, no CAContext port-retry, no reboot, no 停止 macOS. IconServices jobs were left loaded.

### Paint / IPC conclusion

Safari still publishes a capturable window and accepts example.com / baidu.com, but the content area stays white because WebContent never accepts a peer XPC connection, so it never runs InitializeWebProcess / CreateWebPage or creates a CAContext. net17 showed that even when stock `xpc_main` runs with `XPC_FLAGS=8019`, there is no inherited receive right to consume: `mach_ports_lookup` has only a send right in slot 0, empty service/environment slots, and no packed fd. Next work should get a real receive right into the helper task (fix the host-side xpcproxy / `xpc_add_bundle` handshake that should stash Safari's one-shot port, or an equivalent without checking in the public macosbooter name), not retry CAContext ports and not reintroduce a competing Mach listener.

## 35. Non-setuid WebKit stub still has no SERVICE_SLOT receive right (2026-11-10 20:37–20:38 CST / 04:37–04:38 PST)

### What was tried

net17 showed that stock `xpc_main` can run with `XPC_FLAGS=8019` and still never accept a peer, because `mach_ports_lookup` has only a send right in slot 0 and `recv_rights=0`. iOS xpcproxy never deposited an Application-type receive right into the setuid first image. libxpc already refuses setugid XPC services (`running setugid(), which is not allowed`), and iOS xpcproxy itself contains `setuid(%d)` / `Already registered ports` / `No spawn attr blob`. The leading hypothesis for net18 was that shipping the bundle executable as setuid is what caused xpcproxy to skip the one-shot handshake.

net18 keeps net17 libmachook and changes only the WebKit chroot proxies (shared `ViewBridgeChrootProxy/main.c` under `MACWS_WEBKIT_PROXY_EXPERIMENT`). The `CFBundleExecutable` is now mode 0755 so host xpcproxy can complete the Application-type handshake and stash the receive right in `SERVICE_SLOT`. A same-bytes sibling `*.priv` is installed mode 4755; if the stub is still unprivileged after that handshake, it `execve`s the sibling before chroot (Mach ports are expected to survive). The stub also inventories uid/euid and `mach_ports_lookup` to `/tmp/macws-webkit-proxy.log`. Incoming zero `XPC_FLAGS` is still reconstituted to `8019`. No public `macosbooter` check-in, no CAContext port-retry, no competing Mach listener, no libmachook change. `macos_gui.sh` cold-boot trust restore now puts 0755 back on the stub and 4755 on `*.priv`, instead of 4755 on the bundle executable.

### Deploy

Backup tag `pre-net18-20261110-203748` (20:37:48 CST / 04:37:48 PST) saved the live net16 setuid proxies from both trees into `/var/mobile/macws-rollback/proxies.pre-net18-20261110-203748/` (WebContent CDHash `5d3a9cec32167ad654d0796efaf141cbab6345a9`, sha1 `f5d11cd39fd652532a43d42378f9d4f3e5850598`). New thin arm64e proxies (same image for stub and sibling):

- WebContentProxy 84,336 B, jb inode 44300 / rootfs 23451740, CDHash `f10b06cb9c2e329cf9d1600d7dd7a43d5daff5e4` (sha1 `65dca51e7dc42d1228f8a651392968dabd92e6d5`)
- WebNetworkingProxy 84,352 B, CDHash `0a0d7af7bb4ec9cf7c55bc0d0a61b55595140060` (sha1 `1b0851823e9df2fa541a03a2a2555315d759e4e5`)
- WebGPUProxy 84,320 B, CDHash `dd005061bba0aba11009599d8500d2ce3654516e` (sha1 `e6652e5e3ae74b88445dc704739732cfa863e246`)
- SafariSandboxProxy 84,352 B, CDHash `d38fde6be54f36b0db8999c6fd72d839f16aec1f` (sha1 `2f4b5f1da5da8c6d13417062ebe97c234ba4b154`)

Stub mode 0755, `*.priv` mode 4755, both trees. libmachook stayed on net17 (inode 23425648, CDHash `94fa9ceb9ac4f45f1ccb975690d72afe412f83d2`). Smoke at 20:37:58 CST / 04:37:58 PST: `SMOKE_OK` rc=0. IconServices 53067/53071 stayed loaded; iOS agent 1724 was left alone. No reboot, no 停止 macOS.

### Safari run (one guarded attempt)

20:38:05 CST / 04:38:05 PST onward, Safari pid 59506. `LAUNCH_WINDOW_OK`, example.com and baidu.com accepted. Safari RSS grew to ~522 MB (533760–534112 KB), the healthy visible-window class. After example: 26 WebContent helpers plus Networking stayed `Ss` at ~27 MB; after baidu they remained up. GPU was not requested. `comp_occ` 20596 → 20566 (no watchdog). Cleanup CORE_OK (WindowServer 51054, macwshostd 416, SpringBoard 391), WebKit LaunchDaemons unloaded, no leftover macOS Safari/WebContent. Screenshots: `/workspace/net18-example.png` and `/workspace/net18-baidu.png` (2732×2048, clock 04:38 PST), Safari in front, URLs example.com / baidu.com, Start Page + E/B Untitled, content still white. Because the window stayed capturable, net18 was **not** rolled back.

### Field result

In the stock child, `mach_ports_lookup` again succeeded on every helper (`kr=0`, `count=3`, 29 helpers): slot 0 held only a **send** right (`typ=0x10000` = `MACH_PORT_TYPE_SEND`, names such as `0x1403`), and slots 1 and 2 (`ENVIRONMENT_SLOT`, `SERVICE_SLOT`) were `MACH_PORT_NULL`. `recv_rights=0` on all 29 `ports_lookup` lines (58 `recv_rights=0` tokens including the later reapply lines). After the pre-`xpc_main` `setenv`, stock `xpc_main` logged `flags=8019`. **Zero** `xpc_main accepted connection=` lines, no `xpc event`, no `InitializeWebProcess` / `CreateWebPage`, no drawing-area, no WebContent `remoteContextWithOptions`. The five `remoteContextWithOptions` lines were Safari, openAndSavePanel, QuickLookUIService, and the two screencapture processes — not WebContent. `InitializeWebKit2` hook install still `orig=0x0` and was never called. Safari created 28 `com.apple.macosbooter.WebKit.WebContent` connections plus Networking and SandboxBroker, with no `xpc_error`; `add_bundle` still `errno=62` then `add_bundle_done` on the jb Frameworks path.

The first-image proxy log (`/tmp/macws-webkit-proxy.log`) was empty after the run (truncated at start, never rewritten). The child's libmachook inventory is therefore the authoritative handshake result: making the bundle executable non-setuid did not cause iOS xpcproxy to deposit a `SERVICE_SLOT` receive right. Helpers still started, chrooted, and exec'd the stock image as root, so the 0755 stub was sufficient for the privilege/chroot path in this Safari session (the `*.priv` sibling was not required to keep helpers up). Flags were already known not to be the remaining blocker.

So the setuid bit on the first image is not what strips the one-shot port. The remaining host-side gap is the `xpc_add_bundle` / xpcproxy spawn itself: the path Safari registers (`/var/jb/usr/macOS/Frameworks/...`) still fails `access()` inside the chroot with ELOOP (rootfs `/var/jb/usr` is an absolute symlink to `/var/jb/usr`), and iOS xpcproxy still starts the job without an Application-type receive right (`No spawn attr blob` is a plausible match). A `/var/mnt/rootfs/...` add_bundle path is readable in both namespaces but was previously shown to yield Connection interrupted and no helper, and was not retried this turn.

### What was not done

No second variant. net18 proxies stay deployed (window publish still works) with net17 libmachook. Rollback copies remain at `pre-net18-20261110-203748` (net16 proxies, WebContent `5d3a9cec…`, mode 4755, no `*.priv`), `pre-net17-20261110-202135` (net15 dylib `86f08ac4…`), and `pre-net16-20261110-201123`. No SkyLight hooks, no extra Safari CAContext IMPs, no constructor `dispatch_after`, no remapper export, no competing Mach listener, no CAContext port-retry, no reboot, no 停止 macOS. IconServices jobs were left loaded.

### Paint / IPC conclusion

Safari still publishes a capturable window and accepts example.com / baidu.com, but the content area stays white because WebContent never accepts a peer XPC connection, so it never runs InitializeWebProcess / CreateWebPage or creates a CAContext. net18 showed that shipping the WebKit proxy as a non-setuid `CFBundleExecutable` (with a 4755 `*.priv` sibling only for unprivileged chroot) is not enough: after `chroot+exec`, `mach_ports_lookup` still has only a send right in slot 0, empty service/environment slots, and `recv_rights=0`. Next work should fix the host-side `xpc_add_bundle` / xpcproxy handshake so the spawn actually includes Safari's one-shot receive right (for example by making the registered bundle readable inside the chroot without changing the host path that keeps helpers up), or an equivalent that does not check in the public macosbooter name, not retry CAContext ports and not reintroduce a competing Mach listener.

## 36. In-chroot add_bundle path is readable (errno=0) but helpers still have no SERVICE_SLOT receive right (2026-11-10 20:50–20:52 CST / 04:50–04:52 PT)

### What was tried

net18 showed that a non-setuid `CFBundleExecutable` plus a 4755 `*.priv` sibling did not deposit a `SERVICE_SLOT` receive right (`flags=8019`, `mach_ports_lookup` count=3, slot 0 send-only, slots 1 and 2 null, `recv_rights=0`, zero accepted connections). `xpc_add_bundle` still failed inside the chroot with `errno=62` (ELOOP) on `/var/jb/usr/macOS/Frameworks/...`, because rootfs `/var/jb/usr` was an absolute symlink to `/var/jb/usr` — valid in the host namespace (Dopamine `/var/jb` → procursus) but a loop inside the chroot. The leading hypothesis for net19 was that libxpc in chrooted Safari could not read the bundle `Info.plist`, so host xpcproxy / launchd spawned the helper without an Application-type spawnattr blob (`No spawn attr blob` is in iOS xpcproxy strings).

net19 keeps the net18 proxies and net17 libmachook and changes only `ensure_jb_usr_bind.sh`. The registered add_bundle path string stays `/var/jb/usr/macOS/Frameworks/...` (the host-visible path that keeps helpers up; the earlier chroot-only `/usr/local/libexec` path is not retried). Success is now defined as an in-chroot read of a WebKit canary plist, not a host-side `-x` of the looping symlink. `mount_bindfs` / bindfs / lofs / `mount -o bind` are still unavailable on this kernel, so the fallback moves the host-only rootfs symlink aside to `/var/mnt/rootfs/var/jb/usr.pre-macws-hostlink` and places a slim tree whose `macOS/Frameworks/*.framework` entries are dual-namespace symlinks to `/var/mnt/rootfs/usr/local/libexec/macws-xpc/$name` (currently only `WebKit.framework`). The script does not mkdir `usr/lib` (CydiaSubstrate must not escape). The in-chroot canary is probed with `run_bash.sh` / `launchdchrootexec` — the same admission path Safari uses — because a raw `chroot /var/mnt/rootfs /bin/test` of the macOS image is AMFI SIGKILL'd from iOS. Host processes that resolve `/var/jb/usr` still see procursus; only the rootfs/chroot view of that path is repaired. No public `macosbooter` check-in, no CAContext port-retry, no competing Mach listener, no proxy or libmachook change.

### Deploy

Backup tag `pre-net19-20261110-205054` (20:50:54 CST / 04:50:54 PT) saved the previous `ensure_jb_usr_bind.sh` and the live rootfs `/var/jb/usr` symlink into `/var/mobile/macws-rollback/jb-usr.pre-net19-20261110-205054/`. The new script was installed to `/var/jb/usr/macOS/bin/ensure_jb_usr_bind.sh` and run once: it replaced the looping rootfs symlink with the slim macws-xpc tree. A first canary that used raw `chroot /bin/test` was SIGKILL'd and the script exited 1 after the layout was already applied; a `run_bash.sh` probe immediately afterwards read the canary (`RUN_BASH_CANARY_OK`, Info.plist 821 B). The probe in the installed script was then switched to `run_bash.sh` and `ensure_jb_usr_bind.sh` exited 0 without changing the already-correct layout. WebKit proxies stayed on net18 (WebContentProxy stub 0755 / `*.priv` 4755, CDHash `f10b06cb9c2e329cf9d1600d7dd7a43d5daff5e4`). libmachook stayed on net17 (inode 23425648, CDHash `94fa9ceb9ac4f45f1ccb975690d72afe412f83d2`). Smoke at 20:51:36 CST / 04:51:36 PT: chroot `SMOKE_OK` rc=0; the smoke script's extra raw-chroot canary was also SIGKILL'd and is not a real readability failure (`run_bash` canary stayed OK). IconServices 1724 / 53067 / 53071 stayed loaded. No reboot, no 停止 macOS. WindowServer 51054 remained `Ss`.

### Safari run (one guarded attempt)

20:51:54 CST / 04:51:54 PT onward, Safari pid 60764. `LAUNCH_WINDOW_OK`, example.com and baidu.com accepted. Safari RSS grew to ~551 MB (563760–564224 KB), the healthy visible-window class. After example: 28 WebContent helpers plus Networking stayed up; after baidu, 36 WebContent plus Networking. GPU was not requested. `comp_occ` 20557 → 20420 (no watchdog). Cleanup CORE_OK (WindowServer 51054, macwshostd 416, SpringBoard 391), WebKit LaunchDaemons unloaded, no leftover macOS Safari/WebContent. Screenshots: `/workspace/net19-example.png` and `/workspace/net19-baidu.png` (2732×2048, clock 04:52 PT), Safari in front, URLs example.com / baidu.com, Start Page + E/B Untitled, content still white. Because the window stayed capturable, net19 was **not** rolled back.

### Field result

The in-chroot ELOOP is gone for WebKit. `xpc_add_bundle` on the unchanged host path `/var/jb/usr/macOS/Frameworks/WebKit.framework/Versions/A/XPCServices/{WebContent,WebNetworking,SafariSandbox}Proxy.xpc` now logs `access=0 errno=0` (three bundles, no `errno=62`). Safari still creates the remapped connections (`TSV-hit` / `created activate` for 30 WebContent + Networking + SandboxBroker during the example window, no `xpc_error`). Helpers still start, chroot, and exec the stock image.

In the stock child, `mach_ports_lookup` again succeeded on every helper (`kr=0`, `count=3`, 31 helpers: 30 WebContent + 1 Networking): slot 0 held only a **send** right (`typ=0x10000` = `MACH_PORT_TYPE_SEND`, names such as `0x1303` / `0x1503`), and slots 1 and 2 (`ENVIRONMENT_SLOT`, `SERVICE_SLOT`) were `MACH_PORT_NULL`. `recv_rights=0` on all 62 tokens (inventory + reapply lines). Inherited fds were only 0, 1, and 2. After the pre-`xpc_main` `setenv`, stock `xpc_main` logged `flags=8019`. **Zero** `xpc_main accepted connection=` lines, no `xpc event`, no `InitializeWebProcess` / `CreateWebPage`, no drawing-area, no WebContent `remoteContextWithOptions`. The five `remoteContextWithOptions` lines were Safari, openAndSavePanel, QuickLookUIService, and the two screencapture processes — not WebContent. WebContent processes did install CAContext / paint hooks (`wc=1`, 30 helpers) but never created a context. `InitializeWebKit2` hook install still `orig=0x0` and was never called. The first-image proxy log (`/tmp/macws-webkit-proxy.log`) was empty again; the child's libmachook inventory is the authoritative handshake result.

So making the registered bundle readable inside the chroot was necessary for `xpc_add_bundle` itself, but it is not what causes iOS xpcproxy to stash an Application-type receive right. Launchd still resolves the same path in the host `/var/jb` (procursus) tree, which is why helpers stay up; Safari can now also `access()` that path inside the chroot, and add_bundle no longer fails. The receive-right gap is therefore somewhere else in the host xpcproxy / spawnattr handshake, not the ELOOP on the plist. Other jb Frameworks (DockHelper, etc.) still ELOOP inside the chroot until they have a macws-xpc copy or bindfs; only WebKit is in macws-xpc.

### What was not done

No second variant. The net19 `ensure_jb_usr_bind.sh` layout stays deployed (window publish still works) with net18 proxies and net17 libmachook. Rollback copies remain at `pre-net19-20261110-205054` (old ensure script + looping rootfs `/var/jb/usr` symlink), `pre-net18-20261110-203748` (net16 proxies, WebContent `5d3a9cec…`, mode 4755, no `*.priv`), and `pre-net17-20261110-202135` (net15 dylib `86f08ac4…`). No SkyLight hooks, no extra Safari CAContext IMPs, no constructor `dispatch_after`, no remapper export, no competing Mach listener, no CAContext port-retry, no reboot, no 停止 macOS. IconServices jobs were left loaded. The add_bundle path string was not changed to `/var/mnt/rootfs` or `/usr/local/libexec`.

### Paint / IPC conclusion

Safari still publishes a capturable window and accepts example.com / baidu.com, but the content area stays white because WebContent never accepts a peer XPC connection, so it never runs InitializeWebProcess / CreateWebPage or creates a CAContext. net19 fixed the in-chroot ELOOP on the host-visible add_bundle path (`access=0 errno=0`), but after `chroot+exec` `mach_ports_lookup` still has only a send right in slot 0, empty service/environment slots, and `recv_rights=0`. Readable Info.plist / successful `xpc_add_bundle` is not sufficient to place the one-shot receive right. Next work should keep this layout and attack the remaining host xpcproxy / spawnattr handshake (or an equivalent that does not check in the public macosbooter name), not retry CAContext ports and not reintroduce a competing Mach listener.

## 37. Unique-name WebKit peer spawn did not run: TSV-hit still takes the add_bundle path (2026-11-10 21:12–21:13 CST / 05:12–05:13 PT)

### What was tried

net19 made the registered add_bundle path readable inside the chroot (`access=0 errno=0`) but helpers still came up with `flags=8019`, `mach_ports_lookup` count=3, slot 0 send-only, `ENVIRONMENT_SLOT` / `SERVICE_SLOT` null, `recv_rights=0`, inherited fds only 0/1/2, zero accepted connections, no `InitializeWebProcess` / `CreateWebPage`, and no WebContent CAContext. Readable Info.plist is not enough; iOS xpcproxy still starts the helper without an Application-type receive right (`No spawn attr blob` remains a plausible match). `mach_ports_register` cannot move a RECEIVE right (MIG is COPY_SEND; a probe with child-space names returned `kr=17` `KERN_INVALID_RIGHT`), so net20 does not try to stash a port that way and does not check in the public `macosbooter.WebKit.*` name.

The single variant keeps the net19 chroot path layout and the 0755 stub + 4755 `*.priv` proxy split. Safari-side `xpc_connection_create_application_service` is supposed to `posix_spawn` the host-visible / in-chroot-readable WebKit proxy with a unique `MACWS_WK_PEER=com.macwsguide.wk.i.<32 hex>` name and `XPC_FLAGS=8019`, wait up to 2 s on `bootstrap_look_up`, and return `xpc_connection_create_mach_service_raw` on that unique name only. The helper, if `MACWS_WK_PEER` is valid, skips stock `xpc_main`, listens only on that name, and enters `NSApp` `run` or `dispatch_main`. Seatbelt already allows the `com.macwsguide.wk.i.` prefix. If spawn or lookup fails, the hook falls back to the previous add_bundle + macosbooter `create_fn` path so a miss should not be worse than net19.

A chrooted `posix_spawn` of the proxy previously exited 118 because `F_GETPATH` of `/var/mnt/rootfs` is `/` and `MacWSBuildChrootRootEnvironment` still correctly rejects `/` (consumers use it as a prefix). The proxy now adopts an inherited `MACWS_CHROOT_HOST_ROOT` that is an absolute path with a second character (`/private/var/mnt/rootfs` or `/var/mnt/rootfs`) instead of immediately exiting 118. The header still rejects `/`. New contracts: `misc/test_webkit_peer_spawn.py` plus the inherited-root cases in `misc/test_chroot_proxy_environment.c`. 10 tests OK on the Intel Mac before deploy. No new exported symbols (all helpers static). No public macosbooter check-in, no competing Mach listener on that name, no CAContext port-retry.

On-device THEOS / `MacWSBootingGuide` were still absent, so libmachook and the four proxies were cross-compiled on the Intel Mac (thin arm64e + arm64 libmachook; iPhoneOS arm64e proxies). After net19, in-chroot `/var/jb/usr/macOS/Frameworks/WebKit.framework` is the macws-xpc copy, so proxies were installed in both the procursus Frameworks tree and macws-xpc.

### Deploy

Backup tag `pre-net20-20261110-211200` (21:12:00 CST / 05:12:00 PT) saved the live net17 libmachook and net18 proxies (both trees) into `/var/mobile/macws-rollback/{libmachook,proxies}.pre-net20-20261110-211200/`. The net19 path layout was not touched. Two-pass `ldid -S -M` then `jbctl trustcache add`; the rootfs libmachook copy was a byte copy, not re-signed. Live CDHashes: libmachook arm64e `1cd2631fada7480a934a81dd8637d11e8543fd45`, arm64 `ec48565cf5b2a84f12a41099e70e3a162596d02c`; WebContentProxy `2097451dd27a6c353c69c88c6ccb5c62acb14e5b`, WebNetworkingProxy `27513eacf4cdc2f40defc0d771ed432bc8bff8ac`, WebGPUProxy `f455dda3fdc0d34e7c411bc64f1afffdec687b33`, SafariSandboxProxy `3dbb88c7f65e8686fa4cb012643ae0aed9b46641`. Stub 0755 / `*.priv` 4755, 85392 B after ldid, both trees. Smoke at 21:12:08 CST / 05:12:08 PT: `SMOKE_OK` rc=0, `RUN_BASH_CANARY_OK` rc=0, IconServices 1724 / 53067 / 53071 still loaded, WindowServer 51054 `Ss`, macwshostd 416, SpringBoard 391. `SMOKE_PASS`. No reboot, no 停止 macOS.

### Safari run (one guarded attempt)

21:12:15 CST / 05:12:15 PT onward, Safari pid 62437. The window stayed visible. Safari RSS grew to the healthy visible-window class (~593392–593648 KB). After example.com: 31 WebContent helpers plus Networking stayed up; after baidu.com, 42 WebContent plus Networking. GPU was not requested. `CAPTURE_OK` for both pages. Cleanup `CORE_OK` (WindowServer 51054, macwshostd 416, SpringBoard 391), WebKit LaunchDaemons unloaded, no leftover macOS Safari/WebContent (iOS MobileSafari pid 625 / WebContent 703 ignored). Screenshots: `/workspace/net20-example.png` and `/workspace/net20-baidu.png` (2732×2048, clock 05:12 PT / 05:13 PT), Safari in front, URLs example.com / baidu.com, Start Page + E/B Untitled tabs, content still white. Because the window stayed capturable, net20 was **not** rolled back.

### Field result

The unique-name handshake never ran. `/tmp/macws-xpc-app.log` has zero `wk-peer spawn` / `MACWS_WK_PEER` / `com.macwsguide.wk.i.` lines. Every WebKit and SandboxBroker request hit `macws_xpc_app_services_lookup` first; the TSV-hit branch registers the proxy bundle, calls `create_fn` on `com.apple.macosbooter.WebKit.*` / `Safari.SandboxBroker`, and returns immediately. The new `posix_spawn` block lives only on the hardcoded path after a TSV miss, so it was unreachable for the names that matter. `add_bundle` still logged `access=0 errno=0` for WebContent / Networking / SandboxBroker (net19 layout held). Safari still created remapped connections (`TSV-hit` / `created activate`, 32 WebContent + Networking + 3 SandboxBroker in the example window, no `xpc_error`).

Helpers were still started by host xpcproxy, not by the unique-name spawn. All stock children logged `peer=(null)` and took the inherited `xpc_main` path (`service=com.apple.macosbooter.WebKit.WebContent`, `flags=8019`, "no Mach-listener check-in"). `mach_ports_lookup` again succeeded on every helper (`kr=0`, `count=3`): slot 0 send-only (`typ=0x10000`), slots 1 and 2 null, `recv_rights=0`, inherited fds only 0/1/2. **Zero** `xpc_main accepted connection=` lines, no `xpc event`, no `InitializeWebProcess` / `CreateWebPage`, no drawing-area, no WebContent `remoteContextWithOptions`. WebContent processes did install CAContext / paint hooks (`wc=1`, 32 helpers in the paint log; 34 unique WebContent pids in the init log) but never created a context. The `remoteContextWithOptions` lines were Safari, openAndSavePanel, QuickLookUIService, and screencapture — not WebContent. The inherited-root 118 fallback in the proxy was not exercised by the unique-name path.

Unlike net18/net19, the first-image proxy log was populated this run (`/var/mnt/rootfs/tmp/macws-webkit-proxy.log`, 28427 B, 34 handshake records: 32 WebContent + 1 Networking + 1 SandboxBroker). Every record is `incoming=0x0 outgoing=8019`, `already_priv=0 will_exec_priv=0`, `ports_lookup` count=3, slot 0 send-only, slots 1 and 2 null, `recv_rights=0`. That log now agrees with the child's libmachook inventory; it does not change the handshake result.

### What was not done

No second variant (the TSV-hit early return is a field finding, not a same-turn fix). net20 libmachook + proxies stay deployed because the window remained capturable; the net19 path layout is unchanged. Rollback copies remain at `pre-net20-20261110-211200` (this turn's libmachook + proxies only), `pre-net19-20261110-205054` (path layout), `pre-net18-20261110-203748` (proxies, WebContent `f10b06cb…`), and `pre-net17-20261110-202135` (libmachook `94fa9ceb…`). No SkyLight hooks, no extra Safari CAContext IMPs, no constructor `dispatch_after`, no new remapper export, no competing Mach listener on the public macosbooter name, no CAContext port-retry, no reboot, no 停止 macOS. IconServices jobs were left loaded.

### Paint / IPC conclusion

Safari still publishes a capturable window and accepts example.com / baidu.com, but the content area stays white because WebContent never accepts a peer XPC connection, so it never runs InitializeWebProcess / CreateWebPage or creates a CAContext. net20 built a unique-name `posix_spawn` handshake and an inherited-root proxy fallback, but `macws_xpc_create_application_service` still returns on a TSV-hit for every WebKit / SandboxBroker name and never reaches that spawn, so helpers still come from host xpcproxy with `peer=(null)`, slot 0 send-only, empty service/environment slots, and `recv_rights=0`. Successful `xpc_add_bundle` plus a compiled-in unique-name path that is skipped is not enough. Next work should keep this layout and move the unique-name spawn ahead of (or into) the TSV-hit return for WebKit / SandboxBroker only, still falling back to add_bundle + macosbooter `create_fn` on spawn/lookup failure, still not checking in the public macosbooter name, and still not adding a competing Mach listener or retrying CAContext ports.

## 38. Unique-name spawn now runs but bootstrap_look_up times out and Safari misses its window (2026-11-10 21:22–21:24 CST / 05:22–05:24 PT)

### What was tried

net20 compiled in `posix_spawn` of the WebKit proxy with `MACWS_WK_PEER=com.macwsguide.wk.i.<32 hex>`, but `macws_xpc_create_application_service` returned on a TSV-hit for every WebKit / SandboxBroker name and never reached that spawn. Zero `wk-peer spawn` lines; helpers still had `flags=8019`, slot 0 send-only, `recv_rights=0`. The Safari window stayed visible.

net21 moves the unique-name spawn *ahead of* the TSV-hit `add_bundle` / `create_fn` return, but only for the public WebKit / Safari SandboxBroker names (`com.apple.WebKit.WebContent`, `.Networking`, `.GPU`, `com.apple.Safari.SandboxBroker`). macosbooter remaps are not treated as spawnable, so a fallback `create_fn` re-entering this hook cannot start a second unique-name helper. On spawn or `bootstrap_look_up` failure the hook still falls back to add_bundle + macosbooter `create_fn`. Other TSV rows are unchanged. No new exported symbols, no public macosbooter check-in, no competing Mach listener on that name, no CAContext port-retry. Proxies and the net19 path layout were left on net20 / net19. 11 contract tests OK on the Intel Mac (6 peer-spawn + 5 environment).

### Deploy

Backup tag `pre-net21-20261110-212258` (21:22:58 CST / 05:22:58 PT) saved the live net20 libmachook (`1cd2631fada7480a934a81dd8637d11e8543fd45`) and net20 proxies (WebContent `2097451dd27a6c353c69c88c6ccb5c62acb14e5b`, both trees). The net19 path layout was not touched. Only libmachook was replaced (cross-built on the Intel Mac, two-pass `ldid -S -M`, `jbctl trustcache add`, rootfs byte-copy not re-signed). Live net21 CDHashes (while deployed): arm64e `938a1acb42ac3b627c47d9ed151b1160d032dbf1`, arm64 `1654c71e11d1f52b72f90431f0857a2f6a98dbe0`. Proxies stayed net20. Smoke at 21:23:04 CST / 05:23:04 PT: `SMOKE_OK` / `RUN_BASH_CANARY_OK` rc=0, IconServices 1724 / 53067 / 53071 loaded, WindowServer 51054 `Ss`, macwshostd 416, SpringBoard 391. `SMOKE_PASS`. No reboot, no 停止 macOS.

### Safari run (one guarded attempt)

21:23:08 CST / 05:23:08 PT onward, Safari pid 63624. The launcher reported `ok=no … 已启动，但 30 秒内没有发布可捕获的 AppKit 窗口`. After that 30 s Safari was still running (RSS 162992 KB, below the healthy visible-window class of ~593 MB) with 10 stock WebContent helpers plus Networking, but no capturable AppKit window. The script did not open example.com or baidu.com (`LAUNCH_WINDOW_FAIL; not opening URLs`) and wrote `DONE_NO_WINDOW`. Cleanup `CORE_OK`, WebKit LaunchDaemons unloaded, IconServices left loaded. No `/workspace/net21-example.png` or `/workspace/net21-baidu.png` — there was no window to capture.

Because Safari lost a visible/capturable window, net21 was **rolled back immediately** to `pre-net21-20261110-212258`. No second variant. Post-rollback smoke at 21:24:42 CST / 05:24:42 PT: `SMOKE_PASS`, libmachook arm64e again `1cd2631fada7480a934a81dd8637d11e8543fd45`, WebContentProxy still `2097451dd27a6c353c69c88c6ccb5c62acb14e5b`, IconServices 1724 / 53067 / 53071, WindowServer 51054 `Ss`.

### Field result

The unique-name spawn **did run**. `/tmp/macws-xpc-app.log` has 12 `wk-peer spawn rc=0 errno=0` lines (10 WebContent + 1 Networking + 1 SandboxBroker) with `path=/var/jb/usr/macOS/Frameworks/WebKit.framework/...` and distinct `com.macwsguide.wk.i.<32 hex>` names. No `path-fail` / `name-invalid`. Every completed attempt then logged `wk-peer lookup-timeout` (11) and `wk-peer fallback … tsv=hit` (11). Zero `wk-peer connected`. The 12th spawn was still in its 2 s wait when the window watchdog cleaned up.

The spawned children did exec the stock image with `MACWS_WK_PEER` set. 11 helpers logged `peer=com.macwsguide.wk.i.…` (not `(null)`) and `xpc_main WebKit unique peer listener=0x…` on that unique name — a non-null listener object, not a public macosbooter check-in. Those same lines also show `service=com.macwsguide.hostd` (Safari's inherited `XPC_SERVICE_NAME`, copied into the posix_spawn envp). There were **no** follow-up `entering NSApp run`, `entering dispatch_main`, or `listener failed` lines after `xpc_connection_resume`. Inventory taken *before* the listener still had `mach_ports_lookup` count=3, slot 0 send-only, slots 1 and 2 null, `recv_rights=0`. Those unique-spawned proxies also inherited extra fds (`0 1 2 3 4 7`) rather than only 0/1/2. Safari's `bootstrap_look_up` of the unique name never succeeded within 2 s, so the hook fell back.

Fallback add_bundle stayed `access=0 errno=0` (net19 layout held) and created macosbooter connections with no `xpc_error` (10 WebContent + Networking + SandboxBroker). Those fallback helpers logged `peer=(null)` and took stock `xpc_main` with `flags=8019`, still `recv_rights=0`. Zero accepted connections, no `InitializeWebProcess` / `CreateWebPage`, no WebContent `remoteContextWithOptions` (only Safari chrome CAContexts). 11 synchronous 2 s `bootstrap_look_up` waits on Safari's application-service thread are the likely reason the AppKit window was not published within 30 s.

### What was not done

No second variant. net21 libmachook was rolled back; the device is again on net20 libmachook + net20 proxies + the net19 path layout. Rollback copies remain at `pre-net21-20261110-212258` (this turn's `938a1acb…` dylib), `pre-net20-20261110-211200` (net20 libmachook `1cd2631f…` and proxies `2097451d…`), `pre-net19-20261110-205054` (path layout), `pre-net18-20261110-203748`, and `pre-net17-20261110-202135`. No SkyLight hooks, no extra Safari CAContext IMPs, no constructor `dispatch_after`, no new remapper export, no competing Mach listener on the public macosbooter name, no CAContext port-retry, no reboot, no 停止 macOS. IconServices jobs were left loaded.

### Paint / IPC conclusion

The unique-name spawn now actually runs (`wk-peer spawn rc=0`) and the helper creates a unique-name listener object with `MACWS_WK_PEER` set, but Safari's `bootstrap_look_up` of `com.macwsguide.wk.i.<hex>` times out, there are zero connected peers, and the 2 s-per-request wait prevented Safari from publishing a capturable window within 30 s. Fallback add_bundle still starts helpers with no receive right, and there is no InitializeWebProcess / CreateWebPage / WebContent CAContext / page paint. net21 was rolled back. Next work should keep the net19 layout and the TSV-hit reordering (the spawn path is now reachable) but must not block Safari's launch thread for 2 s per helper, and should find why a non-null unique-name listener is still invisible to Safari's `bootstrap_look_up` — without checking in the public macosbooter name, adding a competing Mach listener on that name, or retrying CAContext ports.

## 39. Fail-fast xpc_pipe endpoint rendezvous keeps Safari's window, but the unique peer still does not connect (2026-11-10 21:45–21:47 CST / 05:45–05:47 PT)

### Change

net21 showed that a unique-name `xpc_connection_create_mach_service` LISTENER is created in the helper but is not look-up-able from Safari (`bootstrap_look_up` kr=1102, likely `BOOTSTRAP_UNKNOWN_SERVICE`), and that a 2 s wait per helper on Safari's application-service thread costs the 30 s capturable-window deadline. Same-process chroot probes this turn confirmed the publication failure: an XPC LISTENER never appears in the bootstrap Safari queries; native `bootstrap_register` of the unique name is look-up-able (kr=0) but `create_mach_service` LISTENER after register immediately yields "Connection invalid"; `bootstrap_check_in` after register is kr=1103. Extending macwshostd's Steam Mach-rendezvous broker would require a hostd restart and was not done.

The proven alternative, also confirmed in-chroot (`EPPROBE` `got_peer=1 got_msg=1 client_err=0`, `CHROOT_RC=0`), is an anonymous `xpc_connection_create(NULL)` listener plus `xpc_endpoint_create`, delivered over `xpc_pipe_create_from_port` / `xpc_pipe_simpleroutine` / `xpc_pipe_receive` on a parent-owned rendezvous receive right, then `xpc_connection_create_from_endpoint`. That path does not use bootstrap names and does not check in `macosbooter.WebKit.*`.

net22 keeps the net21 TSV-hit reordering (unique spawn still runs *before* the add_bundle / `create_fn` return, for public WebKit / SandboxBroker names only). It replaces the 100×20 ms `bootstrap_look_up` loop with that pipe/endpoint handshake: Safari allocates a rendezvous receive right, `posix_spawnattr_set_registered_ports_np` COPY_SENDs it in slot 1 (`MACWS_WK_RV_SLOT`), and waits at most `MACWS_WK_RV_WAIT_MS` 150 ms. On timeout the receive right is destroyed, the child is SIGKILL'd, and the hook falls back to add_bundle + macosbooter `create_fn`. The helper unique path no longer creates a named Mach LISTENER; it builds the anonymous listener and sends the endpoint. Inherited `XPC_SERVICE_NAME=com.macwsguide.hostd` is replaced with the unique peer name in the spawn envp. No new exported symbols (still 168 T per slice), no public macosbooter check-in, no competing Mach listener on that name, no CAContext port-retry, no SkyLight/CGS hooks, no Safari CAContext IMPs, no constructor `dispatch_after`. Proxies and the net19 path layout were left on net20 / net19. 12 contract tests OK on the Intel Mac (7 peer-spawn + 5 environment).

### Deploy

Backup tag `pre-net22-20261110-214530` (21:45:30 CST / 05:45:30 PT) saved the live net20 libmachook (`1cd2631fada7480a934a81dd8637d11e8543fd45`) and net20 proxies (WebContent `2097451dd27a6c353c69c88c6ccb5c62acb14e5b`, both trees). The net19 path layout was not touched. Only libmachook was replaced (cross-built on the Intel Mac, `misc/set_macos_version.py` LC_BUILD_VERSION macOS 13.0, host two-pass `ldid -S` + `codesign`, device two-pass `ldid -S$ENT -M`, `jbctl trustcache add`, rootfs byte-copy not re-signed). Live net22 CDHashes: arm64e `98f4443646ba1e480075044de20deba064248675`, arm64 `cc5fc4a2f7575e86c844bfcb01704e4c311b0bee`. Proxies stayed net20. Smoke at 21:45:37 CST / 05:45:37 PT: `SMOKE_OK` / `RUN_BASH_CANARY_OK` rc=0, IconServices 1724 / 53067 / 53071 loaded, WindowServer 51054 `Ss`, macwshostd 416, SpringBoard 391. `SMOKE_PASS`. No reboot, no 停止 macOS.

### Safari run (one guarded attempt)

21:45:42 CST / 05:45:42 PT onward, Safari pid 64924. The launcher reported `LAUNCH_WINDOW_OK` (`ok=yes`). After launch Safari's RSS was in the healthy visible-window class (~599–602 MB; 616720 KB after example.com, 617024 KB after baidu). example.com and baidu.com were opened and captured: `/workspace/net22-example.png` and `/workspace/net22-baidu.png` (clocks 05:46 PT). Chrome (tabs, URL bar, toolbar) painted; page content stayed white, same as net20. Cleanup `CORE_OK`, WebKit LaunchDaemons unloaded, IconServices left loaded. Because the window stayed capturable, net22 was **not** rolled back. No second variant.

### Field result

The unique-name spawn **did run**. `/tmp/macws-xpc-app.log` has 39 `wk-peer spawn rc=0 errno=0` lines (WebContent + Networking + SandboxBroker) with `path=/var/jb/usr/macOS/Frameworks/WebKit.framework/...` and distinct `com.macwsguide.wk.i.<32 hex>` names. No `path-fail` / `name-invalid` / `lookup-timeout`. Every spawn then logged `wk-peer endpoint-timeout … wait_ms=150 timed_out=1` (39) and `wk-peer fallback … tsv=hit` (39). Zero `wk-peer connected`. The 150 ms cap did what it was for: Safari published a capturable window within 30 s, unlike net21.

The handshake itself did not complete. Unique-spawned children never logged `unique peer anonymous listener` or `endpoint-send`. All 36 `xpc_main WebKit inherited path` lines are the *fallback* helpers: `peer=(null)`, `service=com.apple.macosbooter.WebKit.*` (or SandboxBroker), `flags=8019`. Fallback add_bundle stayed `access=0 errno=0` (net19 layout held) and created macosbooter connections with no `xpc_error`.

Two remaining gaps are now visible in the same logs:

1. Registered-port inheritance. All 37 proxy handshake inventories (including unique-spawned proxy pids that show up in the paint log while still the `WebContentProxy` image) have `ports_lookup` count=3, slot 0 SEND only (`typ=0x10000`), `[1]=0x0`, `[2]=0x0`, `recv_rights=0`. The COPY_SEND rendezvous right that Safari placed in slot 1 via `posix_spawnattr_set_registered_ports_np` is not visible in the 0755 proxy at inventory time, so the helper could not have sent an endpoint even if it had reached `xpc_main`. The stub logged `will_exec_priv=0` (it did not exec the `.priv` sibling). The proxy does not unregister ports itself; either `set_registered_ports_np` did not install the right, or something in early proxy / inserted-libmachook startup cleared slot 1 before that inventory.

2. Timing. Unique pids appear as `WebContentProxy` in CAContext hook install logs and are SIGKILL'd after 150 ms; they never appear as stock `com.apple.WebKit.WebContent` `xpc_main` lines. Even with a valid slot 1, 150 ms is likely shorter than chroot + execve + dyld + `xpc_main`. A later change can lengthen that cap modestly, but not with a 2 s-per-helper `bootstrap_look_up` stall.

Fallback helpers still have no receive right. Zero accepted connections, no `InitializeWebProcess` / `CreateWebPage`, no WebContent `remoteContextWithOptions` (only Safari chrome CAContexts from pid 64924).

### What was not done

No second variant. Because the Safari window stayed visible, net22 libmachook was left deployed (arm64e `98f44436…`) with net20 proxies and the net19 path layout. Rollback copies remain at `pre-net22-20261110-214530` (the pre-deploy net20 libmachook `1cd2631f…` and proxies `2097451d…`), `pre-net21-20261110-212258`, `pre-net20-20261110-211200`, `pre-net19-20261110-205054` (path layout), `pre-net18-20261110-203748`, and `pre-net17-20261110-202135`. No SkyLight hooks, no extra Safari CAContext IMPs, no constructor `dispatch_after`, no new remapper export, no public macosbooter check-in, no competing Mach listener on that name, no CAContext port-retry, no macwshostd restart, no reboot, no 停止 macOS. IconServices jobs were left loaded.

### Paint / IPC conclusion

Fail-fast unique-name spawn still runs (`wk-peer spawn rc=0`) and no longer blocks Safari's launch thread long enough to miss the window: the window is visible, example.com and baidu.com open, chrome paints, screenshots are at `/workspace/net22-example.png` and `/workspace/net22-baidu.png`. The unique-name peer does **not** actually connect. `bootstrap_look_up` is no longer used; the remaining gap is that the inherited rendezvous send right is not look-up-able in the proxy (`slot 1 = 0`) and the unique child never reaches the anonymous-listener / `xpc_pipe` send before the 150 ms timeout. Fallback add_bundle still starts helpers with no receive right, and there is no InitializeWebProcess / CreateWebPage / WebContent CAContext / page paint (content still white). Next work should keep the net19 layout, the TSV-hit reordering, and the fail-fast cap, and should get a usable rendezvous send right into the helper across the proxy exec — without checking in the public macosbooter name, adding a competing Mach listener on that name, retrying CAContext ports, or stalling 2 s per helper.

## 40. Parent bootstrap_register plus helper look_up can connect a unique peer, but 800 ms × N missed the Safari window (2026-11-10 21:57–22:00 CST / 05:57–06:00 PT)

### Change

net22's slot-1 `posix_spawnattr_set_registered_ports_np` COPY_SEND never appeared in the 0755 WebContentProxy handshake (`[1]=0x0`), and unique children were SIGKILL'd at 150 ms before stock `xpc_main`. A same-chroot probe this turn (`net23_rvprobe`, `CHROOT_RC=0`) split those findings:

- `posix_spawnattr_set_registered_ports_np` *does* work for a macOS-minos 13 child in this chroot: the child sees slot 1 as a SEND right and `xpc_pipe_simpleroutine` of an anonymous-listener endpoint succeeds.
- `bootstrap_register` of `com.macwsguide.wk.i.<32 hex>` keeps the receive right in the caller (`typ=0x30000` after register). A child `bootstrap_look_up` gets a SEND, and the parent `xpc_pipe_receive` + `xpc_connection_create_from_endpoint` succeeds (`timed_out=0`, `pipe_rc=0`).

net23 therefore keeps the TSV-hit reordering and the fail-fast pipe/endpoint handshake, and adds a bootstrap-independent *name* publish that does not depend on the 0755 proxy inheriting slot 1: Safari `bootstrap_register`s the rendezvous receive under the unique peer name (not a public macosbooter check-in), and the helper, after exec into the stock image, uses slot 1 if present or else `bootstrap_look_up` of that unique name. The wait cap was raised from 150 ms to 800 ms (still well under the old 2 s-per-helper stall) so chroot + execve + dyld + `xpc_main` has a better chance to send the endpoint. Proxies and the net19 path layout were left on net20 / net19. No new exported symbols (168 T per slice). 12 contract tests OK on the Intel Mac (7 peer-spawn + 5 environment).

### Deploy

Backup tag `pre-net23-20261110-215737` (21:57:37 CST / 05:57:37 PT) saved the live net22 libmachook (`98f4443646ba1e480075044de20deba064248675`) and net20 proxies (WebContent `2097451dd27a6c353c69c88c6ccb5c62acb14e5b`). The net19 path layout was not touched. Only libmachook was replaced (same cross-build / two-pass `ldid` / trustcache / rootfs byte-copy path as net22). Live net23 CDHashes (while deployed): arm64e `67d741e3c6b6ff33a7464d51f955a6e4226cf3a8`, arm64 `8c7e873ed711f0fc0fd761225bd008323a9fcd81`. Smoke at 21:57:43 CST / 05:57:43 PT: `SMOKE_OK` / `RUN_BASH_CANARY_OK` rc=0, IconServices 1724 / 53067 / 53071 loaded, WindowServer 51054 `Ss`, macwshostd 416, SpringBoard 391. `SMOKE_PASS`. No reboot, no 停止 macOS.

### Safari run (one guarded attempt)

21:57:47 CST / 05:57:47 PT onward, Safari pid 66268. The launcher reported `ok=no … 已启动，但 30 秒内没有发布可捕获的 AppKit 窗口`. After that 30 s Safari was still running (RSS 190400 KB, below the healthy visible-window class of ~593 MB) with fallback WebContent + one unique-connected Networking helper, but no capturable AppKit window. The script did not open example.com or baidu.com (`LAUNCH_WINDOW_FAIL; not opening URLs`) and wrote `DONE_NO_WINDOW`. Cleanup `CORE_OK`, WebKit LaunchDaemons unloaded, IconServices left loaded. No `/workspace/net23-example.png` or `/workspace/net23-baidu.png`.

Because Safari lost a visible/capturable window, net23 was **rolled back immediately** to `pre-net23-20261110-215737` (the net22 libmachook that was live before this deploy). No second variant. Post-rollback smoke at 22:00:13 CST / 06:00:13 PT: `SMOKE_PASS`, libmachook arm64e again `98f4443646ba1e480075044de20deba064248675`, WebContentProxy still `2097451dd27a6c353c69c88c6ccb5c62acb14e5b`, IconServices 1724 / 53067 / 53071, WindowServer 51054 `Ss`.

### Field result

Spawn and register both ran. 25 `wk-peer register kr=0` and 25 `wk-peer spawn rc=0` lines, unique `com.macwsguide.wk.i.<32 hex>` names, no `register-fail`, no public macosbooter check-in. One helper actually connected: Networking child 66294 logged `xpc_main WebKit unique peer anonymous listener=… rv=0x1403` and `endpoint-send rc=0` *after* exec into the stock `com.apple.WebKit.Networking` image, and Safari logged `wk-peer connected … conn=0x…`. That is the first unique-name peer connection in this stream, and it used the post-exec look_up path rather than inherited slot 1.

The 0755 proxy still never carries slot 1: all 23 handshake inventories have `[1]=0x0`, `[2]=0x0`, `recv_rights=0` (same as net22). The right that reached Networking arrived by `bootstrap_look_up` of the parent-registered unique name after stock exec.

23 of 25 spawns then hit `wk-peer endpoint-timeout wait_ms=800 timed_out=1` and fell back to add_bundle (`tsv=hit`). Almost all `xpc_main WebKit inherited path` lines are those fallback helpers (`peer=(null)`, `flags=8019`, `service=com.apple.macosbooter.WebKit.*`). One later unique WebContent (pid 66639) did reach stock `xpc_main` with `MACWS_WK_PEER` set and created an anonymous listener, but `rv=0x0` and `endpoint-send rc=-1` — Safari had already destroyed the rendezvous receive after the 800 ms timeout. 23 × 800 ms on Safari's application-service thread is the likely reason the AppKit window was not published within 30 s (same class of failure as net21's 2 s × N, even though the per-helper cap stayed under 2 s).

Zero accepted connections, no `InitializeWebProcess` / `CreateWebPage`, no WebContent `remoteContextWithOptions` (only a Safari chrome CAContext from pid 66268).

### What was not done

No second variant. net23 libmachook was rolled back; the device is again on net22 libmachook + net20 proxies + the net19 path layout. Rollback copies remain at `pre-net23-20261110-215737` (this turn's `67d741e3…` dylib, restoring net22 `98f44436…`), `pre-net22-20261110-214530` (net20 libmachook `1cd2631f…` and proxies `2097451d…`), `pre-net21-20261110-212258`, `pre-net20-20261110-211200`, `pre-net19-20261110-205054` (path layout), `pre-net18-20261110-203748`, and `pre-net17-20261110-202135`. No SkyLight hooks, no extra Safari CAContext IMPs, no constructor `dispatch_after`, no new remapper export, no public macosbooter check-in, no competing Mach listener on that name, no CAContext port-retry, no macwshostd restart, no reboot, no 停止 macOS. IconServices jobs were left loaded.

### Paint / IPC conclusion

The rendezvous send right *can* be made visible after proxy exec: Safari's `bootstrap_register` of the unique name succeeds (`kr=0`, receive stays with Safari), and at least the Networking helper reached stock `xpc_main`, obtained a send right (`rv=0x1403`), sent the anonymous-listener endpoint (`endpoint-send rc=0`), and Safari logged `wk-peer connected`. Slot 1 is still not inherited by the 0755 proxy. WebContent generally does not reach that send within 800 ms, and 23 synchronous 800 ms waits prevented Safari from publishing a capturable window. There is no InitializeWebProcess / CreateWebPage / WebContent CAContext / page paint, and there are no net23 screenshots. net23 was rolled back to net22. Next work should keep the register + post-exec look_up path (it is the first proven unique-peer connect) but must not block Safari's launch thread for 800 ms × N helpers — without checking in the public macosbooter name, adding a competing Mach listener on that name, or retrying CAContext ports.

## 41. Parallel unique-peer pool connects WebContent without stalling Safari's window (2026-11-10 22:13–22:14 CST / 06:13–06:14 PT)

### Change

net23 proved that Safari's `bootstrap_register` of `com.macwsguide.wk.i.<32 hex>` plus helper `bootstrap_look_up` after stock exec can complete the anonymous-listener / `xpc_pipe` handshake (Networking connected, `endpoint-send rc=0`, `wk-peer connected`), but 23 synchronous 800 ms waits on the application-service thread missed the 30 s capturable-window deadline, and destroying the receive right on that timeout left a late WebContent with `rv=0x0` / `endpoint-send rc=-1`.

net24 keeps the net21 TSV-hit reordering and the net23 register + post-exec look_up path. It stops waiting on Safari's launch thread for a long cap times N. On the first public WebKit / SandboxBroker `create_application_service`, Safari pre-spawns a unique-peer pool in parallel (8 WebContent + 1 Networking + 1 GPU + 1 SandboxBroker; WebContent is topped up as slots are claimed). `xpc_pipe_receive` runs in the background immediately. The receive right is kept until the child sends or `MACWS_WK_RV_GRACE_MS` 5000 ms, and the child is not SIGKILL'd on a short caller-thread timeout. The application-service thread only pops a ready connection or waits once per leaf up to `MACWS_WK_RV_POP_MS` 3000 ms; later pops for that leaf return immediately if nothing is ready yet, and fall back to add_bundle + macosbooter `create_fn` while the unique child and recv stay alive until grace. No constructor `dispatch_after`. No new exported symbols (still 168 T per slice). No public macosbooter check-in, no competing Mach listener on that name. Proxies and the net19 path layout were left on net20 / net19. 12 contract tests OK on the Intel Mac (7 peer-spawn + 5 environment).

### Deploy

Backup tag `pre-net24-20261110-221300` (22:13:00 CST / 06:13:00 PT) saved the live net22 libmachook (`98f4443646ba1e480075044de20deba064248675`) and net20 proxies (WebContent `2097451dd27a6c353c69c88c6ccb5c62acb14e5b`). The net19 path layout was not touched. Only libmachook was replaced (same cross-build / two-pass `ldid` / trustcache / rootfs byte-copy path as net22/net23). Live net24 CDHashes: arm64e `97b597710bd71b565c577a59d5990607051dd016`, arm64 `f164506b6569ce318fc252a2d8e6b0a21adf1d1c`. Smoke at 22:13:06 CST / 06:13:06 PT: `SMOKE_OK` / `RUN_BASH_CANARY_OK` rc=0, IconServices 1724 / 53067 / 53071 loaded, WindowServer 51054 `Ss`, macwshostd 416, SpringBoard 391. `SMOKE_PASS`. No reboot, no 停止 macOS.

### Safari run (one guarded attempt)

22:13:14 CST / 06:13:14 PT onward, Safari pid 67312. The launcher reported `LAUNCH_WINDOW_OK` (`ok=yes`). After example.com Safari's RSS was 645792 KB (~631 MB); after baidu it was 646048 KB, in the healthy visible-window class. example.com and baidu.com were opened and captured: `/workspace/net24-example.png` and `/workspace/net24-baidu.png` (clocks 06:14 PT). Chrome (tabs, URL bar, toolbar) painted; page content stayed white, same as net22. Cleanup `CORE_OK`, WebKit LaunchDaemons unloaded, IconServices left loaded. Because the window stayed capturable, net24 was **not** rolled back. No second variant.

### Field result

The unique-name spawn ran without stalling the window (0 `pop-timeout`, 0 `endpoint-timeout`). `/tmp/macws-xpc-app.log` has 1 `wk-peer prewarm wc=8 net=1 gpu=1 sandbox=1`, 32 `wk-peer register kr=0`, and 32 `wk-peer spawn rc=0` (29 WebContent + 1 Networking + 1 GPU + 1 SandboxBroker) with distinct `com.macwsguide.wk.i.<32 hex>` names. 31 `wk-peer ready` and 30 `wk-peer connected`: **29 unique WebContent** plus Networking. GPU became `ready` (`endpoint-send rc=0`) but was not popped (`no wk-peer connected` for that leaf). SandboxBroker hit the one `grace-timeout` / `pop-miss` (child 67341) and fell back (`tsv=hit`); there were 10 fallbacks total (7 WebContent after the 32-slot pool filled, 3 SandboxBroker). No public macosbooter check-in.

Helpers reached stock `xpc_main` and completed the handshake: 31 `unique peer anonymous listener` lines and 31 `endpoint-send rc=0` (29 WebContent + Networking + GPU), with valid rendezvous sends (`rv=0x1103`–`0x1603`). Zero `endpoint-send rc=-1`. This is the first time WebContent unique peers have connected in this stream.

The 0755 proxy handshake inventories still have `recv_rights=0` (the right handed over is a COPY_SEND). Unlike net22/net23, the 32 unique-spawned proxy inventories now show slot 1 as a SEND (`[1]=… typ=0x10000`); the 8 fallback inventories still have `[1]=0x0`. Unique children also still have the post-exec look_up path.

Zero accepted connections, no `InitializeWebProcess` / `CreateWebPage` IPC. Stock WebContent processes did install CAContext hooks (`wc=1` after exec into `com.apple.WebKit.WebContent`), but there is no WebContent `remoteContextWithOptions` (the five `remoteContextWithOptions` lines are Safari chrome, AppKit open/save, QuickLook, and screencapture; all `portNumber=0`).

### What was not done

No second variant. Because the Safari window stayed visible, net24 libmachook was left deployed (arm64e `97b59771…`) with net20 proxies and the net19 path layout. Rollback copies remain at `pre-net24-20261110-221300` (the pre-deploy net22 libmachook `98f44436…` and proxies `2097451d…`), `pre-net23-20261110-215737` (net23 `67d741e3…`, restoring net22), `pre-net22-20261110-214530` (net20 libmachook `1cd2631f…` and proxies `2097451d…`), `pre-net21-20261110-212258`, `pre-net20-20261110-211200`, `pre-net19-20261110-205054` (path layout), `pre-net18-20261110-203748`, and `pre-net17-20261110-202135`. No SkyLight hooks, no extra Safari CAContext IMPs, no constructor `dispatch_after`, no new remapper export, no public macosbooter check-in, no competing Mach listener on that name, no CAContext port-retry, no macwshostd restart, no reboot, no 停止 macOS. IconServices jobs were left loaded.

### Paint / IPC conclusion

Parallel pre-spawn plus a kept-alive receive right lets unique WebContent (and Networking) reach stock `xpc_main`, look up / inherit the rendezvous send, deliver the anonymous-listener endpoint, and connect, without 800 ms × N stalls: the Safari window is visible, example.com and baidu.com open, chrome paints, and screenshots are at `/workspace/net24-example.png` and `/workspace/net24-baidu.png`. WebContent unique peers **did connect** (29 `wk-peer connected`). The remaining gap is that those connections do not yet carry WebKit process IPC: there are no accepted connections, no InitializeWebProcess / CreateWebPage, no WebContent `remoteContextWithOptions`, and page content is still white. Next work should keep the register + post-exec look_up path and the non-blocking pool, and should get a connected unique WebContent to accept the Safari connection and run InitializeWebProcess / CreateWebPage — without checking in the public macosbooter name, adding a competing Mach listener on that name, retrying CAContext ports, or stalling Safari's launch thread for a long wait times N.

## 42. Unique helpers enter dispatch_main and hand the peer to xpc_main, but WebKit IPC still does not arrive (2026-11-10 22:27–22:29 CST / 06:27–06:29 PT)

### Change

net24 connected 29 unique WebContent peers plus Networking, and the Safari window stayed visible, but there were 0 accepted connections, 0 InitializeWebProcess / CreateWebPage, no WebContent `remoteContextWithOptions`, and page content stayed white.

Diagnosis of the net24 helper path, from the same logs: every unique child logged `endpoint-send rc=0` and then nothing else — never `entering NSApp run`, never `entering dispatch_main`. The unique path created an anonymous `xpc_connection_create(NULL)` (a peer, not a Mach LISTENER), armed an event handler that only accepted `XPC_TYPE_CONNECTION`, resumed, sent the endpoint, and then called `+[NSApplication sharedApplication]` / `-run` on the `xpc_main` stack, before any run loop was draining the main queue. Two consequences follow: (1) Safari's messages arrive as dictionaries on that anonymous peer itself, so the CONNECTION-only handler would drop `InitializeWebProcess`; (2) `sharedApplication` on that stack can `dispatch_sync` a main queue that is not yet running, which matches helpers stalling after `endpoint-send` and never processing events.

net25 keeps the non-blocking pre-spawn pool, the kept-alive receive right, register + post-exec look_up, and TSV-hit reordering. On the unique path it hands the anonymous peer to the stock `xpc_main` connection handler *before* sending the endpoint, then drains the main queue with `dispatch_main()` instead of NSApp on that stack. `xpc_connection_set_instance` on a remembered unique peer (`com.macwsguide.wk.i.*`) is skipped (instance UUIDs are launchd MultipleInstances metadata and do not apply to a `create_from_endpoint` peer). No public macosbooter check-in, no competing Mach listener on that name, no new exported symbols (168 T per slice), no constructor `dispatch_after`. Proxies and the net19 path layout stayed on net20 / net19. 12 contract tests OK on the Intel Mac (7 peer-spawn + 5 environment).

### Deploy

Backup tag `pre-net25-20261110-222759` (22:27:59 CST / 06:27:59 PT) saved the live net24 libmachook (`97b597710bd71b565c577a59d5990607051dd016`) and net20 proxies (WebContent `2097451dd27a6c353c69c88c6ccb5c62acb14e5b`). The net19 path layout was not touched. Only libmachook was replaced (same cross-build / two-pass `ldid` / trustcache / rootfs byte-copy path). Live net25 CDHashes: arm64e `d76ecf50a089e7588c5f5f28e3c48763d78cc972`, arm64 `bb06d275f767b8596b380940f19c8176ca586962`. Smoke at 22:28:09 CST / 06:28:09 PT: `SMOKE_OK` / `RUN_BASH_CANARY_OK` rc=0, IconServices 1724 / 53067 / 53071 loaded, WindowServer 51054 `Ss`, macwshostd 416, SpringBoard 391. `SMOKE_PASS`. No reboot, no 停止 macOS.

### Safari run (one guarded attempt)

22:28:15 CST / 06:28:15 PT onward, Safari pid 68658. The launcher reported `LAUNCH_WINDOW_OK` (`ok=yes`). After example.com Safari's RSS was 675584 KB (~660 MB); after baidu it was 675920 KB, in the healthy visible-window class. example.com and baidu.com were opened and captured: `/workspace/net25-example.png` and `/workspace/net25-baidu.png` (clocks 06:29 PT). Chrome (tabs, URL bar, toolbar) painted; page content stayed white. Cleanup `CORE_OK`, WebKit LaunchDaemons unloaded, IconServices left loaded. Because the window stayed capturable, net25 was **not** rolled back. No second variant.

### Field result

The helper-side stall is fixed. 31 unique children logged `anonymous listener`, `xpc_main accepted connection` (same pointer as the anonymous peer), `handed to xpc_main handler` (the stock handler returned, it did not hang), `endpoint-send rc=0`, and `entering dispatch_main`. The handshake is unchanged in the good ways: 1 prewarm, 32 `register kr=0`, 32 `spawn rc=0`, 31 ready, 30 `wk-peer connected` (29 WebContent + Networking). GPU became ready but was not popped. SandboxBroker was the one grace-timeout / pop-miss and fell back; 12 fallbacks total. 0 pop-timeout, 0 endpoint-timeout, 0 `xpc_error`, 0 `skip-set-instance`. No public macosbooter check-in.

Those 31 `accepted connection` / `InitializeWebProcess` / `CreateWebPage` *string* hits in the init log are the diagnostic text on the handoff line (`(InitializeWebProcess/CreateWebPage ride this conn)`), not WebKit process IPC. There were **0** `xpc event begin` lines, **0** `InitializeWebKit2 call` / `InitializeWebKit2 orig returned`, and **0** `runInitializationCode call`. Stock WebContent still installed CAContext hooks (`wc=1`) after exec, but there is no WebContent `remoteContextWithOptions` (the five `remoteContextWithOptions` lines are Safari chrome, AppKit open/save, QuickLook, and screencapture; all `portNumber=0`).

### What was not done

No second variant. Because the Safari window stayed visible, net25 libmachook was left deployed (arm64e `d76ecf50…`) with net20 proxies and the net19 path layout. Rollback copies remain at `pre-net25-20261110-222759` (the pre-deploy net24 libmachook `97b59771…` and proxies `2097451d…`), `pre-net24-20261110-221300` (net22 `98f44436…` and proxies `2097451d…`), `pre-net23-20261110-215737`, `pre-net22-20261110-214530`, `pre-net21-20261110-212258`, `pre-net20-20261110-211200`, `pre-net19-20261110-205054` (path layout), `pre-net18-20261110-203748`, and `pre-net17-20261110-202135`. No SkyLight hooks, no extra Safari CAContext IMPs, no constructor `dispatch_after`, no new remapper export, no public macosbooter check-in, no competing Mach listener on that name, no CAContext port-retry, no macwshostd restart, no reboot, no 停止 macOS. IconServices jobs were left loaded.

### Paint / IPC conclusion

Unique WebContent peers still connect, and the helper now stays in `dispatch_main` after handing that anonymous peer to WebKit's `xpc_main` connection handler — the net24 post-`endpoint-send` stall is gone, and the Safari window stays visible. Screenshots: `/workspace/net25-example.png` and `/workspace/net25-baidu.png` (chrome painted, content white). WebContent did **not** accept Safari process IPC and did **not** run InitializeWebProcess / CreateWebPage as messages: 0 `xpc event`, 0 `InitializeWebKit2 call`, no WebContent CAContext, content still white. The remaining gap is on the already-handed peer after the handler returns — Safari may not be sending on the `create_from_endpoint` connection, the stock handler may not arm a message callback on an anonymous already-bound peer the way it does on an xpcproxy accept, or a shared-cache `xpc_connection_set_instance` may still be reaching libxpc (this turn logged 0 skips). Next work should keep the pool, the kept-alive recv, and `dispatch_main` handoff, and should get a dictionary event / InitializeWebProcess onto that unique WebContent peer — without checking in the public macosbooter name, adding a competing Mach listener on that name, retrying CAContext ports, or stalling Safari's launch thread.

## 43. Shared-cache set_instance is skipped and Safari does send, but unique WebContent still gets no dictionary events (2026-11-10 22:36–22:38 CST / 06:36–06:38 PT)

### Change

net25 kept the Safari window visible and got unique helpers into `dispatch_main` after handing the anonymous peer to WebKit's `xpc_main` handler, but there were still 0 `xpc event begin`, 0 `InitializeWebKit2 call`, no WebContent `remoteContextWithOptions`, and page content stayed white. The 31 init-log `InitializeWebProcess` / `CreateWebPage` hits were diagnostic text on the handoff line, not process IPC. That turn also logged 0 `skip-set-instance`, so the DYLD_INTERPOSE early hook never saw unique-peer `set_instance`.

Three hypotheses were on the table: (1) Safari may not be sending on the `create_from_endpoint` connection; (2) the stock handler may not arm a message callback the way it does on an xpcproxy accept; (3) a shared-cache `xpc_connection_set_instance` may still reach libxpc and invalidate the peer (QuickLookSatellite precedent already in-tree). A fourth issue was that `MACWS_XPC_APP_CONN_MAX` was 32 while the field run had 30 unique + 12 fallback plus other app connections, so early unique peers could be evicted from the remember table.

net26 keeps the non-blocking pre-spawn pool, the kept-alive receive right, register + post-exec look_up, TSV-hit reordering, and the `dispatch_main` handoff. It grows the remember table to 128, remembers the anonymous helper peer before the `xpc_main` handoff, and logs `wk-peer set-event-handler` on both sides. Metal_hooks now MSHooks shared-cache `_xpc_connection_set_instance` (skips remembered `com.macwsguide.wk.i.*` peers and logs `wk-peer skip-set-instance`), plus log-only hooks for `_xpc_connection_resume`, `_xpc_connection_send_message`, and `_xpc_connection_send_message_with_reply`. The skip/log helpers are hidden (`t`, not exported `T`); still 168 T symbols per slice. No public macosbooter check-in, no competing Mach listener on that name, no constructor `dispatch_after`. Proxies and the net19 path layout stayed on net20 / net19. 13 contract tests OK on the Intel Mac (8 peer-spawn + 5 environment).

### Deploy

Backup tag `pre-net26-20261110-223631` (22:36:31 CST / 06:36:31 PT) saved the live net25 libmachook (`d76ecf50a089e7588c5f5f28e3c48763d78cc972`) and net20 proxies (WebContent `2097451dd27a6c353c69c88c6ccb5c62acb14e5b`). The net19 path layout was not touched. Only libmachook was replaced (same cross-build / two-pass `ldid` / trustcache / rootfs byte-copy path; hidden `macws_xpc_app_logf` / `macws_xpc_lookup_app_conn_name` stay `t`). Live net26 CDHashes: arm64e `0551602b71e1f4203edac3f5e7265d770048ac57`, arm64 `25400323cafd227ef9e6122f3f97f46fc4148265`. Smoke at 22:36:38 CST / 06:36:38 PT: `SMOKE_OK` / `RUN_BASH_CANARY_OK` rc=0, IconServices 1724 / 53067 / 53071 loaded, WindowServer 51054 `Ss`, macwshostd 416, SpringBoard 391. `SMOKE_PASS`. No reboot, no 停止 macOS.

### Safari run (one guarded attempt)

22:36:44 CST / 06:36:44 PT onward, Safari pid 69894. The launcher reported `LAUNCH_WINDOW_OK` (`ok=yes`). After example.com Safari's RSS was 702624 KB (~686 MB); after baidu it was 702816 KB, in the healthy visible-window class. example.com and baidu.com were opened and captured: `/workspace/net26-example.png` and `/workspace/net26-baidu.png` (clocks 06:37 PT). Chrome (tabs, URL bar, toolbar) painted; page content stayed white. Cleanup `CORE_OK`, WebKit LaunchDaemons unloaded, IconServices left loaded. Because the window stayed capturable, net26 was **not** rolled back. No second variant.

### Field result

Handshake stays in the good net25 shape: 1 prewarm, 32 `register kr=0`, 32 `spawn rc=0`, 31 ready, 30 `wk-peer connected` (29 WebContent + Networking). GPU became ready (`endpoint-send rc=0`) but was not popped. SandboxBroker was the one grace-timeout / pop-miss and fell back; 14 fallbacks total (11 WebContent after the pool could not satisfy later claims, 3 SandboxBroker). 0 pop-timeout, 0 endpoint-timeout, 0 `xpc_error`. 31 unique children still logged `accepted connection` (same pointer as the anonymous peer), `handed to xpc_main handler`, `endpoint-send rc=0`, and `entering dispatch_main`. No public macosbooter check-in (`bootstrap_check_in` 0; `macosbooter.WebKit` lines are fallback activate names).

The three hypotheses can now be scored from the new logs:

- Hypothesis 3 was true in net25 and is now fixed at the hook: Safari pid 69894 logged **30** `wk-peer skip-set-instance` on the unique `create_from_endpoint` connections (shared-cache WebKit *does* call `xpc_connection_set_instance`; the DYLD_INTERPOSE path never saw it).
- Hypothesis 1 is false: after skip, Safari armed `wk-peer set-event-handler … helper=0` (60 lines), `wk-peer resume` (30 from pid 69894), and **30** `wk-peer send-message-with-reply` on those same connection pointers. It *is* sending on the unique peer (ProcessLauncher uses with-reply, not plain `send_message`; 0 plain `send-message`).
- Hypothesis 2 is only partly false: the stock handler *does* arm a callback. Unique helpers logged 62 `set-event-handler … helper=1` and then `resume` (31 helper pids). The first WebContent timeline is ordered correctly: helper set-event-handler + resume, then Safari `ready` / `connected` / `skip-set-instance` / set-event-handler / resume / `send-message-with-reply`.

What still does not happen is delivery: **0** `xpc event begin`, **0** `InitializeWebKit2 call` / `InitializeWebKit2 orig returned`, **0** `runInitializationCode call`. The 31 init-log `InitializeWebProcess` / `CreateWebPage` hits are again the handoff-line diagnostic text (`ride this conn`), not messages. Stock WebContent still installed CAContext hooks (`wc=1`) after exec, but there is no WebContent `remoteContextWithOptions` (the five `remoteContextWithOptions` lines are Safari chrome, AppKit open/save, QuickLook, and two screencapture; all `portNumber=0`).

### What was not done

No second variant. Because the Safari window stayed visible, net26 libmachook was left deployed (arm64e `0551602b…`) with net20 proxies and the net19 path layout. Rollback copies remain at `pre-net26-20261110-223631` (the pre-deploy net25 libmachook `d76ecf50…` and proxies `2097451d…`), `pre-net25-20261110-222759` (net24 `97b59771…` and proxies `2097451d…`), `pre-net24-20261110-221300`, `pre-net23-20261110-215737`, `pre-net22-20261110-214530`, `pre-net21-20261110-212258`, `pre-net20-20261110-211200`, `pre-net19-20261110-205054` (path layout), `pre-net18-20261110-203748`, and `pre-net17-20261110-202135`. No SkyLight hooks, no extra Safari CAContext IMPs, no constructor `dispatch_after`, no new remapper export, no public macosbooter check-in, no competing Mach listener on that name, no CAContext port-retry, no macwshostd restart, no reboot, no 停止 macOS. IconServices jobs were left loaded.

### Paint / IPC conclusion

The window stays visible, unique peers still connect, and the field logs now show that Safari *does* skip `set_instance`, arm a handler, resume, and `send_message_with_reply` on the unique `create_from_endpoint` connection, while the helper *does* arm a callback and resume before that send. Screenshots: `/workspace/net26-example.png` and `/workspace/net26-baidu.png` (chrome painted, content white). InitializeWebProcess / CreateWebPage still did **not** arrive as real IPC: 0 `xpc event begin`, 0 `InitializeWebKit2 call`, no WebContent CAContext, content still white. The remaining gap is message *delivery* on that already-resumed anonymous peer after `send_message_with_reply` — the with-reply transaction or pipe/endpoint channel may not be carrying dictionaries the way an xpcproxy accept does. Next work should keep the pool, the kept-alive recv, the `dispatch_main` handoff, and the shared-cache `set_instance` skip, and should get a dictionary event / InitializeWebProcess onto that unique WebContent peer — without checking in the public macosbooter name, adding a competing Mach listener on that name, retrying CAContext ports, or stalling Safari's launch thread.

## 44. Listener-style accept on the anonymous endpoint still gets no events (2026-11-10 22:43–22:45 CST / 06:43–06:45 PT)

### Change

net26 showed that Safari *does* skip shared-cache `set_instance`, arm a handler, resume, and `send_message_with_reply` on the unique `create_from_endpoint` connection, and that unique helpers *do* arm a callback and resume before that send — but 0 `xpc event begin` arrived and page content stayed white. The remaining hypothesis was that `xpc_connection_create(NULL)` is an anonymous LISTENER, so Safari's attach should arrive as `XPC_TYPE_CONNECTION` and InitializeWebProcess dictionaries should ride the *accepted* connection. net25/net26 had handed the listener object itself to WebKit's `xpc_main` handler (a dictionary callback) before endpoint-send, which would drop accepts.

net27 keeps the non-blocking pre-spawn pool, the kept-alive receive right, register + post-exec look_up, TSV-hit reordering, `dispatch_main` handoff, and the shared-cache `set_instance` skip. On the unique path the helper now arms a `XPC_TYPE_CONNECTION` accept handler on the anonymous listener, resumes it, sends the endpoint, and enters `dispatch_main()`. Only an accepted connection is remembered and handed to the stock `xpc_main` handler. Unique-peer events are also logged to the xpc-app log as `wk-peer xpc-event` with type `connection` / `dictionary`. No public macosbooter check-in, no named Mach LISTENER on that public name, no competing listener, no constructor `dispatch_after`, no new exported symbols (168 T per slice). Proxies and the net19 path layout stayed on net20 / net19. 13 contract tests OK on the Intel Mac (8 peer-spawn + 5 environment).

### Deploy

Backup tag `pre-net27-20261110-224355` (22:43:55 CST / 06:43:55 PT) saved the live net26 libmachook (`0551602b71e1f4203edac3f5e7265d770048ac57`) and net20 proxies (WebContent `2097451dd27a6c353c69c88c6ccb5c62acb14e5b`). The net19 path layout was not touched. Only libmachook was replaced (same cross-build / two-pass `ldid` / trustcache / rootfs byte-copy path). Live net27 CDHashes: arm64e `563a6880de0f838f0b8b9cd03a95998dd3def986`, arm64 `be1cee0bd68b97e66be37725a9d964ee70204246`. Smoke at 22:44:00 CST / 06:44:00 PT: `SMOKE_OK` / `RUN_BASH_CANARY_OK` rc=0, IconServices 1724 / 53067 / 53071 loaded, WindowServer 51054 `Ss`, macwshostd 416, SpringBoard 391. `SMOKE_PASS`. No reboot, no 停止 macOS.

### Safari run (one guarded attempt)

22:44:04 CST / 06:44:04 PT onward, Safari pid 71266. The launcher reported `LAUNCH_WINDOW_OK` (`ok=yes`). After example.com Safari's RSS was 729824 KB (~713 MB); after baidu it was 730096 KB, in the healthy visible-window class. example.com and baidu.com were opened and captured: `/workspace/net27-example.png` and `/workspace/net27-baidu.png` (clocks 06:44 PT and 06:45 PT). Chrome (tabs, URL bar, toolbar) painted; page content stayed white. Cleanup `CORE_OK`, WebKit LaunchDaemons unloaded, IconServices left loaded. Because the window stayed capturable, net27 was **not** rolled back. No second variant.

### Field result

Handshake stayed in the good net25/net26 shape: 1 prewarm, 32 `register kr=0`, 32 `spawn rc=0`, 31 ready, 30 `wk-peer connected` (29 WebContent + Networking). GPU became ready but was not popped. SandboxBroker was the one grace-timeout / pop-miss and fell back; 16 fallbacks total (13 WebContent, 3 SandboxBroker). 0 pop-timeout, 0 endpoint-timeout, 0 `xpc_error`. 31 unique children logged `anonymous listener`, `listening for incoming connection`, `endpoint-send rc=0`, and `entering dispatch_main`. Helper `set-event-handler helper=1` + `resume` now fire once on the listener (no premature stock-handler handoff). Safari again logged 30 `skip-set-instance`, 60 `set-event-handler helper=0`, 30 resumes, and 30 `send-message-with-reply` on the `create_from_endpoint` conns. No public macosbooter check-in (`bootstrap_check_in` 0).

The listener-accept hypothesis was not confirmed in the field. There were **0** `unique peer incoming connection=` lines (the 31 init-log hits for the substring `incoming connection` are the `listening for incoming connection` arming line, not accepts). **0** `wk-peer xpc-event`, **0** `xpc event begin` (connection or dictionary), **0** `listener event type`, **0** `accepted connection`, **0** `InitializeWebKit2 call` / orig returned, **0** `runInitializationCode call`. The misleading net25/net26 handoff-line `InitializeWebProcess` / `CreateWebPage` diagnostic text is gone; those strings are now honestly 0 in the init and paint logs. Stock WebContent still installed CAContext hooks (`wc=1`) after exec, but there is no WebContent `remoteContextWithOptions` (the five `remoteContextWithOptions` lines are Safari chrome, AppKit open/save, QuickLook, and two screencapture; all `portNumber=0`).

Together with net26 (dictionary handler on the same object also saw 0 events), this means the delivery failure is not just "wrong handler type on a live listener." Neither `XPC_TYPE_CONNECTION` accepts nor dictionaries arrive on the helper after Safari's `create_from_endpoint` + `send_message_with_reply`. The pipe/endpoint channel still does not deliver messages onto the helper connection, even though endpoint-send returns 0 and Safari's send path logs no `xpc_error`.

### What was not done

No second variant. Because the Safari window stayed visible, net27 libmachook was left deployed (arm64e `563a6880…`) with net20 proxies and the net19 path layout. Rollback copies remain at `pre-net27-20261110-224355` (the pre-deploy net26 libmachook `0551602b…` and proxies `2097451d…`), `pre-net26-20261110-223631` (net25 `d76ecf50…` and proxies `2097451d…`), `pre-net25-20261110-222759`, `pre-net24-20261110-221300`, `pre-net23-20261110-215737`, `pre-net22-20261110-214530`, `pre-net21-20261110-212258`, `pre-net20-20261110-211200`, `pre-net19-20261110-205054` (path layout), `pre-net18-20261110-203748`, and `pre-net17-20261110-202135`. No SkyLight hooks, no extra Safari CAContext IMPs, no constructor `dispatch_after`, no new remapper export, no public macosbooter check-in, no competing Mach listener on that name, no CAContext port-retry, no macwshostd restart, no reboot, no 停止 macOS. IconServices jobs were left loaded.

### Paint / IPC conclusion

The window stays visible, unique peers still connect, Safari still skips `set_instance` and `send_message_with_reply`, and helpers now listen for `XPC_TYPE_CONNECTION` accepts in `dispatch_main`. Screenshots: `/workspace/net27-example.png` and `/workspace/net27-baidu.png` (chrome painted, content white). InitializeWebProcess / CreateWebPage still did **not** arrive as real IPC: 0 incoming accepts, 0 `xpc event begin`, 0 `InitializeWebKit2 call`, no WebContent CAContext, content still white. The remaining gap is the pipe/endpoint channel itself — `xpc_endpoint_create` + `xpc_pipe_simpleroutine` + `create_from_endpoint` still do not deliver events onto the helper connection (neither accepts nor dictionaries). Next work should keep the pool, the kept-alive recv, the `dispatch_main` path, and the `set_instance` skip, and should inspect or fix that endpoint port handoff so a dictionary event / InitializeWebProcess can arrive — without checking in the public macosbooter name, adding a competing Mach listener on that name, retrying CAContext ports, or stalling Safari's launch thread.

## 45. Endpoint handoff works unsandboxed, but the net28 listener-queue / 4sim-port variant missed the Safari window (2026-11-10 23:00–23:03 CST / 07:00–07:03 PT)

### Change

A same-chroot cross-process probe (`/tmp/net28_epfix.c`, live net27 libmachook inserted by `run_bash`, child *not* classified as a WebKit helper) showed that the pipe/endpoint channel itself is not broken: both the original `xpc_connection_create_from_endpoint` connection and an endpoint rebuilt with `xpc_endpoint_create_mach_port_4sim` from `xpc_dictionary_copy_mach_send` delivered `XPC_TYPE_CONNECTION` accepts, dictionaries, and replies (`orig_reply=1 rebuilt_reply=1`, child `got_peer=2 got_msg=2`). `xpc_endpoint_copy_listener_port_4sim` exists at `0x19ce8cca8` and returns typ=0x30000 (RECV+SEND) in the child; the parent copy is typ=0x10000 (SEND only). `xpc_endpoint_create_bs_from_port` is NULL. The probe listener used a live background serial queue and never called `dispatch_main`. Production unique helpers, by contrast, created the listener on `dispatch_get_main_queue()`, went through the helper wrap that forces unique peers onto main, and only then entered `dispatch_main()`, so Safari could connect and `send_message_with_reply` before the main queue was draining.

net28 therefore keeps the non-blocking pre-spawn pool, the kept-alive receive right, register + post-exec look_up, TSV-hit reordering, the `dispatch_main` handoff, and the shared-cache `set_instance` skip. It creates the anonymous listener on a dedicated background serial queue (`com.macwsguide.wk.peer-listener`) and arms the accept handler with `macws_xpc_connection_set_event_handler_raw` so the wrap does not pin the listener itself to main. Accepted connections are still remembered and handed to `macws_webkit_xpc_main_connection` (targeted at main). Off-main unique-peer wrap events use `dispatch_async` rather than `dispatch_sync` to avoid a deadlock if an accept arrives before `dispatch_main`. The helper also COPY-sends the listener port with `xpc_dictionary_set_mach_send` after `xpc_endpoint_copy_listener_port_4sim`; the parent prefers a 4sim-rebuilt endpoint when that send right is valid and falls back to the received endpoint object. Metal_hooks logs unique-peer `send_message_with_reply` reply types (`wk-peer reply type=`). No public macosbooter check-in, no named Mach LISTENER on that public name, no constructor `dispatch_after`, no new exported symbols (168 T per slice). Proxies and the net19 path layout stayed on net20 / net19. 13 contract tests OK on the Intel Mac (8 peer-spawn + 5 environment).

### Deploy

Backup tag `pre-net28-20261110-230040` (23:00:40 CST / 07:00:40 PT) saved the live net27 libmachook (arm64e `563a6880de0f838f0b8b9cd03a95998dd3def986`, arm64 `be1cee0bd68b97e66be37725a9d964ee70204246`) and net20 proxies (WebContent `2097451dd27a6c353c69c88c6ccb5c62acb14e5b`). The net19 path layout was not touched. Only libmachook was replaced (Theos `FINALPACKAGE=1 STRIP=0 OPTFLAG=-O2`, `misc/set_macos_version.py` LC_BUILD_VERSION macOS 13.0, host two-pass `ldid -S` + `codesign`, device two-pass `ldid -S$ENT -M`, `jbctl trustcache add`, rootfs byte-copy not re-signed). Live net28 CDHashes (while deployed): arm64e `fa00f6a0cdbe7b32ff9f82b731cd1351e19967b1`, arm64 `1de3c8969c8259747c82d862ead7a07149a2204a`. Smoke at 23:00:53 CST / 07:00:53 PT: `SMOKE_OK` / `RUN_BASH_CANARY_OK` rc=0, IconServices 1724 / 53067 / 53071 loaded, WindowServer 51054 `Ss`, macwshostd 416, SpringBoard 391. `SMOKE_PASS`. No reboot, no 停止 macOS.

### Safari run (one guarded attempt)

23:01:01 CST / 07:01:01 PT onward. The launcher reported `ok=no launched-pid=0 message=custom-path 已启动，但 30 秒内没有发布可捕获的 AppKit 窗口`. After that 30 s Safari (pid 72943, which had already pre-spawned the unique-peer pool and received six endpoints) was *gone* from the process list; only unique helpers remained (9 WebContent + Networking + GPU). The script did not open example.com or baidu.com (`LAUNCH_WINDOW_FAIL; not opening URLs`) and wrote `DONE_NO_WINDOW`. Cleanup `CORE_OK`, WebKit LaunchDaemons unloaded, IconServices left loaded. No `/workspace/net28-example.png` or `/workspace/net28-baidu.png`.

Because Safari lost a visible/capturable window, net28 was **rolled back immediately** to `pre-net28-20261110-230040` (the net27 libmachook that was live before this deploy). No second variant. The first post-rollback `run_bash` smoke at 23:02:01 CST / 07:02:01 PT was `Killed: 9` (`SMOKE_FAIL rc=137`): `ldid -h` on the restored *rootfs* vnode SIGKILL'd even though its sha1 matched the jb copy (`563a6880…`), while `ldid -h` on `/var/jb/usr/macOS/lib/libmachook.dylib` succeeded. Re-adding the net27 CDHashes (arm64e `563a6880…` / sha1 `a70bf979…`, arm64 `be1cee0b…` / sha1 `1b6380bb…`) to the trustcache did not clear the rootfs SIGKILL. A fresh byte-copy of the already-signed jb net27 dylibs onto `/var/mnt/rootfs/usr/local/lib/` (new inodes 23586026 / 23586027, not re-signed) fixed it. Post-rollback smoke at 23:03:15 CST / 07:03:15 PT: `SMOKE_PASS`, libmachook arm64e again `563a6880de0f838f0b8b9cd03a95998dd3def986`, arm64 `be1cee0bd68b97e66be37725a9d964ee70204246`, WebContentProxy still `2097451dd27a6c353c69c88c6ccb5c62acb14e5b`, IconServices 1724 / 53067 / 53071, WindowServer 51054 `Ss`. Future rollbacks that leave `ldid -h` of the rootfs dylib SIGKILL'd should recopy the signed jb file rather than reboot.

### Field result

The 4sim port transfer worked in production. Six helpers logged `wk-peer endpoint-port` with typ=0x30000 and a non-NULL `copy_fn`; Safari logged six `wk-peer recv-endpoint typ=endpoint port_typ=0x10000 rebuilt=0x…` and six `wk-peer ready`. 12 `register kr=0` + 12 `spawn rc=0`, 11 helpers armed the raw accept handler (`listening for incoming connection`) and entered `dispatch_main`, 0 `xpc_error`, 0 endpoint-timeout, 0 pop-timeout, 0 fallback, no public macosbooter check-in. Handshake of the endpoint object + explicit mach send right is therefore not the missing piece for *delivery of the endpoint itself*.

What did not happen: 0 `wk-peer connected` (Safari never logged a successful `create_from_endpoint` pop), 0 `skip-set-instance`, 0 `send-message-with-reply`, 0 `wk-peer reply`, 0 `unique peer incoming connection=`, 0 `wk-peer xpc-event`, 0 `xpc event begin`, 0 `InitializeWebKit2 call` / `runInitializationCode call` (the 24 init-log hits are hook-install lines, not process IPC), 0 InitializeWebProcess / CreateWebPage, and the only `remoteContextWithOptions` is Safari chrome pid 72943 with `portNumber=0`. Logs were truncated mid-handshake (six ready, later helpers had resumed the listener but not yet `endpoint-port` / `ready`) when the 30 s launch-path timeout fired and Safari was already gone.

A plausible reason Safari disappeared, and a warning for the next attempt: the parent prefers the 4sim-rebuilt endpoint and then `mach_port_deallocate`s the copied send right. If `xpc_endpoint_create_mach_port_4sim` wraps that right without consuming/retaining it, the stored endpoint can hold a dead name, and the first later `create_from_endpoint` can crash or hang Safari on the launch thread (no `wk-peer connected` line would be printed). The unsandboxed probe sent traffic immediately and did not keep the rebuilt endpoint alive across a 30 s AppKit publish. Next work should not assume a rebuilt endpoint is safe after deallocating the send right, should not stall the launch thread, and must not ship a second variant without a window-capable rollback target (net27 is that target again).

### What was not done

No second variant. Device is back on net27 libmachook (arm64e `563a6880…`) with net20 proxies and the net19 path layout. Rollback copies remain at `pre-net28-20261110-230040` (net27 `563a6880…` + proxies `2097451d…`), `pre-net27-20261110-224355` (net26 `0551602b…` + proxies `2097451d…`), `pre-net26-20261110-223631`, `pre-net25-20261110-222759`, `pre-net24-20261110-221300`, `pre-net23-20261110-215737`, `pre-net22-20261110-214530`, `pre-net21-20261110-212258`, `pre-net20-20261110-211200`, `pre-net19-20261110-205054` (path layout), `pre-net18-20261110-203748`, and `pre-net17-20261110-202135`. No SkyLight hooks, no extra Safari CAContext IMPs, no constructor `dispatch_after`, no new remapper export, no public macosbooter check-in, no competing Mach listener on that name, no CAContext port-retry, no macwshostd restart, no reboot, no 停止 macOS. IconServices jobs were left loaded.

### Paint / IPC conclusion

net28 missed a capturable Safari window and was rolled back immediately; there are no net28 screenshots. InitializeWebProcess / CreateWebPage did **not** arrive as real IPC (the run never reached a connected unique peer or a `send_message_with_reply`). The in-chroot probe still stands: the pipe/endpoint channel *can* deliver accepts, dictionaries, and replies cross-process when the child is not a WebKit helper and the listener lives on a drained background queue. The production failure to get those events, and the window loss under the 4sim-rebuilt-endpoint preference plus send-right deallocate, remain the next gap. Keep the pool, the kept-alive recv, register + look_up, the `dispatch_main` path, and the `set_instance` skip; do not check in the public macosbooter name, add a competing Mach listener on that name, retry CAContext ports, or stall Safari's launch thread. Live device is net27 (`563a6880…` / net20 proxies / net19 layout), window-capable as of the unused `pre-net27-20261110-224355` / net27 rollback target going into this turn.

## 46. Keeping the 4sim send right lets Safari pop and stay visible; dictionaries still do not arrive (2026-11-10 23:07–23:09 CST / 07:07–07:09 PT)

### Change

net28 preferred a 4sim-rebuilt endpoint (`xpc_endpoint_create_mach_port_4sim` from the copied mach send right) and then `mach_port_deallocate`d that right. Safari vanished before `create_from_endpoint` (`LAUNCH_WINDOW_FAIL`, `launched-pid=0`). The in-chroot probe had already shown the pipe/endpoint channel can deliver accepts and replies when the send right stays valid.

net29 is that one fix on top of the net28 listener-queue / raw-setter / 4sim-port path: when a rebuilt endpoint is preferred, the parent stores the send right on the pending slot (`ep_port`) and does not deallocate it, including after the slot is taken for `create_from_endpoint`. A port that was not handed to 4sim is still dropped. The pool, kept-alive recv, register + post-exec look_up, TSV-hit reordering, `dispatch_main` handoff, and shared-cache `set_instance` skip are unchanged. No public macosbooter check-in, no named Mach LISTENER on that public name, no constructor `dispatch_after`, no new exported symbols (168 T per slice). Proxies and the net19 path layout stayed on net20 / net19. 13 contract tests OK on the Intel Mac (8 peer-spawn + 5 environment).

### Deploy

Backup tag `pre-net29-20261110-230747` (23:07:47 CST / 07:07:47 PT) saved the live net27 libmachook (arm64e `563a6880de0f838f0b8b9cd03a95998dd3def986`, arm64 `be1cee0bd68b97e66be37725a9d964ee70204246`) and net20 proxies (WebContent `2097451dd27a6c353c69c88c6ccb5c62acb14e5b`). The net19 path layout was not touched. Only libmachook was replaced (Theos `FINALPACKAGE=1 STRIP=0 OPTFLAG=-O2`, `misc/set_macos_version.py` LC_BUILD_VERSION macOS 13.0, host two-pass `ldid -S` + `codesign`, device two-pass `ldid -S$ENT -M`, `jbctl trustcache add`, rootfs byte-copy not re-signed). Live net29 CDHashes: arm64e `5e2aebc4d90df33baaaf0682943f108cd2d50eb3`, arm64 `0d8d9a0bc05d64c250e107910709a933cb717e61`. Smoke at 23:07:54 CST / 07:07:54 PT: `SMOKE_OK` / `RUN_BASH_CANARY_OK` rc=0, IconServices 1724 / 53067 / 53071 loaded, WindowServer 51054 `Ss`, macwshostd 416, SpringBoard 391. `SMOKE_PASS`. No reboot, no 停止 macOS.

### Safari run (one guarded attempt)

23:08:00 CST / 07:08:00 PT onward, Safari pid 73658. `ok=yes launched-pid=73658` and `LAUNCH_WINDOW_OK`. example.com and baidu.com were opened. RSS 760336 KB after example.com and 760592 KB after baidu.com (~743 MB). Screenshots `/workspace/net29-example.png` (menu-bar clock 07:08 PT / 23:08 CST) and `/workspace/net29-baidu.png` (07:09 PT / 23:09 CST): Safari chrome and tabs painted (example.com / baidu.com in the address field), content area white. Cleanup `CORE_OK`, WebKit LaunchDaemons unloaded, IconServices left loaded.

Because the window stayed capturable, net29 was left deployed. Rollback copies at `pre-net29-20261110-230747` (net27 `563a6880…` + proxies `2097451d…`) were not used. The net29 rollback script also recopies the signed jb dylib onto a fresh rootfs vnode (not re-signed) so a future rollback should not repeat the net28 post-rollback `ldid -h` SIGKILL.

### Field result

The send right stayed alive: 31 `wk-peer recv-endpoint typ=endpoint port_typ=0x10000 rebuilt=… kept_port=1` and 0 `kept_port=0`. Safari popped and `create_from_endpoint` succeeded: 32 register/spawn, 31 ready, 30 `wk-peer connected` (29 WebContent + Networking), 30 `skip-set-instance`, 30 `send-message-with-reply`, 0 pop-timeout, 0 endpoint-timeout, 0 `xpc_error`, no public macosbooter check-in. 31 helpers armed the raw accept handler (`listening for incoming connection`) and entered `dispatch_main`. GPU was ready but not popped. SandboxBroker hit grace-timeout / pop-miss. 18 fallbacks (15 WebContent + 3 SandboxBroker).

Authoritative IPC is still zero. 0 `unique peer incoming connection=` (the 31 init-log “incoming connection” hits are the `listening for incoming connection` arming line), 0 `wk-peer xpc-event`, 0 `xpc event begin`, 0 `InitializeWebKit2 call`, 0 `runInitializationCode call`, 0 InitializeWebProcess / CreateWebPage, 0 `wk-peer reply`. No WebContent `remoteContextWithOptions` (five remoteContext lines are Safari chrome, AppKit open/save, QuickLook, and two screencapture, all `portNumber=0`). Keeping the send right fixed the net28 crash and restored a visible window plus a live `create_from_endpoint` + `send_message_with_reply` path, but those dictionaries still never arrive as events on the helper.

### What was not done

No second variant. net29 libmachook was left deployed (arm64e `5e2aebc4…`) with net20 proxies and the net19 path layout. Rollback copies remain at `pre-net29-20261110-230747` (net27 `563a6880…` + proxies `2097451d…`), `pre-net28-20261110-230040` (also net27 `563a6880…`), `pre-net27-20261110-224355` (net26 `0551602b…`), `pre-net26-20261110-223631`, `pre-net25-20261110-222759`, `pre-net24-20261110-221300`, `pre-net23-20261110-215737`, `pre-net22-20261110-214530`, `pre-net21-20261110-212258`, `pre-net20-20261110-211200`, `pre-net19-20261110-205054` (path layout), `pre-net18-20261110-203748`, and `pre-net17-20261110-202135`. No SkyLight hooks, no extra Safari CAContext IMPs, no constructor `dispatch_after`, no new remapper export, no public macosbooter check-in, no competing Mach listener on that name, no CAContext port-retry, no macwshostd restart, no reboot, no 停止 macOS. IconServices jobs were left loaded.

### Paint / IPC conclusion

The window is visible again and the net28 diagnosis holds: keeping the 4sim send right lets Safari pop `create_from_endpoint` and stay alive. Screenshots: `/workspace/net29-example.png` and `/workspace/net29-baidu.png` (chrome painted, content white). InitializeWebProcess / CreateWebPage still did **not** arrive as real IPC: 0 incoming accepts, 0 `xpc event begin`, 0 `InitializeWebKit2 call`, 0 replies, no WebContent CAContext, content still white. The remaining gap is delivery of `send_message_with_reply` dictionaries onto the already-resumed unique helper (listener on the background serial queue, endpoint + kept send right, `dispatch_main` draining). Next work should keep the pool, the kept-alive recv and send right, register + look_up, the `dispatch_main` path, and the `set_instance` skip, and should inspect why those 30 with-reply sends never surface as helper events — without checking in the public macosbooter name, adding a competing Mach listener on that name, retrying CAContext ports, or stalling Safari's launch thread.

## 47. Authentic endpoint still delivers no helper events (2026-11-10 23:14–23:16 CST / 07:14–07:16 PT)

### Change

net29 always preferred a 4sim-rebuilt endpoint whenever the copied send right was valid. Safari popped `create_from_endpoint`, skipped `set_instance`, and sent 30 `send_message_with_reply`, but helpers saw 0 accepts, 0 xpc events, and 0 replies. An unsandboxed same-chroot probe had already shown that the original `xpc_endpoint_create` object *and* a 4sim-rebuilt endpoint can both deliver CONNECTION + dictionaries (`got_peer=2 got_msg=2`). The rebuilt object can also yield a live-looking `create_from_endpoint` connection that does not actually attach to the helper listener.

net30 therefore prefers the authentic `xpc_endpoint_create` object from the pipe message (`src=orig`), keeps the 4sim send right alive either way (`kept_port=1`), and uses the rebuilt endpoint only if the original is missing. The pool, kept-alive recv, register + post-exec look_up, TSV-hit reordering, background listener queue + raw accept handler, `dispatch_main` handoff, and shared-cache `set_instance` skip are unchanged. No public macosbooter check-in, no named Mach LISTENER on that public name, no constructor `dispatch_after`, no new exported symbols (168 T per slice). Proxies and the net19 path layout stayed on net20 / net19. 13 contract tests OK on the Intel Mac (8 peer-spawn + 5 environment).

### Deploy

Backup tag `pre-net30-20261110-231440` (23:14:40 CST / 07:14:40 PT) saved the live net29 libmachook (arm64e `5e2aebc4d90df33baaaf0682943f108cd2d50eb3`, arm64 `0d8d9a0bc05d64c250e107910709a933cb717e61`) and net20 proxies (WebContent `2097451dd27a6c353c69c88c6ccb5c62acb14e5b`). The net19 path layout was not touched. Only libmachook was replaced (Theos `FINALPACKAGE=1 STRIP=0 OPTFLAG=-O2`, `misc/set_macos_version.py` LC_BUILD_VERSION macOS 13.0, host two-pass `ldid -S` + `codesign`, device two-pass `ldid -S$ENT -M`, `jbctl trustcache add`, rootfs byte-copy not re-signed). Live net30 CDHashes: arm64e `333ef6680c44371b85d5391b2b0a92eb0d6c72bd`, arm64 `3914ebae652e84479fb13037c74d6eb6a31e488a`. Smoke at 23:15:16 CST / 07:15:16 PT: `SMOKE_OK` / `RUN_BASH_CANARY_OK` rc=0, IconServices 1724 / 53067 / 53071 loaded, WindowServer 51054 `Ss`/`Rs`, macwshostd 416, SpringBoard 391. `SMOKE_PASS`. No reboot, no 停止 macOS.

### Safari run (one guarded attempt)

23:15:25 CST / 07:15:25 PT onward, Safari pid 74935. `LAUNCH_WINDOW_OK`. example.com and baidu.com were opened. RSS 784512 KB after example.com and 784880 KB after baidu.com (~766 MB). Screenshots `/workspace/net30-example.png` and `/workspace/net30-baidu.png` (menu-bar clock 07:16 PT / 23:16 CST on both): Safari chrome and tabs painted (example.com / baidu.com in the address field), content area white. Cleanup `CORE_OK`, WebKit LaunchDaemons unloaded, IconServices left loaded.

Because the window stayed capturable, net30 was left deployed. Rollback copies at `pre-net30-20261110-231440` (net29 `5e2aebc4…`) and `pre-net29-20261110-230747` (net27 `563a6880…`) were not used. The rollback script still recopies the signed jb dylib onto a fresh rootfs vnode (not re-signed).

### Field result

Safari used the authentic endpoint: 31 `wk-peer recv-endpoint typ=endpoint … rebuilt_used=0 src=orig kept_port=1`, 0 `src=rebuilt`. Handshake stayed good: 32 register/spawn, 31 ready, 30 `wk-peer connected` (29 WebContent + Networking), 30 `skip-set-instance`, 30 `send-message-with-reply`, 0 pop-timeout, 0 endpoint-timeout, 0 `xpc_error`, no public macosbooter check-in. 31 helpers armed the raw accept handler and entered `dispatch_main`. GPU was ready but not popped. SandboxBroker hit grace-timeout / pop-miss. 20 fallbacks (17 WebContent + 3 SandboxBroker).

Authoritative IPC is still zero, now on the original endpoint with a visible window: 0 `unique peer incoming connection=` (the 31 init-log “incoming connection” hits are the arming line), 0 `wk-peer xpc-event`, 0 `xpc event begin`, 0 `InitializeWebKit2 call`, 0 `runInitializationCode call`, 0 InitializeWebProcess / CreateWebPage, 0 `wk-peer reply`. No WebContent `remoteContextWithOptions` (five remoteContext lines are Safari chrome, AppKit open/save, QuickLook, and two screencapture, all `portNumber=0`).

This rules out “Safari was talking on a 4sim-rebuilt endpoint that was not attached to the helper listener” as the *sole* cause. The same zero-event outcome now holds for the authentic `xpc_endpoint_create` object, with the send right kept, the listener on the background serial queue, the raw accept handler, and `dispatch_main` draining — the combination that had not been field-tested with a live window in net28/net29. The unsandboxed in-chroot probe still delivers; production unique WebContent (stock image after the chroot proxy exec, helper wrap / sandbox environment) still does not.

### What was not done

No second variant. net30 libmachook was left deployed (arm64e `333ef668…`) with net20 proxies and the net19 path layout. Rollback copies remain at `pre-net30-20261110-231440` (net29 `5e2aebc4…` + proxies `2097451d…`), `pre-net29-20261110-230747` (net27 `563a6880…`), `pre-net28-20261110-230040` (also net27), `pre-net27-20261110-224355` (net26 `0551602b…`), `pre-net26-20261110-223631`, `pre-net25-20261110-222759`, `pre-net24-20261110-221300`, `pre-net23-20261110-215737`, `pre-net22-20261110-214530`, `pre-net21-20261110-212258`, `pre-net20-20261110-211200`, `pre-net19-20261110-205054` (path layout), `pre-net18-20261110-203748`, and `pre-net17-20261110-202135`. No SkyLight hooks, no extra Safari CAContext IMPs, no constructor `dispatch_after`, no new remapper export, no public macosbooter check-in, no competing Mach listener on that name, no CAContext port-retry, no macwshostd restart, no reboot, no 停止 macOS. IconServices jobs were left loaded.

### Paint / IPC conclusion

The window is visible and Safari is talking on the authentic endpoint, but InitializeWebProcess / CreateWebPage still did **not** arrive as real IPC. Screenshots: `/workspace/net30-example.png` and `/workspace/net30-baidu.png` (chrome painted, content white). 0 incoming accepts, 0 `xpc event begin`, 0 `InitializeWebKit2 call`, 0 replies, no WebContent CAContext, content still white. Preferring `src=orig` did not fix delivery. The remaining gap is why `send_message_with_reply` on a live `create_from_endpoint` connection never surfaces as a helper event in the production unique WebContent environment (the unsandboxed probe still works). Next work should keep the pool, the kept-alive recv and send right, register + look_up, the authentic endpoint, the `dispatch_main` path, and the `set_instance` skip, and should inspect that helper-side receive path — without checking in the public macosbooter name, adding a competing Mach listener on that name, retrying CAContext ports, or stalling Safari's launch thread.

## 48. Unique peers exec with XPC_FLAGS=0, still no helper events (2026-11-10 23:26–23:30 CST / 07:26–07:30 PT)

### Change

net30 preferred the authentic `xpc_endpoint_create` object (`src=orig`, send right kept) and still saw 0 helper events. The working unsandboxed in-chroot probe delivers CONNECTION + dictionaries on the same pipe/endpoint channel, but that probe is a bare C child: no 0755 proxy / 4755 `*.priv` exec into the stock WebKit image, no Application-type `XPC_FLAGS=8019`, and not a `macws_is_webkit_helper_process()`. Production unique children after the chroot proxy had `recv_rights=0` (slot 1 = rendezvous SEND, slot 2 empty) yet were still launched with `XPC_FLAGS=8019` — the proxy reconstituted zero incoming flags, `macws_wk_peer_envp` set 8019, and `macws_webkit_reapply_packed_xpc_flags` ran before the unique listener path. That is a concrete difference from the probe: libxpc was told the process was a managed Application-type service without a `SERVICE_SLOT` receive right to consume.

net31 therefore stops advertising `XPC_FLAGS=8019` on the unique-peer path only. The 0755 WebKit proxy, when `MACWS_WK_PEER` contains `com.macwsguide.wk.i.`, execs the stock image with `XPC_FLAGS=0` instead of reconstituting 8019 (`unique_peer=1` in the proxy log). Fallback helpers (no unique peer) still get 8019. Safari's unique `posix_spawn` envp now sets `XPC_FLAGS=0`. Hooked `xpc_main` skips `macws_webkit_reapply_packed_xpc_flags` when the peer name is valid, and would `setenv("XPC_FLAGS","0")` if a non-zero value were still present. The pool, kept-alive recv, register + post-exec look_up, authentic endpoint (`src=orig`) with kept send right, background listener queue + raw accept handler, `dispatch_main` handoff, and shared-cache `set_instance` skip are unchanged. No public macosbooter check-in, no named Mach LISTENER on that public name, no constructor `dispatch_after`, no new exported symbols (168 T per slice). The net19 path layout was not touched. 14 contract tests OK on the Intel Mac (9 peer-spawn + 5 environment), including unique-peer skip of 8019 in both the proxy unit test and the hook contract.

### Deploy

Backup tag `pre-net31-20261110-232604` (23:26:04 CST / 07:26:04 PT) saved the live net30 libmachook (arm64e `333ef6680c44371b85d5391b2b0a92eb0d6c72bd`, arm64 `3914ebae652e84479fb13037c74d6eb6a31e488a`) and net20 proxies (WebContent `2097451dd27a6c353c69c88c6ccb5c62acb14e5b`). The net19 path layout was not touched. libmachook and the four WebKit proxies were replaced (Theos `FINALPACKAGE=1 STRIP=0 OPTFLAG=-O2`, `misc/set_macos_version.py` LC_BUILD_VERSION macOS 13.0 on libmachook only, host two-pass `ldid -S` + `codesign`, device two-pass `ldid -S$ENT -M`, `jbctl trustcache add`, rootfs / macws-xpc byte-copy not re-signed; 0755 stub + same-bytes 4755 `*.priv`). Live net31 CDHashes: libmachook arm64e `06da6f2348ac6c4726453d5cf172f4920cfe021d`, arm64 `3f35f3ed753b63059e91cd6ee38a18c20fd14bb7`; WebContentProxy arm64e `124857f954b1d5174741de2e71bb1b078cda15ec` (Networking `c20210305138bb22167c94d8262c54aece8729cc`, GPU `c6a2886f1c3f3eaf755c72aeda712ec9bf20a140`, SandboxBroker `98f5a92c26f7e91a6b0914948d85b4ffec34ee1f`). Smoke at 23:26:12 CST / 07:26:12 PT: `SMOKE_OK` / `RUN_BASH_CANARY_OK` rc=0, IconServices 1724 / 53067 / 53071 loaded, WindowServer 51054 `Ss`, macwshostd 416, SpringBoard 391. `SMOKE_PASS`. No reboot, no 停止 macOS.

### Safari run (one guarded attempt)

23:28:19 CST / 07:28:19 PT onward, Safari pid 76523. `LAUNCH_WINDOW_OK`. example.com and baidu.com were opened. RSS 814272 KB after example.com and 814112 KB after baidu.com (~795 MB). Screenshots `/workspace/net31-example.png` and `/workspace/net31-baidu.png` (menu-bar clock 07:29 PT / 23:29 CST on both): Safari chrome and tabs painted (example.com / baidu.com in the address field), content area white. Cleanup `CORE_OK`, WebKit LaunchDaemons unloaded, IconServices left loaded.

Because the window stayed capturable, net31 was left deployed. Rollback copies at `pre-net31-20261110-232604` (net30 `333ef668…` + net20 proxies `2097451d…`) and `pre-net30-20261110-231440` (net29 `5e2aebc4…`) were not used. The rollback script still recopies the signed jb dylib onto a fresh rootfs vnode (not re-signed).

### Field result

The unique-peer exec environment changed as intended. The proxy logged 32 `unique_peer=1 outgoing=0` (incoming still `0x0`) and 20 fallback `unique_peer=0 outgoing=8019`. All 31 unique helpers reached hooked `xpc_main` with `flags=0x0` (0 `unique peer cleared XPC_FLAGS`, because the proxy already supplied 0). Fallback helpers still took stock `xpc_main` with `flags=8019` / `peer=(null)` (19 `reapplied XPC_FLAGS`). Handshake otherwise matched net30: 32 register/spawn, 31 `wk-peer recv-endpoint typ=endpoint … rebuilt_used=0 src=orig kept_port=1`, 31 ready, 30 `wk-peer connected` (29 WebContent + Networking), 30 `skip-set-instance`, 30 `send-message-with-reply`, 0 `src=rebuilt`, 0 endpoint-timeout, 0 `xpc_error`, no public macosbooter check-in. 31 helpers armed the raw accept handler and entered `dispatch_main`. GPU was ready but not popped. One unique WebContent hit pop-timeout (recv kept until grace). SandboxBroker hit grace-timeout / pop-miss. 22 fallbacks (19 WebContent + 3 SandboxBroker).

Authoritative IPC is still zero, now with unique children running `XPC_FLAGS=0x0` after the proxy exec: 0 `unique peer incoming connection=` (the 31 init-log “incoming connection” hits are the arming line), 0 `wk-peer xpc-event`, 0 `xpc event begin`, 0 `InitializeWebKit2 call`, 0 `runInitializationCode call`, 0 InitializeWebProcess / CreateWebPage, 0 `wk-peer reply`. No WebContent `remoteContextWithOptions` (five remoteContext lines are Safari chrome, AppKit open/save, QuickLook, and two screencapture, all `portNumber=0`).

This rules out “unique WebContent fails only because the proxy / reapply path advertised Application-type `XPC_FLAGS=8019` with no inherited receive right” as the sole cause. The same zero-event outcome now holds for `src=orig`, a kept send right, the background listener queue, the raw accept handler, `dispatch_main`, and `flags=0x0` — the flags the unsandboxed probe also ran without. Remaining production-vs-probe differences after the 0755 proxy exec into the stock image include helper classification / the helper `set_event_handler` wrap, `dispatch_main` vs the probe’s live `usleep` main thread, the WebContent seatbelt / first-image, and `XPC_SERVICE_NAME` of the unique peer.

### What was not done

No second variant. net31 libmachook and the unique-peer `XPC_FLAGS=0` proxies were left deployed (arm64e `06da6f23…`, WebContentProxy `124857f9…`) with the net19 path layout. Rollback copies remain at `pre-net31-20261110-232604` (net30 `333ef668…` + net20 proxies `2097451d…`), `pre-net30-20261110-231440` (net29 `5e2aebc4…`), `pre-net29-20261110-230747` (net27 `563a6880…`), `pre-net28-20261110-230040` (also net27), `pre-net27-20261110-224355` (net26 `0551602b…`), `pre-net26-20261110-223631`, `pre-net25-20261110-222759`, `pre-net24-20261110-221300`, `pre-net23-20261110-215737`, `pre-net22-20261110-214530`, `pre-net21-20261110-212258`, `pre-net20-20261110-211200`, `pre-net19-20261110-205054` (path layout), `pre-net18-20261110-203748`, and `pre-net17-20261110-202135`. No SkyLight hooks, no extra Safari CAContext IMPs, no constructor `dispatch_after`, no new remapper export, no public macosbooter check-in, no competing Mach listener on that name, no CAContext port-retry, no macwshostd restart, no reboot, no 停止 macOS. IconServices jobs were left loaded.

### Paint / IPC conclusion

The window is visible and unique helpers now start with `XPC_FLAGS=0x0` after the chroot proxy exec, but InitializeWebProcess / CreateWebPage still did **not** arrive as real IPC. Screenshots: `/workspace/net31-example.png` and `/workspace/net31-baidu.png` (chrome painted, content white). 0 incoming accepts, 0 `xpc event begin`, 0 `InitializeWebKit2 call`, 0 replies, no WebContent CAContext, content still white. Skipping managed `XPC_FLAGS=8019` on the unique path did not fix delivery. The remaining gap is why `send_message_with_reply` on a live `create_from_endpoint` connection never surfaces as a helper event in the production unique WebContent environment after the 0755 proxy exec into the stock image (the unsandboxed probe still works; `XPC_FLAGS=8019` is no longer the distinguishing factor). Next work should keep the pool, the kept-alive recv and send right, register + look_up, the authentic endpoint, the `dispatch_main` path, the `set_instance` skip, and the unique-peer `XPC_FLAGS=0` exec, and should inspect the remaining production-vs-probe differences (helper wrap, sandbox / first-image, `xpc_main` vs the probe listener) — without checking in the public macosbooter name, adding a competing Mach listener on that name, retrying CAContext ports, or stalling Safari's launch thread.

## 49. Unique peers omit XPC_SERVICE_NAME, still no helper events (2026-11-10 23:36–23:38 CST / 07:36–07:38 PT)

### Change

net31 exec'd unique children with `XPC_FLAGS=0x0` after the 0755 proxy, matching the unsandboxed probe's flags class, but still saw 0 helper events. Remaining production-vs-probe gaps were the helper `set_event_handler` wrap / classification, `dispatch_main` vs the probe's `usleep` main thread, WebContent seatbelt / first-image, and `XPC_SERVICE_NAME` of the unique peer. `secinit` and `sandbox_init*` are already stubbed in libmachook, so a seatbelt profile is not applied at helper start (the unique path is already unsandboxed in that sense). `dispatch_main` is kept. Unique spawn still set `XPC_SERVICE_NAME=com.macwsguide.wk.i.<32 hex>`. The probe had no service name; `xpc_connection_create(NULL)` in a process whose `XPC_SERVICE_NAME` is set can bind as a named client instead of an anonymous listener, which would explain accepts never arriving.

net32 therefore omits `XPC_SERVICE_NAME` from the unique `posix_spawn` envp (it is still stripped from the parent environ) and `unsetenv("XPC_SERVICE_NAME")` in hooked `xpc_main` before `create(NULL)`. Fallback helpers are unchanged. The pool, kept-alive recv, register + look_up, authentic endpoint (`src=orig`) with kept send right, unique-path `XPC_FLAGS=0`, background listener queue + raw accept handler, `dispatch_main` handoff, and shared-cache `set_instance` skip are unchanged. No public macosbooter check-in, no named Mach LISTENER on that public name, no constructor `dispatch_after`, no new exported symbols (168 T per slice). Proxies and the net19 path layout stayed on net31 / net19. 14 contract tests OK on the Intel Mac (9 peer-spawn + 5 environment).

### Deploy

Backup tag `pre-net32-20261110-233630` (23:36:30 CST / 07:36:30 PT) saved the live net31 libmachook (arm64e `06da6f2348ac6c4726453d5cf172f4920cfe021d`, arm64 `3f35f3ed753b63059e91cd6ee38a18c20fd14bb7`) and net31 proxies (WebContent `124857f954b1d5174741de2e71bb1b078cda15ec`). The net19 path layout was not touched. Only libmachook was replaced (Theos `FINALPACKAGE=1 STRIP=0 OPTFLAG=-O2`, `misc/set_macos_version.py` LC_BUILD_VERSION macOS 13.0, host two-pass `ldid -S` + `codesign`, device two-pass `ldid -S$ENT -M`, `jbctl trustcache add`, rootfs byte-copy not re-signed). Live net32 CDHashes: arm64e `358ba07cdba2869bddd5bcc8e5e8139fa95d9470`, arm64 `821b2a5b74f5c8090f1803f0bf2fcaac2ecf28f6`. WebContentProxy remained `124857f9…`. Smoke at 23:36:37 CST / 07:36:37 PT: `SMOKE_OK` / `RUN_BASH_CANARY_OK` rc=0, IconServices 1724 / 53067 / 53071 loaded, WindowServer 51054 `Ss`, macwshostd 416, SpringBoard 391. `SMOKE_PASS`. No reboot, no 停止 macOS.

### Safari run (one guarded attempt)

23:36:41 CST / 07:36:41 PT onward, Safari pid 77855. `LAUNCH_WINDOW_OK`. example.com and baidu.com were opened. RSS 843456 KB after example.com and 843712 KB after baidu.com (~824 MB). Screenshots `/workspace/net32-example.png` and `/workspace/net32-baidu.png` (menu-bar clock 07:37 PT / 23:37 CST on both): Safari chrome and tabs painted (example.com / baidu.com in the address field), content area white. Cleanup `CORE_OK`, WebKit LaunchDaemons unloaded, IconServices left loaded.

Because the window stayed capturable, net32 was left deployed. Rollback copies at `pre-net32-20261110-233630` (net31 `06da6f23…` + proxies `124857f9…`) and `pre-net31-20261110-232604` (net30 `333ef668…` + net20 proxies `2097451d…`) were not used. The rollback script still recopies the signed jb dylib onto a fresh rootfs vnode (not re-signed).

### Field result

The service-name change landed: 31 `unique peer omitted XPC_SERVICE_NAME`, and all 31 unique helpers reached hooked `xpc_main` with `service=(null) flags=0x0`. Handshake otherwise matched net30/net31: 32 register/spawn, 31 `wk-peer recv-endpoint typ=endpoint … rebuilt_used=0 src=orig kept_port=1`, 31 ready, 30 `wk-peer connected` (29 WebContent + Networking), 30 `skip-set-instance`, 30 `send-message-with-reply`, 0 `src=rebuilt`, 0 pop-timeout, 0 endpoint-timeout, 0 `xpc_error`, no public macosbooter check-in. 31 helpers armed the accept handler and entered `dispatch_main`. GPU was ready but not popped. SandboxBroker hit grace-timeout / pop-miss. 24 fallbacks (21 WebContent + 3 SandboxBroker).

Authoritative IPC is still zero, now with unique children running `flags=0x0` and `service=(null)` after the proxy exec: 0 `unique peer incoming connection=` (the 31 init-log “incoming connection” hits are the arming line), 0 `wk-peer xpc-event`, 0 `xpc event begin`, 0 `InitializeWebKit2 call`, 0 `runInitializationCode call`, 0 InitializeWebProcess / CreateWebPage, 0 `wk-peer reply`. No WebContent `remoteContextWithOptions` (five remoteContext lines are Safari chrome, AppKit open/save, QuickLook, and two screencapture, all `portNumber=0`).

This rules out “`xpc_connection_create(NULL)` was only binding as a named client of `XPC_SERVICE_NAME=com.macwsguide.wk.i.*`” as the sole cause. The same zero-event outcome now holds for `src=orig`, a kept send right, `XPC_FLAGS=0x0`, no unique `XPC_SERVICE_NAME`, the background listener queue, the intended raw accept handler, and `dispatch_main`.

A remaining production-vs-probe difference is still visible in the field log: all 31 unique listeners also logged `wk-peer set-event-handler … helper=1` on the listener connection itself, which is the helper wrap. That wrap forces the connection onto the main queue for every `set_event_handler` in a WebKit helper. The unsandboxed probe was not classified as `macws_is_webkit_helper_process()`, so it did not take that wrap; its listener stayed on the background serial queue and delivered CONNECTION + dictionaries. The unique path intends to use `macws_xpc_connection_set_event_handler_raw` to skip the wrap, but those 31 helper=1 lines show the wrap still ran on the listener. Seatbelt at process start is already a no-op (`_libsecinit_initializer` / `sandbox_init*` return 0).

### What was not done

No second variant. net32 libmachook was left deployed (arm64e `358ba07c…`) with net31 proxies (`124857f9…`) and the net19 path layout. Rollback copies remain at `pre-net32-20261110-233630` (net31 `06da6f23…` + proxies `124857f9…`), `pre-net31-20261110-232604` (net30 `333ef668…` + net20 proxies `2097451d…`), `pre-net30-20261110-231440` (net29 `5e2aebc4…`), `pre-net29-20261110-230747` (net27 `563a6880…`), `pre-net28-20261110-230040` (also net27), `pre-net27-20261110-224355` (net26 `0551602b…`), `pre-net26-20261110-223631`, `pre-net25-20261110-222759`, `pre-net24-20261110-221300`, `pre-net23-20261110-215737`, `pre-net22-20261110-214530`, `pre-net21-20261110-212258`, `pre-net20-20261110-211200`, `pre-net19-20261110-205054` (path layout), `pre-net18-20261110-203748`, and `pre-net17-20261110-202135`. No SkyLight hooks, no extra Safari CAContext IMPs, no constructor `dispatch_after`, no new remapper export, no public macosbooter check-in, no competing Mach listener on that name, no CAContext port-retry, no macwshostd restart, no reboot, no 停止 macOS. IconServices jobs were left loaded.

### Paint / IPC conclusion

The window is visible and unique helpers now start with `XPC_FLAGS=0x0` and `XPC_SERVICE_NAME` unset after the chroot proxy exec, but InitializeWebProcess / CreateWebPage still did **not** arrive as real IPC. Screenshots: `/workspace/net32-example.png` and `/workspace/net32-baidu.png` (chrome painted, content white). 0 incoming accepts, 0 `xpc event begin`, 0 `InitializeWebKit2 call`, 0 replies, no WebContent CAContext, content still white. Omitting the unique service name did not fix delivery. The remaining gap is why `send_message_with_reply` on a live `create_from_endpoint` connection never surfaces as a helper event in the production unique WebContent environment — in particular, the helper wrap still runs on the unique listener (`helper=1`, target queue forced to main), which the working unsandboxed probe never took. Next work should keep the pool, the kept-alive recv and send right, register + look_up, the authentic endpoint, unique-path `XPC_FLAGS=0`, the omitted unique `XPC_SERVICE_NAME`, the `dispatch_main` path, and the `set_instance` skip, and should make the unique listener actually bypass that helper wrap — without checking in the public macosbooter name, adding a competing Mach listener on that name, retrying CAContext ports, or stalling Safari's launch thread.

## 50. Unique listeners skip the helper wrap; incoming accepts arrive; still no dictionaries (2026-11-10 23:43–23:45 CST / 07:43–07:45 PT)

### Change

net32 kept the Safari window and landed unique helpers with `XPC_FLAGS=0x0` and `XPC_SERVICE_NAME` unset, but still saw 0 helper events. The remaining production-vs-probe gap visible in that field log was that all 31 unique listeners also logged `wk-peer set-event-handler … helper=1` on the listener itself. That is the helper wrap: it forces every helper `set_event_handler` onto the main queue. The unique path intended to use `macws_xpc_connection_set_event_handler_raw` to skip it, but a direct call to that asm-alias still hits DYLD_INTERPOSE (`*_early`) and the shared-cache MSHook, both of which enter `macws_xpc_wrap_app_event_handler`. The working unsandboxed in-chroot probe was not a WebKit helper, so it never took that wrap and delivered CONNECTION + dictionaries on a live background serial queue.

net33 therefore marks the unique-path anonymous listener (`macws_xpc_remember_unique_listener`) before the setter, and `macws_xpc_wrap_app_event_handler` skips the wrap body for those connections (call `orig` only, no `set_target_queue` to main, log `wk-peer set-event-handler-raw … helper=0 wrap=0`). Incoming accepted peers stay unmarked and still take the main-queue wrap, which InitializeWebKit2's isMainThread assertion still needs. The pool, kept-alive recv, register + look_up, authentic endpoint (`src=orig`) with kept send right, unique-path `XPC_FLAGS=0`, omitted unique `XPC_SERVICE_NAME`, background listener queue, `dispatch_main` handoff, and shared-cache `set_instance` skip are unchanged. No public macosbooter check-in, no named Mach LISTENER on that public name, no constructor `dispatch_after`, no new exported symbols (168 T per slice). Proxies and the net19 path layout stayed on net31 / net19. 14 contract tests OK on the Intel Mac (9 peer-spawn + 5 environment).

### Deploy

Backup tag `pre-net33-20261110-234320` (23:43:20 CST / 07:43:20 PT) saved the live net32 libmachook (arm64e `358ba07cdba2869bddd5bcc8e5e8139fa95d9470`, arm64 `821b2a5b74f5c8090f1803f0bf2fcaac2ecf28f6`) and net31 proxies (WebContent `124857f954b1d5174741de2e71bb1b078cda15ec`). The net19 path layout was not touched. Only libmachook was replaced (Theos `FINALPACKAGE=1 STRIP=0 OPTFLAG=-O2`, `misc/set_macos_version.py` LC_BUILD_VERSION macOS 13.0, host two-pass `ldid -S` + `codesign`, device two-pass `ldid -S$ENT -M`, `jbctl trustcache add`, rootfs byte-copy not re-signed). Live net33 CDHashes after device ldid: arm64e `662c8443cbab1d84b74b8ae3f812001150e1a0e7`, arm64 `5b6dc824ece5bfb29a46747cb753e11f1e5a8ca4`. WebContentProxy remained `124857f9…`. Smoke at 23:43:26 CST / 07:43:26 PT: `SMOKE_OK` / `RUN_BASH_CANARY_OK` rc=0, IconServices 1724 / 53067 / 53071 loaded, WindowServer 51054 `Ss`, macwshostd 416, SpringBoard 391. `SMOKE_PASS`. No reboot, no 停止 macOS.

### Safari run (one guarded attempt)

23:43:38 CST / 07:43:38 PT onward, Safari pid 79190. `LAUNCH_WINDOW_OK`. example.com and baidu.com were opened. RSS 865456 KB after example.com and 865728 KB after baidu.com (~845 MB). Screenshots `/workspace/net33-example.png` and `/workspace/net33-baidu.png` (menu-bar clock 07:44 PT / 23:44 CST on both): Safari chrome and tabs painted (example.com / baidu.com in the address field), content area white. Cleanup `CORE_OK`, WebKit LaunchDaemons unloaded, IconServices left loaded.

Because the window stayed capturable, net33 was left deployed. Rollback copies at `pre-net33-20261110-234320` (net32 `358ba07c…` + proxies `124857f9…`) and `pre-net32-20261110-233630` (net31 `06da6f23…` + proxies `124857f9…`) were not used. The rollback script still recopies the signed jb dylib onto a fresh rootfs vnode (not re-signed).

### Field result

The wrap skip landed on the unique listeners: 31 `wk-peer set-event-handler-raw … helper=0 wrap=0`, 31 distinct listener connection pointers, and 0 overlap with the helper=1 set. Incoming accepted peers still took the wrap (60 `wk-peer set-event-handler … helper=1` on 30 distinct incoming pointers — both DYLD_INTERPOSE and the shared-cache MSHook). Unique helpers still reached hooked `xpc_main` with `service=(null) flags=0x0` (31 omitted `XPC_SERVICE_NAME`). Handshake otherwise matched net32: 32 register/spawn, 31 `wk-peer recv-endpoint typ=endpoint … rebuilt_used=0 src=orig kept_port=1`, 31 ready, 30 `wk-peer connected` (29 WebContent + Networking), 30 `skip-set-instance`, 30 `send-message-with-reply`, 0 `src=rebuilt`, 0 pop-timeout, 0 endpoint-timeout, 0 `xpc_error`, no public macosbooter check-in (the macosbooter name hits in the Safari log are fallback / TSV-hit activate strings, not helper check-in). 31 helpers armed the accept handler and entered `dispatch_main`. GPU was ready but not popped. SandboxBroker hit grace-timeout / pop-miss. 26 fallbacks (23 WebContent + 3 SandboxBroker).

Unlike net32, the unique listeners now actually accept peers: 30 `unique peer incoming connection=` (real accepts, `pthread_main=0`, distinct from the 31 arming-line “listening for incoming connection” hits) and 30 `accepted connection=` hand-offs into `macws_webkit_xpc_main_connection`. That is the first time production unique WebContent has accepted the Safari `create_from_endpoint` peer.

Authoritative follow-up IPC is still zero: 0 `wk-peer xpc-event`, 0 `xpc event begin`, 0 `InitializeWebKit2 call`, 0 `runInitializationCode call`, 0 InitializeWebProcess / CreateWebPage as messages (the 30 init-log InitializeWebProcess / CreateWebPage hits are the “accepted connection=… (InitializeWebProcess/CreateWebPage ride this conn)” diagnostic lines, not deliveries), 0 `wk-peer reply`, 0 `handed to xpc_main handler`. No WebContent `remoteContextWithOptions` (five remoteContext lines are Safari chrome, AppKit open/save, QuickLook, and two screencapture, all `portNumber=0`).

This rules out “the helper wrap on the unique listener was forcing it onto main and that alone prevented accepts” as the *sole* remaining delivery failure — skips now match the intended raw setter, accepts arrive off-main, and `send_message_with_reply` still does not surface as a dictionary event on the accepted peer. The remaining production-vs-probe gap is why those dictionaries never arrive on the accepted (still wrap’d, main-queued) incoming connections after the chroot proxy exec into the stock WebKit image.

### What was not done

No second variant. net33 libmachook was left deployed (arm64e `662c8443…`) with net31 proxies (`124857f9…`) and the net19 path layout. Rollback copies remain at `pre-net33-20261110-234320` (net32 `358ba07c…` + proxies `124857f9…`), `pre-net32-20261110-233630` (net31 `06da6f23…` + proxies `124857f9…`), `pre-net31-20261110-232604` (net30 `333ef668…` + net20 proxies `2097451d…`), `pre-net30-20261110-231440` (net29 `5e2aebc4…`), `pre-net29-20261110-230747` (net27 `563a6880…`), `pre-net28-20261110-230040` (also net27), `pre-net27-20261110-224355` (net26 `0551602b…`), `pre-net26-20261110-223631`, `pre-net25-20261110-222759`, `pre-net24-20261110-221300`, `pre-net23-20261110-215737`, `pre-net22-20261110-214530`, `pre-net21-20261110-212258`, `pre-net20-20261110-211200`, `pre-net19-20261110-205054` (path layout), `pre-net18-20261110-203748`, and `pre-net17-20261110-202135`. No SkyLight hooks, no extra Safari CAContext IMPs, no constructor `dispatch_after`, no new remapper export, no public macosbooter check-in, no competing Mach listener on that name, no CAContext port-retry, no macwshostd restart, no reboot, no 停止 macOS. IconServices jobs were left loaded.

### Paint / IPC conclusion

The window is visible and the unique listener no longer takes the helper wrap (31 `helper=0 wrap=0`, 0 listener/helper=1 overlap). Real incoming accepts now arrive (30 `unique peer incoming connection=`, `pthread_main=0`), but InitializeWebProcess / CreateWebPage still did **not** arrive as real IPC. Screenshots: `/workspace/net33-example.png` and `/workspace/net33-baidu.png` (chrome painted, content white). 0 `xpc event begin`, 0 `InitializeWebKit2 call`, 0 replies, no WebContent CAContext, content still white. Skipping the helper wrap on the unique listener unblocked accepts but did not fix dictionary delivery. The remaining gap is why `send_message_with_reply` on a live `create_from_endpoint` connection, now accepted by the unique listener, never surfaces as a helper event on that incoming peer in the production unique WebContent environment (the unsandboxed probe still delivers dictionaries; the incoming peers still take the main-queue wrap). Next work should keep the pool, the kept-alive recv and send right, register + look_up, the authentic endpoint, unique-path `XPC_FLAGS=0`, the omitted unique `XPC_SERVICE_NAME`, the `dispatch_main` path, the `set_instance` skip, and the unique-listener wrap skip, and should inspect delivery on the accepted incoming connection — without checking in the public macosbooter name, adding a competing Mach listener on that name, retrying CAContext ports, or stalling Safari's launch thread.

## 51. Unique accepted peers stay on a live serial queue; wrap fires; main bounce does not run (2026-11-10 23:51–23:53 CST / 07:51–07:53 PT)

### Change

net33 skipped the helper wrap on the unique listener and got the first production accepts (30 `unique peer incoming connection=`, `pthread_main=0`). The stock `xpc_main` handler then armed and resumed those incoming peers (60 `set-event-handler … helper=1` on 30 distinct pointers, all 30 resumed). Authoritative follow-up IPC stayed zero: 0 `wk-peer xpc-event`, 0 `xpc event begin`, 0 `InitializeWebKit2 call`. The remaining production-vs-probe gap on that already-accepted peer was the helper wrap still forcing the incoming connection onto the main queue. The unsandboxed probe delivered dictionaries on a live background serial queue and never took that wrap. net33 field logs showed the wrap handler itself never ran, which is consistent with libxpc delivering onto a main queue that `dispatch_main` does not drain the way a live GCD serial queue does.

net34 therefore keeps unique accepted peers on a live background serial queue `com.macwsguide.wk.peer-incoming` (`macws_wk_unique_incoming_queue`). `macws_webkit_xpc_main_connection` logs `target=incoming-serial` and does not `set_target_queue(main)` for those conns. The wrap still logs and still bounces off-main events onto main with `dispatch_async` so InitializeWebKit2's isMainThread assertion can hold, but it no longer forces `mainq` (`wk-peer set-event-handler-incoming … helper=1 wrap=1 mainq=0`). Unique listeners still skip the wrap entirely (`helper=0 wrap=0`). Fallback helpers are unchanged. The pool, kept-alive recv, register + look_up, authentic endpoint (`src=orig`) with kept send right, unique-path `XPC_FLAGS=0`, omitted unique `XPC_SERVICE_NAME`, raw setter / wrap skip on the unique listener, `dispatch_main` handoff, and shared-cache `set_instance` skip are unchanged. No public macosbooter check-in, no named Mach LISTENER on that public name, no constructor `dispatch_after`, no new exported symbols (168 T per slice). Proxies and the net19 path layout stayed on net31 / net19. 14 contract tests OK on the Intel Mac (9 peer-spawn + 5 environment).

### Deploy

Backup tag `pre-net34-20261110-235109` (23:51:09 CST / 07:51:09 PT) saved the live net33 libmachook (arm64e `662c8443cbab1d84b74b8ae3f812001150e1a0e7`, arm64 `5b6dc824ece5bfb29a46747cb753e11f1e5a8ca4`) and net31 proxies (WebContent `124857f954b1d5174741de2e71bb1b078cda15ec`). The net19 path layout was not touched. Only libmachook was replaced (Theos `FINALPACKAGE=1 STRIP=0 OPTFLAG=-O2`, `misc/set_macos_version.py` LC_BUILD_VERSION macOS 13.0, host two-pass `ldid -S` + `codesign`, device two-pass `ldid -S$ENT -M`, `jbctl trustcache add`, rootfs byte-copy not re-signed). Live net34 CDHashes after device ldid: arm64e `d774151c50fb3e893c1d93f3d7df0063e200bc7b`, arm64 `6724bb1f417ae8fd618689b9ff0c4fc9184465ce`. WebContentProxy remained `124857f9…`. Smoke at 23:51:15 CST / 07:51:15 PT: `SMOKE_OK` / `RUN_BASH_CANARY_OK` rc=0, IconServices 1724 / 53067 / 53071 loaded, WindowServer 51054 `Ss`, macwshostd 416, SpringBoard 391. `SMOKE_PASS`. No reboot, no 停止 macOS.

### Safari run (one guarded attempt)

23:51:19 CST / 07:51:19 PT onward, Safari pid 80615. `LAUNCH_WINDOW_OK`. example.com and baidu.com were opened. RSS 892272 KB after example.com and 892288 KB after baidu.com (~871 MB). Screenshots `/workspace/net34-example.png` and `/workspace/net34-baidu.png` (menu-bar clock 07:52 PT / 23:52 CST on both): Safari chrome and tabs painted (example.com / baidu.com in the address field), content area white. Cleanup `CORE_OK`, WebKit LaunchDaemons unloaded, IconServices left loaded.

Because the window stayed capturable, net34 was left deployed. Rollback copies at `pre-net34-20261110-235109` (net33 `662c8443…` + proxies `124857f9…`) and `pre-net33-20261110-234320` (net32 `358ba07c…` + proxies `124857f9…`) were not used. The rollback script still recopies the signed jb dylib onto a fresh rootfs vnode (not re-signed).

### Field result

The incoming-queue change landed: 31 `wk-peer set-event-handler-raw … helper=0 wrap=0` on unique listeners, 60 `wk-peer set-event-handler-incoming … helper=1 wrap=1 mainq=0` on 30 accepted peers, 0 plain `set-event-handler … helper=1` force-main lines, and 30 `accepted connection=… target=incoming-serial`. All 30 incoming peers were still resumed by the stock handler. Unique helpers still reached hooked `xpc_main` with `service=(null) flags=0x0` (31 omitted `XPC_SERVICE_NAME`). Handshake otherwise matched net33: 32 register/spawn, 31 `wk-peer recv-endpoint typ=endpoint … rebuilt_used=0 src=orig kept_port=1`, 31 ready, 30 `wk-peer connected` (29 WebContent + Networking), 30 `skip-set-instance`, 30 `send-message-with-reply`, 0 `src=rebuilt`, 0 endpoint-timeout, 0 `xpc_error`, no public macosbooter check-in. 31 helpers armed the accept handler and entered `dispatch_main`. GPU was ready but not popped. SandboxBroker hit grace-timeout / pop-miss. One pop-timeout. 28 fallbacks (25 WebContent + 3 SandboxBroker).

Unlike net33, the wrap handler on the accepted peer now actually runs. After `entering dispatch_main`, each of the 30 accepted peers logged `xpc event off main; dispatch_async` (`pthread_main=0`) — the first time a production unique helper has seen a real event on that incoming connection. The bounce then submitted `run()` to the main queue.

Authoritative follow-up IPC is still zero: 0 `wk-peer xpc-event`, 0 `xpc event begin`, 0 `InitializeWebKit2 call`, 0 `runInitializationCode call`, 0 InitializeWebProcess / CreateWebPage as messages (the 30 init-log “accepted connection=… (InitializeWebProcess/CreateWebPage ride this conn)” lines remain diagnostic), 0 `wk-peer reply`. Those event-begin / xpc-event logs live inside `run()`, which never executed, so the 30 off-main events were queued onto main and not drained. No WebContent `remoteContextWithOptions` (five remoteContext lines are Safari chrome, AppKit open/save, QuickLook, and two screencapture, all `portNumber=0`).

This rules out “the remaining wrap forced the accepted peer onto main and that alone prevented libxpc from delivering the with-reply message into the handler” as the sole cause: delivery onto a live background serial queue now reaches the wrap, matching the probe's receive queue class, and the stock handler is armed and resumed. The remaining gap is that the wrap's `dispatch_async` onto the helper's main queue does not run `run()` / the stock dictionary callback even though `dispatch_main` was already entered — so InitializeWebProcess never executes. The probe never needed that bounce (it was not a WebKit helper and handled dictionaries on the same background queue).

### What was not done

No second variant. net34 libmachook was left deployed (arm64e `d774151c…`) with net31 proxies (`124857f9…`) and the net19 path layout. Rollback copies remain at `pre-net34-20261110-235109` (net33 `662c8443…` + proxies `124857f9…`), `pre-net33-20261110-234320` (net32 `358ba07c…` + proxies `124857f9…`), `pre-net32-20261110-233630` (net31 `06da6f23…` + proxies `124857f9…`), `pre-net31-20261110-232604` (net30 `333ef668…` + net20 proxies `2097451d…`), `pre-net30-20261110-231440` (net29 `5e2aebc4…`), `pre-net29-20261110-230747` (net27 `563a6880…`), `pre-net28-20261110-230040` (also net27), `pre-net27-20261110-224355` (net26 `0551602b…`), `pre-net26-20261110-223631`, `pre-net25-20261110-222759`, `pre-net24-20261110-221300`, `pre-net23-20261110-215737`, `pre-net22-20261110-214530`, `pre-net21-20261110-212258`, `pre-net20-20261110-211200`, `pre-net19-20261110-205054` (path layout), `pre-net18-20261110-203748`, and `pre-net17-20261110-202135`. No SkyLight hooks, no extra Safari CAContext IMPs, no constructor `dispatch_after`, no new remapper export, no public macosbooter check-in, no competing Mach listener on that name, no CAContext port-retry, no macwshostd restart, no reboot, no 停止 macOS. IconServices jobs were left loaded. `dispatch_main` was not replaced with `CFRunLoopRun`.

### Paint / IPC conclusion

The window is visible, unique listeners still skip the helper wrap, and unique accepted peers now stay on a live background serial queue (`target=incoming-serial`, `mainq=0`). Real events now reach the incoming wrap (30 `xpc event off main; dispatch_async` after `dispatch_main`), but InitializeWebProcess / CreateWebPage still did **not** arrive as real IPC because the bounce onto main never ran `run()` / the stock handler. Screenshots: `/workspace/net34-example.png` and `/workspace/net34-baidu.png` (chrome painted, content white). 0 `xpc event begin`, 0 `InitializeWebKit2 call`, 0 replies, no WebContent CAContext, content still white. Moving the accepted peer off main unblocked delivery into the wrap but did not get the dictionary callback onto a drained main queue. The remaining gap is why `dispatch_async(dispatch_get_main_queue(), run)` in the unique helper does not execute after `dispatch_main` — without checking in the public macosbooter name, adding a competing Mach listener on that name, retrying CAContext ports, stalling Safari's launch thread, or replacing `dispatch_main` with a long `CFRunLoopRun` on this turn. Next work should keep the pool, the kept-alive recv and send right, register + look_up, the authentic endpoint, unique-path `XPC_FLAGS=0`, the omitted unique `XPC_SERVICE_NAME`, the unique-listener wrap skip, the incoming serial queue, the `dispatch_main` path, and the `set_instance` skip, and should get `run()` / InitializeWebProcess to actually execute on those already-delivered events.

## 52. CFRunLoopRun park did not drain run() and lost the Safari window; rolled back to net34 (2026-11-10 23:57–23:59 CST / 07:57–07:59 PT)

### Change

net34 kept the Safari window and got the first production events into the unique-incoming wrap: after `entering dispatch_main`, all 30 accepted peers logged `xpc event off main; dispatch_async` (`pthread_main=0`). `run()` / the stock handler never executed (0 `xpc event begin`, 0 `InitializeWebKit2 call`, 0 replies). Those begin/xpc-event logs live inside `run()`, so the 30 events were queued onto the helper's GCD main queue and not drained.

Diagnosis: these Application-type helpers already load Foundation. On Darwin, once CoreFoundation has attached, the GCD main queue is drained by the main CFRunLoop, while `dispatch_main()` only `sigsuspend`s and does not run `dispatch_async(main, …)` blocks. That matches the net34 field log (bounce scheduled after `dispatch_main`, `run()` never runs). The unsandboxed probe never needed that bounce: it handled dictionaries on the same live background queue and was not a WebKit helper.

net35 therefore parks the unique-helper main thread in `CFRunLoopRun()` after endpoint-send, so the existing wrap bounce (`dispatch_async` onto main, then `run()` / the stock handler) can be drained on the main thread and satisfy InitializeWebKit2's isMainThread assertion. This is the helper's own park, not a wait on Safari's launch thread, and not `NSApp` `-run` (which previously stalled when invoked on the `xpc_main` stack). `dispatch_main()` remains as a fallback if the run loop is stopped. Unique listeners still skip the wrap, accepted peers stay on `com.macwsguide.wk.peer-incoming`, and the pool, kept-alive recv, register + look_up, authentic endpoint (`src=orig`) with kept send right, unique-path `XPC_FLAGS=0`, omitted unique `XPC_SERVICE_NAME`, and `set_instance` skip are unchanged. No public macosbooter check-in, no named Mach LISTENER on that public name, no constructor `dispatch_after`, no new exported symbols (168 T per slice). Proxies and the net19 path layout stayed on net31 / net19. 14 contract tests OK on the Intel Mac (9 peer-spawn + 5 environment).

### Deploy

Backup tag `pre-net35-20261110-235755` (23:57:55 CST / 07:57:55 PT) saved the live net34 libmachook (arm64e `d774151c50fb3e893c1d93f3d7df0063e200bc7b`, arm64 `6724bb1f417ae8fd618689b9ff0c4fc9184465ce`) and net31 proxies (WebContent `124857f954b1d5174741de2e71bb1b078cda15ec`). The net19 path layout was not touched. Only libmachook was replaced (Theos `FINALPACKAGE=1 STRIP=0 OPTFLAG=-O2`, `misc/set_macos_version.py` LC_BUILD_VERSION macOS 13.0, host two-pass `ldid -S` + `codesign`, device two-pass `ldid -S$ENT -M`, `jbctl trustcache add`, rootfs byte-copy not re-signed). Live net35 CDHashes after device ldid: arm64e `01785942432c6178def4f786fd49a081bf974e91`, arm64 `bd4888d075afd30b9388e43a39112a109736720a`. WebContentProxy remained `124857f9…`. Smoke at 23:58:01 CST / 07:58:01 PT: `SMOKE_OK` / `RUN_BASH_CANARY_OK` rc=0, IconServices 1724 / 53067 / 53071 loaded, WindowServer 51054 `Ss`, macwshostd 416, SpringBoard 391. `SMOKE_PASS`. No reboot, no 停止 macOS.

### Safari run (one guarded attempt)

23:58:07 CST / 07:58:07 PT onward, Safari pid 81921. The custom launch path reported `ok=no launched-pid=0 message=custom-path 已启动，但 30 秒内没有发布可捕获的 AppKit 窗口`. After-launch diagnostics still showed Safari pid 81921 RSS 242656 KB (~237 MB) and the unique WebKit helpers running, plus one Safari-chrome `remoteContextWithOptions` (`portNumber=0`). `LAUNCH_WINDOW_FAIL`; example.com and baidu.com were not opened. No `/workspace/net35-example.png` or `/workspace/net35-baidu.png`. Cleanup `CORE_OK`, `DONE_NO_WINDOW`. IconServices left loaded.

Because the window did not publish, net35 was rolled back immediately with `rollback.sh pre-net35-20261110-235755` (restore net34). Rollback smoke at 23:59:09 CST / 07:59:09 PT: `SMOKE_OK` / `RUN_BASH_CANARY_OK` rc=0 on the first try (no rootfs-vnode SIGKILL, no recopy retry, no reboot). Live libmachook is again net34 arm64e `d774151c…` / arm64 `6724bb1f…`, WebContentProxy `124857f9…`, IconServices 1724 / 53067 / 53071 loaded, WindowServer 51054, macwshostd 416, SpringBoard 391. `SMOKE_PASS`. `pre-net34-20261110-235109` (net33) was not needed.

### Field result

The CFRunLoop park landed in the helpers: 31 `entering CFRunLoopRun (Foundation owns main queue; dispatch_main would not drain)`, 0 `entering dispatch_main`, 0 `CFRunLoopRun returned`. Unique listeners still skipped the wrap (31 `helper=0 wrap=0`). Accepted peers stayed on the incoming serial queue (60 `set-event-handler-incoming … mainq=0`, 30 `target=incoming-serial`, all 30 resumed). Handshake otherwise matched net34: 32 register/spawn, 31 `recv-endpoint … src=orig kept_port=1`, 31 ready, 30 connected, 30 `skip-set-instance`, 30 `send-message-with-reply`, 31 `service=(null) flags=0x0`, 0 pop-timeout, 0 `xpc_error`, no public macosbooter check-in. GPU was ready but not popped. SandboxBroker hit grace-timeout / pop-miss. 25 fallbacks.

The wrap still saw the events: after `entering CFRunLoopRun`, all 30 accepted peers logged `xpc event off main; dispatch_async` (`pthread_main=0`). Authoritative follow-up IPC stayed zero: 0 `wk-peer xpc-event`, 0 `xpc event begin`, 0 `InitializeWebKit2 call`, 0 `runInitializationCode call`, 0 `wk-peer reply`. `run()` still never executed, so `CFRunLoopRun()` did not drain those already-queued main-queue bounce blocks in this environment either. No WebContent CAContext.

The CFRunLoop park also prevented Safari from publishing a capturable AppKit window within the 30 s launch-path timeout, unlike `dispatch_main` on the same unique path (net25–net34). The helper CFRunLoop did not get `run()` onto main, and it was not a safe substitute for `dispatch_main` here.

### What was not done

No second variant. net35 was rolled back; the device is back on net34 libmachook (arm64e `d774151c…`) with net31 proxies (`124857f9…`) and the net19 path layout. Rollback copies remain at `pre-net35-20261110-235755` (net34 `d774151c…` + proxies `124857f9…`; used), `pre-net34-20261110-235109` (net33 `662c8443…` + proxies `124857f9…`; unused this turn), `pre-net33-20261110-234320` (net32 `358ba07c…`), `pre-net32-20261110-233630` (net31 `06da6f23…`), `pre-net31-20261110-232604` (net30 `333ef668…` + net20 proxies `2097451d…`), and earlier tags as in §51. No SkyLight hooks, no extra Safari CAContext IMPs, no constructor `dispatch_after`, no new remapper export, no public macosbooter check-in, no competing Mach listener on that name, no CAContext port-retry, no macwshostd restart, no reboot, no 停止 macOS. IconServices jobs were left loaded. `NSApp` `-run` was not used.

### Paint / IPC conclusion

The Safari window did **not** stay visible, so net35 was rolled back to net34 and no net35 screenshots were taken. `run()` / the stock handler still did **not** execute: 0 `xpc event begin`, 0 `InitializeWebKit2 call`, 0 replies, no WebContent CAContext, no page paint. Parking unique helpers in `CFRunLoopRun()` did not drain the net34 `dispatch_async(main, run)` bounce and lost the launch window. The remaining gap is still how to get those already-delivered incoming-serial events to run the stock dictionary callback on a thread/queue that actually drains in this Foundation helper, without a helper park that blocks Safari's AppKit window from publishing — without checking in the public macosbooter name, adding a competing Mach listener on that name, retrying CAContext ports, or stalling Safari's launch thread. Next work should start from the restored net34 path (incoming serial queue, wrap bounce, `dispatch_main` park) and try a drain that does not replace that park with `CFRunLoopRun` / `NSApp` `-run`.

## 53. Helper-main-drain ran run() on main, stock handler never returned, window lost; rolled back to net34 (2026-11-11 00:02–00:03 CST / 2026-11-10 08:02–08:03 PT)

### Change

net35 parked unique helpers in `CFRunLoopRun()` so the wrap's `dispatch_async(main, run)` bounce could drain. The run loop was entered (31 helpers), the wrap still logged `xpc event off main; dispatch_async`, `run()` still never executed, and Safari did not publish a capturable AppKit window within 30 s. That park was rolled back to net34. The standing constraint for this turn was not to retry `CFRunLoopRun` or `NSApp` `-run`.

net34 already delivers unique incoming events onto a live background serial queue. The remaining gap is getting `run()` / the stock dictionary callback onto a thread that actually drains, without a helper park that blocks Safari's window. `dispatch_main()` does not drain GCD main (Foundation has attached; it only `sigsuspend`s). net36 therefore keeps delivery on `com.macwsguide.wk.peer-incoming`, and for unique incoming peers schedules `run()` onto a small cond-wait drain on the helper main thread (`macws_wk_schedule_on_helper_main` / `macws_wk_helper_main_drain`) after endpoint-send. That drain is not a CFRunLoop and not NSApp; `dispatch_main()` remains as a fallback if it ever returns. Other helpers still bounce with `dispatch_async` onto GCD main. Unique listeners still skip the wrap. The pool, kept-alive recv, register + look_up, authentic endpoint (`src=orig`) with kept send right, unique-path `XPC_FLAGS=0`, omitted unique `XPC_SERVICE_NAME`, and `set_instance` skip are unchanged. No public macosbooter check-in, no named Mach LISTENER on that public name, no constructor `dispatch_after`, no new exported symbols (168 T per slice). Proxies and the net19 path layout stayed on net31 / net19. 14 contract tests OK on the Intel Mac (9 peer-spawn + 5 environment).

### Deploy

Backup tag `pre-net36-20261111-000251` (00:02:51 CST / 08:02:51 PT) saved the live net34 libmachook (arm64e `d774151c50fb3e893c1d93f3d7df0063e200bc7b`, arm64 `6724bb1f417ae8fd618689b9ff0c4fc9184465ce`) and net31 proxies (WebContent `124857f954b1d5174741de2e71bb1b078cda15ec`). The net19 path layout was not touched. Only libmachook was replaced (Theos `FINALPACKAGE=1 STRIP=0 OPTFLAG=-O2`, `misc/set_macos_version.py` LC_BUILD_VERSION macOS 13.0, host two-pass `ldid -S` + `codesign`, device two-pass `ldid -S$ENT -M`, `jbctl trustcache add`, rootfs byte-copy not re-signed). Live net36 CDHashes after device ldid: arm64e `59453370e3a44242dfdffcb2e2c961029de1d104`, arm64 `8303d5c2759abfc2192083c8ae41f8e7793b50e7`. WebContentProxy remained `124857f9…`. Smoke at 00:02:56 CST / 08:02:56 PT: `SMOKE_OK` / `RUN_BASH_CANARY_OK` rc=0, IconServices 1724 / 53067 / 53071 loaded, WindowServer 51054 `Ss`, macwshostd 416, SpringBoard 391. `SMOKE_PASS`. No reboot, no 停止 macOS.

### Safari run (one guarded attempt)

00:03:02 CST / 08:03:02 PT onward, Safari pid 82786. The custom launch path reported `ok=no launched-pid=0 message=custom-path 已启动，但 30 秒内没有发布可捕获的 AppKit 窗口`. After-launch diagnostics still showed Safari pid 82786 RSS 242704 KB (~237 MB) and the unique WebKit helpers running, plus one Safari-chrome `remoteContextWithOptions` (`portNumber=0`). `LAUNCH_WINDOW_FAIL`; example.com and baidu.com were not opened. No `/workspace/net36-example.png` or `/workspace/net36-baidu.png`. Cleanup `CORE_OK`, `DONE_NO_WINDOW`. IconServices left loaded.

Because the window did not publish, net36 was rolled back immediately with `rollback.sh pre-net36-20261111-000251` (restore net34). Rollback smoke at 00:03:52 CST / 08:03:52 PT: `SMOKE_OK` / `RUN_BASH_CANARY_OK` rc=0 on the first try (no rootfs-vnode SIGKILL, no recopy retry, no reboot). Live libmachook is again net34 arm64e `d774151c…` / arm64 `6724bb1f…`, WebContentProxy `124857f9…`, IconServices 1724 / 53067 / 53071 loaded, WindowServer 51054, macwshostd 416, SpringBoard 391. `SMOKE_PASS`. `pre-net35-20261110-235755` (also net34) and `pre-net34-20261110-235109` (net33) were not needed.

### Field result

The helper-main-drain landed: 31 `entering helper-main-drain`, 0 `entering CFRunLoopRun`, 0 `entering dispatch_main`, 0 overflow. Unique listeners still skipped the wrap (31 `helper=0 wrap=0`). Accepted peers stayed on the incoming serial queue (60 `set-event-handler-incoming … mainq=0`, 30 `target=incoming-serial`, all 30 resumed). Handshake otherwise matched net34: 32 register/spawn, 31 `recv-endpoint … src=orig kept_port=1`, 31 ready, 30 connected, 30 `skip-set-instance`, 30 `send-message-with-reply`, 31 `service=(null) flags=0x0`, 0 pop-timeout, 0 `xpc_error`, no public macosbooter check-in. GPU was ready but not popped. SandboxBroker hit grace-timeout / pop-miss. 25 fallbacks (24 WebContent + 1 SandboxBroker).

Unlike net34/net35, the scheduled bounce actually ran on the helper main thread: 30 `schedule helper-main-drain`, 30 `helper-main-drain invoke`, then 58 `xpc event begin type=dictionary main=1` and 58 `wk-peer xpc-event type=dictionary` (29 WebContent helpers × the double DYLD_INTERPOSE+MSHook wrap). That is the first time production unique WebContent has executed `run()` on main with a real dictionary event.

The stock handler then did **not** return: 0 `xpc event end` (logged only after `user(event)`), 0 `InitializeWebKit2 call` (the hook was installed — 57 `InitializeWebKit2 raw=` lines — but never entered), 0 `runInitializationCode call`, 0 `wk-peer reply`. No WebContent `remoteContextWithOptions` (the one remoteContext line is Safari chrome, `portNumber=0`). So the dictionaries reached `run()` / the start of the stock callback on the main thread, but the callback blocked or never came back, and InitializeWebProcess / CreateWebPage were not observed as completed IPC. The hung `user(event)` on the helper main thread is a plausible reason Safari's with-reply never completed and the AppKit window did not publish within 30 s, same launch-path failure class as net35.

### What was not done

No second variant. net36 was rolled back; the device is back on net34 libmachook (arm64e `d774151c…`) with net31 proxies (`124857f9…`) and the net19 path layout. Rollback copies remain at `pre-net36-20261111-000251` (net34 `d774151c…` + proxies `124857f9…`; used), `pre-net35-20261110-235755` (also net34; unused this turn), `pre-net34-20261110-235109` (net33 `662c8443…`), `pre-net33-20261110-234320` (net32 `358ba07c…`), `pre-net32-20261110-233630` (net31 `06da6f23…`), and earlier tags as in §52. No SkyLight hooks, no extra Safari CAContext IMPs, no constructor `dispatch_after`, no new remapper export, no public macosbooter check-in, no competing Mach listener on that name, no CAContext port-retry, no macwshostd restart, no reboot, no 停止 macOS. IconServices jobs were left loaded. `CFRunLoopRun` and `NSApp` `-run` were not used.

### Paint / IPC conclusion

The Safari window did **not** stay visible, so net36 was rolled back to net34 and no net36 screenshots were taken. `run()` **did** start on the helper main thread with real dictionary events (`type=dictionary main=1`), but the stock handler did not return and InitializeWebProcess / CreateWebPage did **not** arrive as completed IPC: 0 `xpc event end`, 0 `InitializeWebKit2 call`, 0 replies, no WebContent CAContext, no page paint. The helper-main-drain fixed the net34 “bounce never runs” gap, then hung inside `user(event)` and lost the launch window. The remaining gap, starting again from restored net34, is how to run that already-delivered dictionary through the stock callback to InitializeWebKit2 / a reply without a helper-main park that blocks Safari's AppKit window from publishing — without checking in the public macosbooter name, adding a competing Mach listener on that name, retrying CAContext ports, stalling Safari's launch thread, or retrying `CFRunLoopRun` / `NSApp` `-run` / this cond-wait drain as the next first step.

## 54. Running user() on incoming-serial and inlining GCD main still hung the stock handler and lost the Safari window; rolled back to net34 (2026-11-11 00:12–00:14 CST / 2026-11-10 08:12–08:14 PT)

### Change

net36's helper-main-drain ran `run()` on the helper main thread with real dictionary events, but `user(event)` never returned (0 `xpc event end`, 0 `InitializeWebKit2 call`) and Safari did not publish a capturable AppKit window within 30 s. That park was rolled back to net34. The standing constraint for this turn was not to retry `CFRunLoopRun`, `NSApp` `-run`, or the cond-wait drain, and to prefer net34's `dispatch_main` for window safety.

Diagnosis from the net36 hang: `run()` was already on the helper main thread (`type=dictionary main=1`) when the stock callback blocked, before `InitializeWebKit2` was entered. A plausible cause is a nested bounce onto the GCD main queue or an isMainThread wait while main was parked in the cond-wait drain (main could not drain GCD main / CFRunLoop). net34 already delivers unique incoming events onto a live background serial queue; `dispatch_main` keeps the window but does not drain `dispatch_async(main, run)`.

net37 therefore restores the net34 `dispatch_main` park after endpoint-send (no `CFRunLoopRun`, no `NSApp`, no helper-main-drain). Unique incoming events call `run()` / the stock handler directly on `com.macwsguide.wk.peer-incoming`. While that run is active, a TLS flag makes `pthread_main_np` / `+[NSThread isMainThread]` report main, and shared-cache hooks for `dispatch_sync` / `dispatch_async` / `dispatch_sync_f` / `dispatch_async_f` execute main-queue blocks inline on the incoming-serial thread. Those hooks are installed only in WebKit helper processes, not in Safari. `hooked_InitializeWebKit2` / `hooked_runInitializationCode` call orig directly when the flag is set, so they do not `dispatch_sync` onto an undrained GCD main. Unique listeners still skip the wrap, accepted peers stay on the incoming serial queue, and the pool, kept-alive recv, register + look_up, authentic endpoint (`src=orig`) with kept send right, unique-path `XPC_FLAGS=0`, omitted unique `XPC_SERVICE_NAME`, and `set_instance` skip are unchanged. No public macosbooter check-in, no named Mach LISTENER on that public name, no constructor `dispatch_after`, no new exported symbols (168 T per slice). Proxies and the net19 path layout stayed on net31 / net19. 14 contract tests OK on the Intel Mac (9 peer-spawn + 5 environment).

### Deploy

Backup tag `pre-net37-20261111-001219` (00:12:19 CST / 08:12:19 PT) saved the live net34 libmachook (arm64e `d774151c50fb3e893c1d93f3d7df0063e200bc7b`, arm64 `6724bb1f417ae8fd618689b9ff0c4fc9184465ce`) and net31 proxies (WebContent `124857f954b1d5174741de2e71bb1b078cda15ec`). The net19 path layout was not touched. Only libmachook was replaced (Theos `FINALPACKAGE=1 STRIP=0 OPTFLAG=-O2`, `misc/set_macos_version.py` LC_BUILD_VERSION macOS 13.0, host two-pass `ldid -S` + `codesign`, device two-pass `ldid -S$ENT -M`, `jbctl trustcache add`, rootfs byte-copy not re-signed). Live net37 CDHashes after device ldid: arm64e `7434c64bac58b1887c48b98eeac58491bb435fc6`, arm64 `8fd6ca4602b2b9ccfb970b4ab358d438de793e8f`. WebContentProxy remained `124857f9…`. Smoke at 00:12:25 CST / 08:12:25 PT: `SMOKE_OK` / `RUN_BASH_CANARY_OK` rc=0, IconServices 1724 / 53067 / 53071 loaded, WindowServer 51054 `Ss`, macwshostd 416, SpringBoard 391. `SMOKE_PASS`. No reboot, no 停止 macOS.

### Safari run (one guarded attempt)

00:12:42 CST / 08:12:42 PT onward, Safari pid 83751. The custom launch path reported `ok=no launched-pid=0 message=custom-path 已启动，但 30 秒内没有发布可捕获的 AppKit 窗口`. After-launch diagnostics still showed Safari pid 83751 RSS 240672 KB (~235 MB) and unique WebKit helpers running, plus one Safari-chrome `remoteContextWithOptions` (`portNumber=0`). `LAUNCH_WINDOW_FAIL`; example.com and baidu.com were not opened. No `/workspace/net37-example.png` or `/workspace/net37-baidu.png`. Cleanup `CORE_OK`, `DONE_NO_WINDOW`. IconServices left loaded.

Because the window did not publish, net37 was rolled back immediately with `rollback.sh pre-net37-20261111-001219` (restore net34). Rollback smoke at 00:13:56 CST / 08:13:56 PT: `SMOKE_OK` / `RUN_BASH_CANARY_OK` rc=0 on the first try (no rootfs-vnode SIGKILL, no recopy retry, no reboot). Live libmachook is again net34 arm64e `d774151c…` / arm64 `6724bb1f…`, WebContentProxy `124857f9…`, IconServices 1724 / 53067 / 53071 loaded, WindowServer 51054, macwshostd 416, SpringBoard 391. `SMOKE_PASS`. `pre-net36-20261111-000251` (also net34) and earlier tags were not needed.

### Field result

The incoming-serial run and `dispatch_main` park landed: 31 `entering dispatch_main`, 0 `entering CFRunLoopRun`, 0 `entering helper-main-drain`. Unique listeners still skipped the wrap (31 `helper=0 wrap=0`). Accepted peers stayed on the incoming serial queue (60 `set-event-handler-incoming … mainq=0`, 30 `target=incoming-serial`, all 30 resumed). Handshake otherwise matched net34: 32 register/spawn, 31 `recv-endpoint … src=orig kept_port=1`, 31 ready (29 WebContent + Networking + GPU), 30 connected, 30 `skip-set-instance`, 30 `send-message-with-reply`, 31 `service=(null) flags=0x0`, 1 pop-timeout (WebContent), 1 grace-timeout, no public macosbooter check-in (0 `bootstrap_check_in`). GPU was ready but not popped. 25 fallbacks (24 WebContent + 1 SandboxBroker). After the hung run / cleanup, Safari logged 60 unique-peer `xpc_error` lines (`Connection interrupted`); those are not a public-name check-in.

Unlike net34, `run()` actually executed on the live incoming-serial queue: 30 `xpc event off main; run on incoming-serial` (`pthread_main=0` at schedule), then 58 `xpc event begin type=dictionary main=1` and 58 `wk-peer xpc-event type=dictionary` (29 WebContent helpers × the double wrap). The TLS `isMainThread` / `pthread_main_np` hooks are why begin logs say `main=1` even though the thread is incoming-serial. The stock path did bounce to GCD main: 30 `main-queue-inline-fn` (shared-cache `dispatch_sync_f` / `dispatch_async_f`) and 0 bare `main-queue-inline` (`dispatch_sync` / `dispatch_async` of a block). That is the first time production unique WebContent has executed `run()` off the helper main thread with a real dictionary event and with the GCD-main bounce inlined.

The stock handler then did **not** return: 0 `xpc event end` (logged only after `user(event)`), 0 `InitializeWebKit2 call` (the hook was installed — 57 `InitializeWebKit2 raw=` lines — but never entered), 0 `runInitializationCode call`, 0 `wk-peer reply`. No WebContent `remoteContextWithOptions` (the one remoteContext line is Safari chrome, `portNumber=0`). The inlined `dispatch_*_f(main)` work therefore ran, then blocked inside `user(event)` before InitializeWebKit2, same IPC stall as net36. Running the stock callback on incoming-serial (helper main staying in `dispatch_main`) was **not** enough to keep Safari's AppKit window: the launch path still failed in 30 s, so hanging inside `user(event)` is sufficient to block window publish even when helper main is not the parked thread. Safari's with-reply during launch is the likely coupling.

### What was not done

No second variant. net37 was rolled back; the device is back on net34 libmachook (arm64e `d774151c…`) with net31 proxies (`124857f9…`) and the net19 path layout. Rollback copies remain at `pre-net37-20261111-001219` (net34 `d774151c…` + proxies `124857f9…`; used), `pre-net36-20261111-000251` (also net34; unused this turn), `pre-net35-20261110-235755` (also net34), `pre-net34-20261110-235109` (net33 `662c8443…`), `pre-net33-20261110-234320` (net32 `358ba07c…`), `pre-net32-20261110-233630` (net31 `06da6f23…`), and earlier tags as in §52. No SkyLight hooks, no extra Safari CAContext IMPs, no constructor `dispatch_after`, no new remapper export, no public macosbooter check-in, no competing Mach listener on that name, no CAContext port-retry, no macwshostd restart, no reboot, no 停止 macOS. IconServices jobs were left loaded. `CFRunLoopRun`, `NSApp` `-run`, and the helper-main-drain were not used.

### Paint / IPC conclusion

The Safari window did **not** stay visible, so net37 was rolled back to net34 and no net37 screenshots were taken. `run()` **did** start on the incoming-serial queue with real dictionary events (`type=dictionary`, logged `main=1` only because of the TLS hook), and the stock path's `dispatch_*_f(main)` bounce was inlined (30 `main-queue-inline-fn`), but the stock handler did not return and InitializeWebProcess / CreateWebPage did **not** arrive as completed IPC: 0 `xpc event end`, 0 `InitializeWebKit2 call`, 0 replies, no WebContent CAContext, no page paint. Inlining the GCD-main bounce fixed the “callback waits on an undrained main queue” hypothesis as the sole cause, then still hung inside `user(event)` and lost the launch window. The remaining gap, starting again from restored net34, is why that already-delivered dictionary blocks inside the stock callback before InitializeWebKit2 / a reply — and how to get a reply without a delivery that stalls Safari's AppKit window — without checking in the public macosbooter name, adding a competing Mach listener on that name, retrying CAContext ports, stalling Safari's launch thread, or retrying `CFRunLoopRun` / `NSApp` `-run` / the cond-wait drain / this incoming-serial + inline-GCD-main run as the next first step.

## 55. user(event) hang is WorkQueue::main().dispatchSync / initializer needing a live main CFRunLoop; no deploy (2026-11-11 00:16–00:21 CST / 2026-11-10 08:16–08:21 PT)

### Status

Diagnosis only. No libmachook change and no device deploy this turn. The device was left on restored net34 (arm64e `d774151c50fb3e893c1d93f3d7df0063e200bc7b`, arm64 `6724bb1f417ae8fd618689b9ff0c4fc9184465ce`, WebContentProxy `124857f954b1d5174741de2e71bb1b078cda15ec`, net19 layout, IconServices 1724 / 53067 / 53071). No reboot, no 停止 macOS, no second variant, no screenshots. Rollback copies were not used.

### What was already known from the field

net34 delivers unique incoming dictionaries onto a live background serial queue and parks helper main in `dispatch_main`; the wrap logs `xpc event off main; dispatch_async` but `run()` never executes, and the Safari window stays visible.

net36 ran `run()` on the helper main thread via a cond-wait drain: 58 `xpc event begin type=dictionary main=1`, then 0 `xpc event end`, 0 `InitializeWebKit2 call`, and the launch window was lost.

net37 restored `dispatch_main`, ran `run()` on incoming-serial, inlined `dispatch_*_f(main)` (30 `main-queue-inline-fn`, 0 bare block `dispatch_sync` / `dispatch_async`), and lied about `pthread_main_np` / `+[NSThread isMainThread]` for that run. `run()` started (58 begin / xpc-event), the GCD-main bounce was inlined, then `user(event)` still never returned (0 end, 0 hook `InitializeWebKit2 call`) and the window was lost again. Inlining therefore ruled out “the callback only waits on an undrained GCD main queue” as the *sole* cause.

### Why “0 InitializeWebKit2 call” is not evidence IW2 never ran

Every net37 hook install logged `hooked InitializeWebKit2 via offset-unsigned … orig=0x0` and `hooked runInitializationCode … orig=0x0` (57 of each). That is the same shared-cache `MSHookFunction` failure documented in §22: no trampoline is created, the stock functions stay unhooked, and `hooked_InitializeWebKit2` never logs a call even if the stock `InitializeWebKit2` runs. The 57 `InitializeWebKit2 raw=` lines only mean the installer found the UUID offset. This observation also applies to net36’s “0 InitializeWebKit2 call”.

### Stock callback path (WebKit-7615.1.26.11.5 / Ventura 13.4, `safari-7615-branch`)

The accepted-peer `user(event)` is `WebKit::XPCServiceEventHandler`’s message block (`Source/WebKit/Shared/EntryPointUtilities/Cocoa/XPCService/XPCServiceMain.mm`). It requires a dictionary with `message-name`. For `"bootstrap"` it looks up `WebContentServiceInitializer` (or Networking/GPU) in the WebKit bundle, **sends** a `"process-finished-launching"` reply, then:

```
WorkQueue::main().dispatchSync({
    WebContentServiceInitializer(connection, event);
});
```

`WorkQueue::constructMainWorkQueue` is `dispatch_get_main_queue()`. `WorkQueueBase::dispatchSync` is `dispatch_sync_f` of a `DispatchWorkItem` — which is exactly the 30 `main-queue-inline-fn` hits (no block `dispatch_sync`). The net7 crash stack already named this path: `WebContentServiceInitializer` ← `XPCServiceEventHandler` block ← `_dispatch_sync_f_slow` ← `_xpc_connection_call_event_handler`.

`WebContentServiceInitializer` (`WebContentServiceEntryPoint.mm`) calls `WTF::initializeMainThread()` *before* `XPCServiceInitializer`, which then calls `InitializeWebKit2()`. `initializeMainThread()` is `initialize()` + `initializeMainThreadPlatform()` + `RunLoop::initializeMain()`. On Cocoa, `RunLoop::initializeMain()` sets WTF’s main `RunLoop` to `RunLoop::current()`, and the CF `RunLoop` constructor binds `CFRunLoopGetCurrent()` — the *calling thread’s* loop, not necessarily `CFRunLoopGetMain()`.

Apple’s xpcproxy delivers Application-type (`RunLoopType=_NSApplicationMain`) first events on the process main thread with a running main CFRunLoop, so `dispatch_sync_f(main)` can be completed by `_dispatch_sync_f_slow` pumping that loop, and `RunLoop::initializeMain()` binds the real main CFRunLoop.

### How that path hung in net36 vs net37

**net36 (real main thread, cond-wait drain):** `user()` is already on the process main thread, but that thread is not in a GCD main or CFRunLoop drain. `WorkQueue::main().dispatchSync` therefore takes `_dispatch_sync_f_slow` and waits for the main queue to be pumped. The cond-wait drain does not pump it, so the initializer (and IW2) never run. This is the “undrained GCD main” deadlock. It also explains why Safari’s launch window fails: the helper never finishes the bootstrap work Safari is waiting on (0 `wk-peer reply` in these runs; Safari’s `send_message_with_reply` callback never fired).

**net37 (incoming-serial + inlined `dispatch_sync_f` + isMainThread lie):** The `dispatch_sync_f` wait is bypassed, so the initializer *does* start on incoming-serial (30 `main-queue-inline-fn`). That is a different hang, inside the inlined work, still in `user()` so `run()` never returns. Two mutually reinforcing defects follow from not being on a *running process-main CFRunLoop`:

1. `WTF::initializeMainThread()` / `RunLoop::initializeMain()` ran off the real main thread (the TLS `pthread_main_np` lie only hides `isMainThread()` checks). WTF’s “main” `RunLoop` is then bound to the incoming-serial thread’s CFRunLoop, which is not running (the thread is inside `user()`). Later `RunLoop::main().dispatch` / `callOnMainRunLoopAndWait` work is signaled on a loop that will never drain, or on the real process main, which is parked in `dispatch_main()` and also does not drain CF.
2. Even the isMainThread assert in `runInitializationCode` cannot be trusted as a gate: if stock `InitializeWebKit2` did run (unhooked), the lie would let it proceed into `JSC::initialize()` / the rest of `XPCServiceInitializer` still without a live main CFRunLoop.

So inlining GCD-main `dispatch_*_f` fixed net36’s *first* deadlock and still left `user()` blocked, which is sufficient to keep Safari from publishing an AppKit window in 30 s (same launch-path class as net35/net36). The TLS lie is not a window-safe substitute for actually being on the process main thread with that thread’s CFRunLoop running.

Other leads that do not fit as well as the sole cause: a missing `message-name` would return from the stock handler and log `xpc event end`; a sandbox denial or `exit(EXIT_FAILURE)` after entitlements would kill the helper, but after-launch still showed many unique WebContent processes `Ss` at ~28 MB; the double wrap only explains the 2× begin/xpc-event counts; `orig=0x0` explains the missing hook logs, not the hang itself.

### Why a fix was not deployed

A correct delivery needs the stock initializer to run on the *real* process main thread *while a main CFRunLoop is actually running*, which is what xpcproxy provides for Application-type services and what this chroot-proxy `execve` does not. Every attempt so far to provide that park has been either incomplete or window-unsafe:

- `dispatch_main` (net34): window stays; it does not drain GCD main or CF, so `dispatch_sync_f(main)` / `run()` never complete.
- `CFRunLoopRun` (net35): did not drain the bounce and lost the window; not retried.
- helper-main-drain (net36): ran `run()` on main, then deadlocked in `dispatch_sync_f(main)`; lost the window; not retried.
- incoming-serial + inline GCD main + isMainThread lie (net37): bypassed the first deadlock, hung inside the initializer; lost the window; not retried.

No clearly window-safe alternative is identified on this turn (including “run a real CFRunLoop / NSApp on helper main”, already excluded, and any further isMainThread lie). A logging-only probe of `message-name` / `service-name` without calling `user()` would be window-safe and is unnecessary for the path identification above; it can wait. Next work should start from restored net34 and should not retry those four delivery/park variants as the first step. The remaining product problem is how to get a running process-main CFRunLoop (or an equivalent pump of `dispatch_sync_f(main)` *and* WTF’s main `RunLoop`) in the unique helper *without* blocking Safari’s AppKit window publish, still without a public macosbooter check-in, a competing Mach listener on that name, CAContext port retries, or a long serial wait on Safari’s launch thread.

### Paint / IPC conclusion

No new Safari run. The diagnosed reason `user(event)` does not return is the stock bootstrap path’s `WorkQueue::main().dispatchSync` → `WebContentServiceInitializer` / `WTF::initializeMainThread` / `InitializeWebKit2` requiring a live process-main CFRunLoop. net36 deadlocked on the `dispatch_sync_f(main)` wait; net37 inlined that wait and then hung inside the initializer after binding or waiting on the wrong (or unpumped) main `RunLoop`. `0 InitializeWebKit2 call` cannot be read as “IW2 never entered” because the offset hook still has `orig=0x0`. No window-safe fix was deployed. Device remains on net34; content-area paint and WebContent CAContext remain unreached from this unique-peer path.

## 56. Application-type bring-up probe: SERVICE_SLOT becomes a dead name after chroot+exec; stock xpc_main then SIGTRAPs (2026-11-11 01:19–01:24 CST / 2026-11-10 09:19–09:24 PT)

Standalone, generic (not Safari-only) probe of the system-level first-image path. No Safari launch, no new libmachook, no change to the live net34 WebKit install, no reboot, no 停止 macOS. IconServices 1724 / 53067 / 53071 stayed loaded. `com.macwsguide.webkit.*` LaunchDaemons stayed unloaded. No public `macosbooter.WebKit.*` check-in and no competing Mach listener on that name. Device remained on net34: libmachook arm64e `d774151c50fb3e893c1d93f3d7df0063e200bc7b` / arm64 `6724bb1f417ae8fd618689b9ff0c4fc9184465ce`, pre- and post-flight identical. Rollback tag `pre-net37-20261111-001219` was not used.

Sources (no binaries on the PR): `misc/appsvc_probe/` (`probe_proxy.c`, `probe_appsvc.c`, `probe_client.c`, `Info.plist` with `ServiceType=Application` and `RunLoopType=_NSApplicationMain`, `build.sh`, `run_probe.sh`). Throwaway on-device paths only: `/var/mobile/appsvc-probe/` and `/var/mnt/rootfs/tmp/macws-appsvc-probe.xpc/` (byte-copied after signing on the mobile vnode; no `ldid` on the rootfs vnode).

### What the first image did

The iOS-host proxy (`probe_proxy`, arm64e) allocated a RECEIVE right, `mach_port_insert_right` MAKE_SEND, `mach_ports_register` into `SERVICE_SLOT` (slot 2) while preserving `NAME_SERVER` / `ENVIRONMENT`, `bootstrap_register` under `com.macwsguide.probe.appsvc.<pid>` (not `macosbooter.WebKit.*`), then `chroot(/var/mnt/rootfs)` + `execve` of the macOS test service with `XPC_FLAGS=8019`. Before exec, `mach_ports_lookup` was:

`count=3 [0]=SEND typ=0x10000 [1]=0 [2]=RECV typ=0x30000 recv_rights=1`

(`register` kr=0). That is the layout the proxy comments in `ViewBridgeChrootProxy/main.c` expect to survive a same-task `chroot`+`execve`.

### A macOS image cannot start after this chroot without the live libmachook

The first three exec attempts never reached the test service `main`:

- iOS-built, `set_macos_version`-patched arm64e service: `Trace/BPT trap: 5` (wait 133), after `malloc: nano zone abandoned due to inability to reserve vm space`.
- Genuine macOS 13 arm64e (Xcode 26 SDK, `-target arm64e-apple-macos13`): same SIGTRAP.
- Genuine macOS 13 arm64 (the arch used by `launchservicesd` / other in-chroot custom executables): same SIGTRAP, still no constructor log.

A control on the same iOS-side `chroot /var/mnt/rootfs` path showed this is not specific to the probe binary: **stock Ventura `/bin/echo` and `/bin/sleep` also SIGTRAP (133)**. `MallocNanoZone=0` plus `GIGACAGE_ENABLED=0` / `HOME` / `TMPDIR` / `MACWS_CHROOT_HOST_ROOT` removed the nano-zone warning but still trapped. Inserting the *already deployed* net34 `/usr/local/lib/libmachook.dylib` made stock `/bin/echo` print and exit 0. That matches the production proxy, which always sets `DYLD_INSERT_LIBRARIES=/usr/local/lib/libmachook.dylib` (and `MallocNanoZone=0`) in `ViewBridgeChrootProxy/main.c`. The on-disk dylib was not replaced. `macws_is_webkit_helper_process()` keys only on WebContent / Networking / GPU / SandboxBroker paths, so this probe's `xpc_main` still falls through to `macws_xpc_main_raw` (stock).

### After exec (01:24:03.981 CST / 09:24:03.981 PT, proxy pid 86277)

With libmachook inserted, the arm64 test service constructor and `main` ran (same pid 86277). Authoritative after-exec inventory:

```
after-exec ports_lookup kr=0 count=3 [0]=0x807 typ=0x10000 [1]=0x0 typ=0x0 [2]=0xffffffff typ=0x100000 recv_rights=0 slot2_recv=0 flags=0x0 service=com.macwsguide.probe.appsvc
SERVICE_SLOT_SURVIVED=no
```

- **SERVICE_SLOT did not survive as a receive right.** Slot 2 is a **dead name** (`0xffffffff`, `typ=0x100000` = `MACH_PORT_TYPE_DEAD_NAME`), not an empty slot and not `typ=0x30000 RECV`. Slot 0 was refreshed `0x707` → `0x807` (SEND), the same post-exec NAME_SERVER pattern as the net16–net19 WebKit handshake (`recv_rights=0`).
- **`XPC_FLAGS` did not survive as `8019`.** The exec environment set `XPC_FLAGS=8019` (no `0x` prefix). By `main`, `getenv` was the string `0x0`. That rewrite matches the existing comment on `macws_webkit_reapply_packed_xpc_flags` in `libmachook/mac_hooks.m`: after the proxy `execve`, classic environ may be overwritten by **libxpc unpacking xpcproxy's packed launch context** (binprefs / registered ports). A homemade `mach_port_allocate` RECEIVE in slot 2 is not that packed context; unpack is the leading explanation for both the flags overwrite and the receive right dying into a dead name before `main`.
- Before `xpc_main`: `pthread_main=1`, `cf_main_mode=(null)` (main CFRunLoop not running yet), `_dispatch_get_main_queue_port_4CF=0xb03` (**GCD_MAIN_CF_PORT=valid**), `_dispatch_main_queue_callback_4CF` non-NULL. The GCD-main CF port exists at process start; the runloop was not yet pumping.
- `dlsym xpc_main` succeeded and stock `xpc_main` was entered (`macws_xpc_main` falls through to `macws_xpc_main_raw` for this argv). There was no `stock xpc_main returned`, no `accepted`, no sampler tick, no `dispatch_sync_f`, and no reply. Within ~1 s the process died **`wait_rc=133` / `proxy_signal=5` (SIGTRAP)**. The iOS client then got `bootstrap_look_up kr=1102` (unknown service; the name dies with the process) and `Connection invalid`.

This is the failure the probe was designed to report clearly: the Application-type receive right does not survive this `chroot`+`execve` as a live `SERVICE_SLOT` RECV, and stock `xpc_main` does not park and accept — it traps, consistent with `XPC_EXIT_REASON_UNMANAGED` / libxpc aborting when the packed launch context and receive right are missing. `dispatch_sync_f(main)` was never reached, so the CF-pump question remains unanswered on a successful accept (not a deadlock observation).

### Implication for a later Safari hookup

A raw `mach_port_allocate` + `mach_ports_register(SERVICE_SLOT)` + `XPC_FLAGS=8019` first image is **not** a substitute for Apple xpcproxy's Application-type bring-up. The right is present immediately before `chroot`+`execve` and is a dead name with `flags=0x0` by the time the stock image reaches `main`. That is the same `recv_rights=0` outcome as net16–net19, now seen on a non-WebKit test service with a known-good pre-exec RECV. Re-applying `XPC_FLAGS=8019` after exec (the existing WebKit fallback hook) cannot resurrect a destroyed receive right.

A later Safari hookup should not assume this placement is sufficient. Next work needs a real packed Application-type launch context (or another way to have stock `xpc_main` consume a live receive on the process main thread with a running main CFRunLoop) that still does not check in `macosbooter.WebKit.*`. This turn did not start a guarded Safari run, did not deploy a libmachook or proxy variant, and left net34 untouched.

Final throwaway CDHashes (not part of the WebKit install): probe_proxy arm64e `9b07655d587ca15bf2dd7f30e5d90f252389da45`; macws-appsvc-probe arm64 `ec6b676f569930306540f786d82238e9180f5520` (sha1 candidate `75001742915089225d070a45be51a963f6985e6f`).

## 57. Packed Application-type context: the RECEIVE is already a dead name before libxpc is linked; launchd's blob+ports is the missing piece (2026-11-11 01:34:01 CST / 2026-11-10 09:34:01 PT)

Follow-up to §56. Standalone throwaway probe only. No Safari launch, no new libmachook, no change to the live net34 WebKit install, no reboot, no 停止 macOS. IconServices 1724 / 53067 / 53071 stayed loaded. `com.macwsguide.webkit.*` LaunchDaemons stayed unloaded. No public `macosbooter.WebKit.*` check-in. Device remained on net34: libmachook arm64e `d774151c50fb3e893c1d93f3d7df0063e200bc7b` / arm64 `6724bb1f417ae8fd618689b9ff0c4fc9184465ce`, pre- and post-flight identical. Rollback tag `pre-net37-20261111-001219` was not used.

Sources (no binaries on the PR): `misc/appsvc_probe/` now also has `probe_prexpc.c` (macOS 13 arm64, libSystem only). Throwaway paths unchanged under `/var/mobile/appsvc-probe/` and `/var/mnt/rootfs/tmp/macws-appsvc-probe.xpc/` (byte-copied after signing on the mobile vnode).

### What Apple xpcproxy actually packs

Read-only copies of the live binaries (`/usr/libexec/xpcproxy` iOS 96 352 B, libxpc_executables-2462.120.4; `/var/mnt/rootfs/usr/libexec/xpcproxy` fat x86_64+arm64e, 2462.121.1) plus HookCase's xpcproxy notes and the x86_64 slice disassembly:

1. launchd `posix_spawn`s xpcproxy with two registered ports (bootstrap + a pipe back to launchd; HookCase, `OS_ALLOC_ONCE_KEY_LIBXPC`).
2. xpcproxy `xpc_pipe_routine`s launchd and gets a reply whose interesting keys include **`blob`**, **`ports`**, program / argv / environment, cwd, stdin/stdout/stderr, username / group, sandbox profile, container_id, `setup-event`, `uuid`, `XPC_FLAGS`, `Executable` / `Path`.
3. If `blob` is missing, xpcproxy logs **`No spawn attr blob`** (the string already cited in net17–net19). If it is present but shorter than **`0x104` (260) bytes**, it logs **`Insufficient spawnattr length: %zu < %lu`** (the `%lu` immediate is `0x104`). The first 0x104 bytes are a packed `posix_spawnattr`; the tail is applied together with the `ports` object (special / registered ports).
4. xpcproxy then `posix_spawn`s the service over itself with **`POSIX_SPAWN_SETEXEC`**, using that unpacked attr + env. The child's `__libxpc_initializer` is supposed to see a live `SERVICE_SLOT` receive and `XPC_FLAGS=8019` *because the kernel already applied that attr*, not because the service re-reads the launchd blob.

`__xpc_spawnattr_binprefs_pack/unpack` (libxpc TBD) is the CPU-architecture pref helper (`_xpc_binprefs_cpu_type` / `_xpc_binprefs_set_psattr`), not the launch-context blob itself. The packed Application-type context is the launchd reply (`blob` + `ports` + packed environment), applied by xpcproxy via SETEXEC.

### Probe design

The iOS first image still allocated a RECEIVE, `mach_ports_register`ed it in slot 2 (`typ=0x30000`, `recv_rights=1`, `register` kr=0), set `XPC_FLAGS=8019`, `bootstrap_register`ed `com.macwsguide.probe.appsvc.<pid>`, chrooted, and `execve`d a **new libSystem-only stub** (`macws-appsvc-prexpc`) so `__libxpc_initializer` would not run *in that image's link set*. The stub logged `mach_ports_lookup` / `getenv("XPC_FLAGS")` / inherited fds, re-set `XPC_FLAGS=8019`, then `posix_spawnattr_setflags(POSIX_SPAWN_SETEXEC)` + `posix_spawnattr_set_registered_ports_np` of the current slots and SETEXEC'd `macws-appsvc-probe` (stock `xpc_main`). Live net34 libmachook was still inserted; a macOS image still SIGTRAPs without it (§56). A real launchd `blob` was not transplanted: that requires launchd to emit one (xpcproxy's "No spawn attr blob" path is exactly the missing handshake).

### Result (01:34:01.139–01:34:02.276 CST / 09:34:01.139–09:34:02.276 PT, pid 86643)

Before `chroot`+`execve`, the first image again had a live slot-2 RECEIVE (`0x1c07 typ=0x30000`).

**In the libSystem-only stub, already at `main` (no libxpc in the stub's link set):**

```
environ flags=0x0 service=com.macwsguide.probe.appsvc dyld_insert=/usr/local/lib/libmachook.dylib
after-exec ports_lookup kr=0 count=3 [0]=0x807 typ=0x10000 [1]=0x0 typ=0x0 [2]=0xffffffff typ=0x100000 DEAD recv_rights=0
PREXPC_SERVICE_SLOT_SURVIVED=dead_name
inherited_fds 0 1 2
```

The receive right is already a dead name, and `XPC_FLAGS` is already the string `0x0`, *before* the test service that links libxpc is admitted. Inherited fds are only 0/1/2 (no packed fileports). So the §56 “libxpc unpacks after exec and overwrites environ” picture has to be narrowed: the destroy is not unique to the service binary linking libxpc. The live libmachook insert (still required for the macOS image to start) loads libmachook, which depends on libxpc, so `__libxpc_initializer` can still run as a constructor of that dependency *before* the stub's `main`. execve itself, that early libxpc/libmachook init, or both, leave slot 2 dead and flags `0x0`. This probe cannot separate those two without a pre-libxpc logger that also does not need libmachook, which this environment does not have (stock `/bin/echo` SIGTRAPs without the insert).

SETEXEC still ran (`set_registered_ports` rc=0; `errno=9` is a leftover from the fd scan, not a setter failure) and did admit the `xpc_main` image (same pid 86643). It could only hand over a dead name in slot 2. After that second image's constructors: `SERVICE_SLOT_SURVIVED=no`, `flags=0x0` again (the stub's `setenv("XPC_FLAGS","8019")` did not survive into the service `main`), stock `xpc_main` entered and SIGTRAP'd (`wait_rc=133` / signal 5) with no accept, no park, no `dispatch_sync_f(main)`, no reply. Client `bootstrap_look_up kr=1102`.

Re-applying 8019 and SETEXEC of a proper posix_spawnattr therefore cannot resurrect a right that is already a dead name at the first after-exec `main`. A homemade `mach_port_allocate` RECEIVE is still not a substitute for launchd's `blob`+`ports`.

### Implication for a later generic proxy fix

The missing system-level object is the launchd-issued spawnattr blob (≥ 0x104 bytes) plus the `ports` array that xpcproxy applies with SETEXEC — the same handshake iOS xpcproxy logs as **`No spawn attr blob`** on the WebKit first image (net17–net19). Until that blob is actually present (or the early destroy of a same-task RECEIVE is prevented *before* `__libxpc_initializer`), placing a RECEIVE in `SERVICE_SLOT` and setting `XPC_FLAGS=8019` in the exec environment will keep showing a dead name and `flags=0x0` at `main`. This turn did not invent a fake 0x104-byte blob (unsafe, version-specific, and too late once the right is already dead) and did not register a throwaway LaunchDaemon to ask launchd for a real one. No Safari hookup, no libmachook or proxy deploy. Device remains on net34.

Throwaway CDHashes (not part of the WebKit install): probe_proxy arm64e `ef4ac526de45efec711db200481420605efbe1fd`; macws-appsvc-prexpc arm64 `12f93b980395fd23d5da46b9a7a1ed6815c15837` (sha1 `e9b6f6a1b53688a434312ede6ea96b8a9204fc56`); macws-appsvc-probe arm64 `269746af25865ca98cff6d8c56189ceee97d39ac` (sha1 `ce0e0391054a526b18b3d64aee80d01dfc4fb096`).
