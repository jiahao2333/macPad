# WindowServer 内存暴涨：代码调查（iPad14,5 / iPadOS 16.5）

> 范围：只读源码，没有碰设备。下面的 GB/MB 数字只引用 `docs/field-notes-ipad14-5-ipados16.5.md` 里已经记下的现象，不是这次测的。代码能说明的是“哪些路径有上界、谁负责释放”，说不清那次 12.7–18.1 GB 每一页归谁。行号对应本分支的 HEAD。

## 现象（引用 field notes）

- 8 GB 的 iPad14,5。WindowServer 的 jetsam footprint 从约 12.7 GB 涨到约 18.1 GB，reason 是 `vm-compressor-space-shortage`。
- 早期 `task_info` RSS 只有一百多 MB。也就是说增长主要落在 compressor 和 footprint 记账里，而不是常驻页。
- plain-texture lease pool 峰值约 191 MB。整段会话 `MACWS-MEMORY-REAP` 一共只回收了几十到几百 MB。

## 结论速览（按可能性排序）

| # | 假设 | 置信度 | 关键证据 |
|---|---|---|---|
| H1 | composite 交接里 libmachook 持有的 texture/retain 没放掉（`g_wscd_tex`、bail-pop slot），`--no-vnc` 时也会走 | 中 | `mac_hooks.m:2780`、`2622-2660`、`2578-2613` |
| H2 | 不归任何 libmachook pool 管的 GPU 资源（native early-return 纹理、全部 AGX buffer/heap），macOS 的 purge 机制在这台 iOS 内核上可能不生效 | 低–中 | `Metal_hooks.x:13150-13347`；没有 buffer/heap 回收代码 |
| H3 | `shadowCache` 只插入不删除，key 是原始 IOSurface 指针 | 低–中 | `Metal_hooks.x:12798`、`12850` |
| H4 | lease pool 在条目 busy 时超出预算 | 低（不是主因） | `Metal_hooks.x:13688`、`13724`；field notes 记的峰值约 191 MB |
| H5 | 小块 heap 泄漏（`[desc copy]`、REDIRECT `self`、没有 autorelease pool 的线程） | 低 | `Metal_hooks.x:13163`、`24166-24205`、`12580` |

**没有一条能单独被代码证明就是那 18 GB**，所以本分支没有加 mitigation，只加了诊断 `MACWS_MEMDIAG`（见后文）。要先用诊断把增长定位到一个桶，再决定改哪里。

## H1：composite 交接路径（中）

libmachook hook 了 SkyLight 的 `StartComposite*`、`EndCurrentComposite` 和 `EndUpdate`。这些 hook 不受 `--no-vnc` 控制，常驻生效。

1. **`g_wscd_tex`**
   - `WSCompositeDestinationCreateWithMetalTexture` 每次成功，都会把 `wscd → texture` 存进字典并 retain 一次（`mac_hooks.m:2780`）。
   - 只有 `hooked_skylight_start_composite_wscd` 返回 `rv != 0` 时才把条目取走（`mac_hooks.m:2622-2660`，消费点在 2660）。
   - 注释里写，这里曾经让每个 pf550 IOSurface 多出一份 `0x13bc000` 字节的 type-0x82 资源（`mac_hooks.m:2772`、`2650`）。旁边的注释也承认 StartComposite 走 bail 路径是“the common case”（`mac_hooks.m:2542`）。
   - 后果：失败的 dest 条目会一直留在字典里，直到同一个指针被复用。如果 dest 指针一直在换新，这部分就没有上界。
2. **bail-pop 不释放 slot 里的引用**
   - `macws_pop_on_startcomp_bail` 只在 `rv == 0` 且 state stack 变大时运行（`mac_hooks.m:2578-2583`）。它在 `pop_back` 之前先把 0xb0 的 slot `memset` 成 0（`mac_hooks.m:2594-2613`）。
   - 注释写明，每次失败的 composite 最多有约 4 个 ObjC retain 不会释放。以前让 destructor 去 release 这些引用，第一次就 SIGSEGV 了。
   - 如果这些引用里有 render target texture，每次失败的 composite 都会多留一份。**不要**直接恢复 release，那条路径已有崩溃记录。
3. **`g_vnc_composite_stages`**
   - 每次 StartComposite 成功都会压栈（`Metal_hooks.x:9550`、`9560/9574`），只在 `EndCurrentComposite` 里弹出（`Metal_hooks.x:9578-9588`）。
   - `EndCurrentComposite` 和 `EndUpdate` 的 hook 在找不到符号时按 fallback offset 安装（`mac_hooks.m:~2880-2940`）。我在这台设备 rootfs 的 dyld cache 里核对过：SkyLight `__TEXT` 在 0x1850e5000，EndCurrentComposite 在 +0x14753c，EndUpdate 在 +0x1470b0，和 fallback offset 一致。
   - assert bypass 默认关闭（`mac_hooks.m:7639-7651`）。SkyLight 会在 `EndUpdate` 断言 `_state_stack.empty()`（`mac_hooks.m:2544`），composite 不配对就会 abort。所以这一项泄漏的可能性**低**，列出来只是因为它也是只靠配对来释放的 retain。
4. 被以上路径 retain 住的纹理，如果来自 lease pool，会一直处于 busy 状态，reaper 不会回收（`Metal_hooks.x:13565`）。这和“REAP 只回收几十 MB”的现象吻合，但不能证明就是这个原因。

**候选修复（未实现，UNTESTED ON DEVICE）：**
- 在最外层 `EndUpdate` 把该 context 剩下的 stage 条目清掉。SkyLight 自己保证那时 state stack 为空，所以这样做是安全的。
- 给 `g_wscd_tex` 加老化：按代数或时间淘汰，或者在 dest 释放时清理。

看诊断时盯 `wscd_map=` 和 `composite_stage_depth=`。

## H2：pool 以外的 GPU 资源（低–中）

- plain hook 里，compressed、mip、memoryless、depth、2DArray、非 2D 这几类纹理在进 lease pool 之前就 return 了，生命周期完全交给调用方（`Metal_hooks.x:13150`、`13185`、`13229`、`13263`、`13303`、`13347`）。
- 旧注释记过 memoryless 被错误路由成 IOSurface、累计“46+ GB”的事故（`Metal_hooks.x:13129`）。当前代码在 `storageMode == 3` 时走 native。
- AGX 的 `newBufferWithLength:options:` 和 `newHeap*` 在 WindowServer 里没有任何 libmachook 记账或回收。
- 增长主要落在 compressor，和“CA/SkyLight 持有大量本来该是 purgeable 的资源，但在这里没被 purge”相容。代码里没看到 IOSurface purgeable 的处理。`SetPurgeable` 只做了 0xd→0xc 的翻译（`mac_hooks.m:17236`）。

诊断看 `graphics_footprint=`、`purgeable_volatile_resident=`，以及 `kind=plain-native|buffer|heap` 这几个桶。

## H3：`shadowCache`（低–中）

- `hooked_newTextureWithDescriptor:iosurface:plane:` 在 owned-scanout wrap 失败、且 FourCC 是 `0x26xxxxxx` 时，会创建一张全屏 shadow IOSurface。这张 surface 按原始指针存进 `shadowCache`（`Metal_hooks.x:12798`、`12850`）。
- 整个文件没有对 `shadowCache` 的删除。
- 原始 surface 指针如果一直在换，每个指针都会永久留一张。看诊断里的 `shadow_surfaces=`，也可以 grep 已有 log 里的 `MTL_TEX/iosurf SHADOW alloc'd`。

## H4：lease pool 超预算（低）

- idle 回收的预算是 64 MB（`Metal_hooks.x:13480`）。分配时 LRU 的预算是 256 MB（`Metal_hooks.x:13688`），只淘汰 retain count 回到 baseline 的条目（`Metal_hooks.x:13641`）。busy 条目可以超出预算，这时会打 `LEASE-OVER-BUDGET`（`Metal_hooks.x:13724`）。
- field notes 里 pool 的峰值约 191 MB，所以这次的 GB 级增长不在这个 pool 的记账里。

## H5：小泄漏（低）

- `[desc copy]` 在 compressed 路由里没有 release（`Metal_hooks.x:13163`）。
- AGXBuffer REDIRECT 返回 `ios_buf` 时没有释放 `self`，而且提前调用了 deallocator（`Metal_hooks.x:24166-24205`、`24176`）。
- MANAGED-BOUNDARY 在可能没有 autorelease pool 的线程上调用 `autorelease`（`Metal_hooks.x:12580`）。
- 看 `malloc_in_use=` 的趋势。

## 排除或有上界的部分

- final composite snapshot slot 固定 4 个（`MacWSFinalCompositePublisher.m:64`）。slot 被占满时返回 NULL，不会新分配（`MacWSFinalCompositePublisher.m:378/385`）。
- owned scanout 有 128 MB 的 LRU（`Metal_hooks.x:12353`）。
- `macwsallocd` 的 mlock 内存记在 allocd 进程上，不算进 WindowServer（`macwsallocd/main.m:153`、`184`）。而且 `macws_make_mem_entry_xpc` 没有调用方（`mac_hooks.m:1611`）。
- `g_dispTexs` 只在 disp mode 的 sentinel 打开时才追加（`Metal_hooks.x:6373`、`10172`）。
- AGX_LIFE 表最多 16384 个元数据槽，不持有 GPU 对象（`mac_hooks.m:17349`）。

## 诊断：`MACWS_MEMDIAG=1`

默认关闭。进程启动时只检查一次 `getenv("MACWS_MEMDIAG")`，值严格等于 `"1"` 才打开，结果原子缓存（`Metal_hooks.x:~23216`）。关闭时不装 hook、不建 timer，行为和以前完全一样。

打开后：
- 用 `MSHookMessageEx` 在 `AGXG13GFamilyDevice` 上包一层 wrapper，覆盖下面几个 selector：
  - `newTextureWithDescriptor:`、`newTextureWithDescriptor:iosurface:plane:`
  - `newBufferWithLength:options:`、`newBufferWithBytes:length:options:`、`newBufferWithBytesNoCopy:length:options:deallocator:`
  - `newHeapWithDescriptor:`

  安装点紧跟在 AGX native swizzle 之后（`Metal_hooks.x:~24078`）。
- 释放计数靠一个 associated `dispatch_data`：对象释放时它的 destructor 会减计数，所以不需要新增 ObjC 类，arm64e 也没有 PAC 问题。
- AGX_LIFE 也会一起打开（`mac_hooks.m:~17412`）。

所有输出都在 `/var/jb/var/mobile/WindowServer.err`，每 10 秒打一组，收到 memory pressure 事件时也打一组：

```
#### MACWS-MEMDIAG tick=N reason=timer|pressure-* phys_footprint= peak= internal= compressed= resident= purgeable_volatile_resident= graphics_footprint= graphics_compressed= malloc_in_use= malloc_allocated=
#### MACWS-MEMDIAG caches owned_scanout= wscd_map= composite_stage_contexts= composite_stage_depth= composite_pending= shadow_surfaces= shadow= disp_tracked=
#### MACWS-MEMDIAG agx_life live=... full=...
#### MACWS-MEMDIAG metal-total nonlease_live= nonlease= lease_live= ... tracked= freed= bucket_overflow=
#### MACWS-MEMDIAG metal kind= storage= pf= live= bytes= created=     (前 24 个桶)
```

怎么读：

| 看到的趋势 | 指向 |
|---|---|
| `graphics_footprint` 和 `agx_life live` 一起涨 | GPU 资源泄漏，再看 `metal kind=` 是哪一类 |
| `wscd_map` 或 `composite_stage_depth` 涨 | H1 |
| `shadow_surfaces` 涨 | H3 |
| `kind=plain-native|buffer|heap` 涨 | H2 |
| `malloc_in_use` 涨 | heap 泄漏（H5） |
| `phys_footprint` 涨，但以上都平 | 增长在 hook 不到的地方（CA 内部、非 Metal 的 IOSurface 等） |
| `agx_life full>0` | 表满，`live` 只是下限 |

**怎么打开（注意 preflight）：**
- `macos_gui.sh` 的 `production_preflight` 发现 WindowServer plist 里有任何 production=off 的诊断变量，就会报错退出（`layout/usr/macOS/bin/macos_gui.sh:3317-3338`），名单来自 `macws_diagnostic_flags.sh`。本分支已经按惯例把 `MACWS_MEMDIAG` 加进了那张名单。
- 所以要在 preflight **之后**再把 `MACWS_MEMDIAG=1` 写进 `/var/jb/usr/macOS/LaunchDaemons/com.apple.WindowServer.plist` 的 `EnvironmentVariables`，然后 `launchctl unload` / `load` 这个 job。`kickstart -k` 不会刷新环境变量。这一步会重启 WindowServer，需要用户批准，也要先装好新编译的 libmachook。
- 用完把这个 key 删掉，否则下次 preflight 会拒绝启动。

**盲区：**
- 从 heap 或 buffer 上创建的子纹理不计数。
- arm64e on-device 构建里，AGX swizzle 这一段在 `#if` 外面，诊断 hook 不会安装，只有 ledger 那几行有效。
- `bytes` 取的是 `-allocatedSize`，多张纹理共享同一块 IOSurface 时会重复计算。

## 下一步

1. 打开诊断跑一次会话，记下 footprint 上涨期间哪一行在跟着涨。
2. 如果是 H1，实现上面的 `EndUpdate` 清理和 `g_wscd_tex` 老化，放在单独的 flag 后面，并在设备上验证。
3. 如果是 H2，查 CA 的 purgeable 和 `SetPurgeable` 路径。
