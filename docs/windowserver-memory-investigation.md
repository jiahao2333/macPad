# WindowServer 内存暴涨：代码调查（iPad14,5 / iPadOS 16.5）

> 范围：只读源码，没有碰设备。下面的 GB/MB 数字只引用 `docs/field-notes-ipad14-5-ipados16.5.md` 里已经记下的现象，不是这次测的。代码能说明的是“哪些路径有上界、谁负责释放”，说不清那次 12.7–18.1 GB 每一页归谁。行号对应本分支的 HEAD。
>
> 例外：“实测结论 2026-11-09”一节是设备实测，数字来自那次的 MEMDIAG 日志和 jetsam 报告。

## 现象（引用 field notes）

- 8 GB 的 iPad14,5。WindowServer 的 jetsam footprint 从约 12.7 GB 涨到约 18.1 GB，reason 是 `vm-compressor-space-shortage`。
- 早期 `task_info` RSS 只有一百多 MB。也就是说增长主要落在 compressor 和 footprint 记账里，而不是常驻页。
- plain-texture lease pool 峰值约 191 MB。整段会话 `MACWS-MEMORY-REAP` 一共只回收了几十到几百 MB。

## 实测结论 2026-11-09

> 来源：iPad14,5 上一次 `MACWS_MEMDIAG=1` 会话的 `WindowServer.err`（WindowServer pid 37803，10 秒一组采样）和随后的三份 `JetsamEvent-*.ips`。下表数字逐字摘自日志，没有换算。tick 10/11/18 是 memory-pressure 事件触发的额外采样。

### 趋势（`MACWS-MEMDIAG` 主行 + `caches` 行 + `metal-total` 行）

| tick | reason | phys_footprint | peak | internal | compressed | graphics_footprint | malloc_in_use | composite_stage_depth | owned_scanout | wscd_map | shadow_surfaces |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 1 | start | 19.4MB | 25.4MB | 18.8MB | 0.0MB | 0.0MB | 15.1MB | 0 | 0.0MB | 0 | 0 |
| 2 | timer | 21.2MB | 25.4MB | 20.4MB | 0.0MB | 0.0MB | 9.0MB | 0 | 0.0MB | 0 | 0 |
| 3 | timer | 342.4MB | 353.7MB | 39.0MB | 0.0MB | 13.1MB | 16.6MB | 4 | 21.5MB | 0 | 0 |
| 4 | timer | 431.0MB | 433.6MB | 88.4MB | 0.0MB | 32.3MB | 17.8MB | 18 | 21.5MB | 0 | 0 |
| 5 | timer | 443.2MB | 445.1MB | 88.4MB | 0.0MB | 30.3MB | 18.8MB | 27 | 21.5MB | 0 | 0 |
| 6 | timer | 540.4MB | 540.4MB | 91.8MB | 0.0MB | 34.8MB | 19.5MB | 37 | 21.5MB | 0 | 0 |
| 7 | timer | 2724.9MB | 2724.9MB | 61.6MB | 44.6MB | 39.7MB | 21.3MB | 88 | 25.8MB | 0 | 0 |
| 8 | timer | 9199.7MB | 9206.9MB | 46.1MB | 47.4MB | 33.9MB | 20.5MB | 319 | 25.8MB | 0 | 0 |
| 9 | timer | 9192.4MB | 9206.9MB | 45.5MB | 46.9MB | 26.7MB | 20.3MB | 319 | 25.8MB | 0 | 0 |
| 10 | pressure-critical | 9192.7MB | 9206.9MB | 45.9MB | 46.6MB | 26.7MB | 20.3MB | 320 | 25.8MB | 0 | 0 |
| 11 | pressure-normal | 9192.6MB | 9206.9MB | 46.0MB | 46.4MB | 26.7MB | 20.1MB | 320 | 25.8MB | 0 | 0 |
| 14 | timer | 9210.2MB | 9213.5MB | 64.4MB | 45.9MB | 35.7MB | 20.3MB | 329 | 25.8MB | 0 | 0 |
| 18 | pressure-warn | 9201.8MB | 9213.5MB | 63.9MB | 45.9MB | 33.2MB | 20.3MB | 329 | 25.8MB | 0 | 0 |

所有 tick 的 `composite_stage_contexts=1`、`composite_pending=0`、`shadow=0.0MB`、`disp_tracked=0`。

被 hook 的 Metal 资源（`metal-total`）在暴涨区间里一直只有几百 MB：

```
tick=6 nonlease_live=109 nonlease=202.5MB lease_live=64 lease=18.4MB plain_native=17/0.0MB iosurf=15/90.8MB iosurf_owned=4/86.0MB buffer=73/25.7MB heap=0/0.0MB tracked=669 freed=496 bucket_overflow=0
tick=7 nonlease_live=154 nonlease=218.8MB lease_live=102 lease=198.0MB plain_native=21/0.0MB iosurf=15/90.8MB iosurf_owned=7/99.0MB buffer=111/29.0MB heap=0/0.0MB tracked=1768 freed=1512 bucket_overflow=0
tick=8 nonlease_live=224 nonlease=312.3MB lease_live=107 lease=199.0MB plain_native=62/0.0MB iosurf=40/184.2MB iosurf_owned=7/99.0MB buffer=115/29.0MB heap=0/0.0MB tracked=4297 freed=3966 bucket_overflow=0
```

读法：
- `phys_footprint` 从 540.4MB（tick 6）到 9199.7MB（tick 8），同期能分项记账的部分（graphics ledger、malloc、compressed、被 hook 的 texture/buffer/heap）合计都在 500MB 以内。
- 唯一同步暴涨的是 `composite_stage_depth`：37 → 88 → 319 → 329。tick 7→8 多了 231 层、footprint 多了 6474.8MB，约 28MB/层；tick 6→7 多了 51 层、footprint 多了 2184.5MB，约 43MB/层。一个 2388×1668 的 pf550（8 字节/像素）目标是 31,866,336 字节 ≈ 30.4MB，量级吻合。
- `wscd_map=0`、`shadow_surfaces=0`：原 H1 里的 `g_wscd_tex` 和 H3 的 `shadowCache` 都被排除。
- 被钉住的是 IOSurface 后备内存，记在 IOKit/footprint 上，不进 `graphics_footprint`，MACWS-MEMORY-REAP 也看不到，所以每次回收只回收一点。

### Jetsam 报告（逐字）

| 报告 | timestamp | largestProcess | WindowServer rpages | lifetimeMax | 被杀进程数（reason） |
|---|---|---|---|---|---|
| JetsamEvent-181827.ips | 2026-11-09 18:18:27.00 +0800 | WindowServer | 588467 | 589239 | 683（vm-compressor-space-shortage） |
| JetsamEvent-181830.ips | 2026-11-09 18:18:30.00 +0800 | WindowServer | 588921 | 589661 | 155（vm-compressor-space-shortage） |
| JetsamEvent-181840.ips | 2026-11-09 18:18:40.00 +0800 | WindowServer | 588912 | 589661 | 134（vm-compressor-space-shortage） |

- `pageSize=16384`，所以 588912 页 = 9201.8MB，`lifetimeMax` 589661 页 = 9213.5MB，和 MEMDIAG 的 `peak=9213.5MB` 一致。
- 最后一份里 WindowServer 自己被杀：`"reason" : "vm-compressor-space-shortage"`，`killDelta` 6326549。
- 同一份里 rpages 第二名是 backboardd（3645 页），WindowServer 一个进程占了绝大部分。
- 时间换算：18:18:27 +0800 = 02:18:27 PST。

### 根因状态

- **确认**：`g_vnc_composite_stages` 只压不弹。
  - 压栈在 `macws_vnc_stage_composite`，从 `mac_hooks.m` 的三个 StartComposite hook（2483、2643、2694 行）调用，不受 VNC share 开关控制，`--no-vnc` 也会走。
  - 弹栈只发生在 hook 过的 `MetalContext::EndCurrentComposite(bool)`（`mac_hooks.m:2707` → `macws_vnc_complete_composite`）。
  - 最外层 `EndUpdate` 调的 `macws_vnc_finish_update` 以前只清 `g_vnc_composite_pending`，不清栈。
- **推断**：SkyLight 有一部分 composite 不经过被 hook 的 `EndCurrentComposite` 就结束了，可能是另一个结束入口、内联路径，或者 bail 后直接 pop `_state_stack`。具体是哪条路径**还不知道**，需要后续用 LLDB 或者给 StartComposite/EndCurrentComposite 计数配对来定位。

### 修复（默认开启）

`libmachook/Metal_hooks.x`：
- `macws_vnc_finish_update`（约 9989 行起）：在清 pending 的同一个 `@synchronized(g_vnc_composite_stages)` 里，把这个 MetalContext 的整个 stage 栈 `removeObjectForKey:` 掉，然后在锁外释放。数量累加到 `g_vnc_stages_drained_at_endupdate`。理由是最外层 EndUpdate 已经 Flush，这个 update 里的 composite 不可能还开着。
- `macws_vnc_stage_composite`（约 9617 行起）：每个 context 的栈硬上限，默认 8。超出时先丢最旧的一层，因为配对的 End 弹的是最新的那层。被丢的层在锁外释放，数量累加到 `g_vnc_stages_capped`。
- 新环境变量（`docs/runtime-switches.tsv` 登记为 production=off，`macws_diagnostic_flags.sh` 已用 `audit_runtime_switches.py --write-cleanup` 重新生成）：
  - `MACWS_VNC_STAGE_DRAIN=0`：A/B 回滚，恢复旧的无界行为（不 drain、不封顶）。不设就是修复开启。
  - `MACWS_VNC_STAGE_CAP=N`：1–4096 的十进制，覆盖默认上限 8；非法值仍用 8。
  - 两个值都只在进程里读一次。和 `MACWS_MEMDIAG` 一样，`production_preflight` 会拒绝 WindowServer plist 里出现它们，所以只能在 preflight 之后临时加，用完删掉。正常启动不需要设置任何变量。
- MEMDIAG 新增输出：
  - `caches` 行：`stages_drained_at_endupdate=`、`stages_capped=`、`stage_drain=`、`stage_cap=`。
  - 主行：`ledger_purgeable_nonvolatile=`、`ledger_purgeable_novolatile_compressed=`、`media_footprint=`、`media_footprint_compressed=`、`media_nofootprint=`、`neural_footprint=`、`neural_footprint_compressed=`、`neural_nofootprint=`、`vm_info_count=`。这些都是 TASK_VM_INFO REV3 的字段，内核返回的 count 小于 `TASK_VM_INFO_REV3_COUNT` 时打 0。

**验证预期**：开 MEMDIAG 跑同样的会话，`composite_stage_depth` 应该一直 ≤ 8，`stages_drained_at_endupdate` 持续增长，`phys_footprint` 不再随它线性涨。如果 `stages_capped` 也一直在涨，说明单个 update 内的泄漏就超过 8 层，值得继续追那条不走 EndCurrentComposite 的路径。

**要盯的回归**：macwsdisplayd 的 final-composite replay。这次日志里能看到 `#### FINAL-COMPOSITE replay-request ... freshness=source-fresh-replay-copy-completed published=YES sequence=8`。修复后要确认：
- `published=YES` 照常出现，sequence 继续递增；
- 不出现长时间的 `published=NO`、`source=unavailable` 或 `source-stale`；
- 画面不冻结、不黑屏。

原因是 pending 候选仍然只在 `macws_vnc_complete_composite` 里选，drain 只丢掉从来没被 End 配对的栈项。但如果 SkyLight 在 EndUpdate 之后才补 End，那一帧的候选就会丢。出现回归时先用 `MACWS_VNC_STAGE_DRAIN=0` 做 A/B 对照。

## 实测结论 2：IOSurface 释放 ABI 不匹配（根因，2026-11-09 晚）

> 来源：iPad14,5（iPadOS 16.5 20F66）上三次 `MACWS_MEMDIAG=1` 会话，打开 macOS 天气（Weather）作为触发。5 秒一组采样。数字逐字摘自 `WindowServer.err` 和 `trend.tsv`。

### 现象

装上 composite stage drain/cap（5933628）以后，stage 栈已经平了（`composite_stage_depth` 0–2，`stages_drained_at_endupdate` 持续增长），但打开天气后 WindowServer 仍然在 15–20 秒内从约 400 MB 涨到 3–4.5 GB。涨的全是 `ledger_purgeable_nonvolatile`（含 compressed），hook 到的 Metal、graphics、malloc 都很小。

### 诊断：`MACWS-PURGEDIAG`（`libmachook/Diagnostics/MacWSPurgeableDiag.m`）

跟 `MACWS_MEMDIAG=1` 一起开，只在 WindowServer 里生效，纯观察。MEMDIAG 的定时器同时从 10 秒改成 5 秒。每个 tick 输出：

```
#### MACWS-PURGEDIAG census tick= conn= id_off= live= nonvolatile=N/MB volatile=N/MB empty=N/MB query_failed=N/MB(last kr) size_unknown= own_nv= import_nv= ledger_nv=MB+MBc ledger_vol= untracked_nv= state_mismatch= overflow= unknown_release=
#### MACWS-PURGEDIAG origins kcreate=N/MBnv api=N/MBnv lookup=N/MBnv
#### MACWS-PURGEDIAG setpurgeable surf_kernel nv=ok/fail vol= empty= keep= old[...] api[...] gpu ... gpu_res create= fail= final=
#### MACWS-PURGEDIAG surf-sel <selector>=calls/fail ...        (IOSurface user client 的每个 selector)
#### MACWS-PURGEDIAG site live= bytes= nv= created= released= set[...] <5 帧 image+offset>   (前 10)
#### MACWS-PURGEDIAG setter ...                                 (IOSurfaceSetPurgeable 的调用点)
#### MACWS-PURGEDIAG fmt pf= w>= h>= live= bytes= nv=            (前 10)
#### MACWS-PURGEDIAG vmwalk nonvolatile=regions/MB(res MB) volatile=... empty=...
#### MACWS-PURGEDIAG vmwalk-tags (tag:state resMB) ...
```

数据来源：
- `IOSurfaceCreate_safe` 返回值（API 创建，带大小/格式/调用栈）；`IOConnectCallMethod_new` 里 IOSurface 连接的每个 selector 和 kr（连接按调用方镜像是 `IOSurface` 来识别，`IOObjectGetClass` 对 connect port 在这个内核上返回空串）；sel 0 回复里的 surfaceID 偏移在运行时用第一次 API 创建对出来（实测 `id_off=24`）；释放 adapter 的结果。
- 状态用 `IOSurfaceLookup` + `IOSurfaceSetPurgeable(kIOSurfacePurgeableKeepCurrent)` 查询，不会改变状态。
- `vmwalk` 用 `mach_vm_region_recurse` + `mach_vm_purgable_control(GET_STATE)` 遍历本进程映射的 purgeable 对象，按 VM tag 汇总（88 = IOSurface，100 = IOAccelerator）。

### 三次运行

| 运行 | libmachook | 打开天气前 phys | 天气后最高 phys | ledger_nv（+compressed） | vmwalk 映射 nonvolatile | 结果 |
|---|---|---|---|---|---|---|
| A 20:06 | 5933628 + 诊断 v1 | 427.6 MB | 3870.8 MB（6 秒内 577→2582→3870） | 1511.1 + 2227.6 MB | 861 区 / 4328 MB | 触发 3 GB 中止，回滚 memdiag |
| B 20:12 | + 诊断 v2（识别连接、5 帧调用栈） | 355.0 MB | 4571.0 MB | 1977.1 + 2480.5 MB | 866 区 / 4527.6 MB | 触发 3 GB 中止，回滚 memdiag |
| C 20:18 | + 释放 ABI 修复 | 326–330 MB | 524.7 MB（60 次点城市+滚动的压力操作期间），之后稳定在 418–423 MB（至 20:27:46） | 303.9 MB（+0） | 159 区 / 275 MB | 跑满监控窗口，无中止 |

关键证据：
- A：`SkyLight+0x2d44f8 < SkyLight+0x2d42a0`（`iosurface_create_common` ← `WSIOSurfaceCreateWithFormat`）在约 15 秒里创建了 461 个 surface，共 4064 MB，几乎全是 BGRA、宽高 1024–2047（每个约 9.7 MB，窗口大小的 backing store）。`setpurgeable` 一共只有 10 次调用，所以不是“该变 volatile 没变”，而是 surface 根本没被释放。
- B：`surf-sel ... 0=542/0 0x1=476/476`。WindowServer 发起的 476 次释放**全部失败**；诊断表随“释放”删了条目，所以 `live=74`，但 vmwalk 看到 866 个 IOSurface 区、4.5 GB 仍然映射着。
- C：`#### IOSURFACE-RELEASE-ABI probe surfaceID=40 sel=1 -> 0xe00002c2; native trap4 -> 0 trap5 -> 0; mode=native-traps`，之后 `0x1=…/0`（释放不再失败）。

### 根因

`mac_hooks.m` 的 `IOConnectTrap1_new` 把 macOS 13.4 `IOSurfaceClientRelease` 的 trap 对（`IOConnectTrap1(conn, 4, id)` / `IOConnectTrap1(conn, 5, id)`）改写成 iOS 16.3 的 `IOConnectCallMethod(conn, 1, &id, 1)`：吞掉 trap 4，把 trap 5 换成 selector 1。这是按 iPad13 / iOS 16.3（IOSurface DF041B53）做的。

iOS 16.5 不一样。从 iPad14,5 自己的 dyld cache 静态反汇编 `_IOSurfaceClientRelease`（`0x1a5782910 mov w1,#0x4 → IOConnectTrap1`，`0x1a5782974 mov w1,#0x5 → IOConnectTrap1`）：iOS 16.5 用的就是和 macOS 一样的 trap 对。iOS 16.5 内核对 selector 1 返回 `kIOReturnBadArgument`（0xe00002c2）。结果 WindowServer 创建的每一个 IOSurface 在内核里都不会被释放，内存一直挂在 WindowServer 的 owner ledger 上，默认状态 nonvolatile，所以表现为 `ledger_purgeable_nonvolatile` 暴涨。天气这种窗口内容频繁重建 backing store 的 app 会在几秒内放大这个泄漏。

同一次反汇编也确认 `IOSurfaceClientSetPurgeable` 两边 ABI 完全一样（selector 0x14，in = {surfaceID, state}，1 个 scalar out），H2 里“SetPurgeable selector 不匹配”的猜测不成立。

### 修复（默认开启，不需要环境变量）

`IOConnectTrap1_new` 改成按内核行为决定一次：
- 第一对释放仍然先走 selector 1。内核接受 → 记为 `mode=selector1`（iOS 16.3 行为不变）。
- 内核拒绝 → 对同一个 surface 补发原生 trap 4、trap 5。trap 5 成功 → 记为 `mode=native-traps`，之后所有 trap 对原样透传。
- 切换瞬间已经被吞掉 trap 4 的那一对，会在 trap 5 前补发 trap 4。
- 原生模式下 trap 失败会打 `#### IOSURFACE-RELEASE-ABI native trap%u failure`（前 16 次，之后每 500 次一行）；selector 1 模式的失败也改成 `dprintf`，前 16 次和每 500 次一行（以前用 `fprintf(stderr)`，在 WindowServer 里实测没有落到日志里）。

**要盯的回归**：`mode=` 那一行每次 WindowServer 启动应该只出现一次；不应该出现 `native trap… failure`；`surf-sel` 里 `0x1` 的 fail 应该是 0。iOS 16.3 设备上应该打 `mode=selector1`。

## 结论速览（按可能性排序）

> 下面是实测之前只读代码时的排序，保留作记录。实测结果见上一节：H1 里的 `g_wscd_tex` 被排除，真正的泄漏是 H1 第 3 项 `g_vnc_composite_stages`，当时只读代码判断它可能性低，判断错了。

| # | 假设 | 置信度 | 关键证据 |
|---|---|---|---|
| H1 | composite 交接里 libmachook 持有的 texture/retain 没放掉（`g_wscd_tex`、bail-pop slot），`--no-vnc` 时也会走 | 中 | `mac_hooks.m:2780`、`2622-2660`、`2578-2613` |
| H2 | 不归任何 libmachook pool 管的 GPU 资源（native early-return 纹理、全部 AGX buffer/heap），macOS 的 purge 机制在这台 iOS 内核上可能不生效 | 低–中 | `Metal_hooks.x:13150-13347`；没有 buffer/heap 回收代码 |
| H3 | `shadowCache` 只插入不删除，key 是原始 IOSurface 指针 | 低–中 | `Metal_hooks.x:12798`、`12850` |
| H4 | lease pool 在条目 busy 时超出预算 | 低（不是主因） | `Metal_hooks.x:13688`、`13724`；field notes 记的峰值约 191 MB |
| H5 | 小块 heap 泄漏（`[desc copy]`、REDIRECT `self`、没有 autorelease pool 的线程） | 低 | `Metal_hooks.x:13163`、`24166-24205`、`12580` |

**没有一条能单独被代码证明就是那 18 GB**，所以本分支没有加 mitigation，只加了诊断 `MACWS_MEMDIAG`（见后文）。要先用诊断把增长定位到一个桶，再决定改哪里。（更新：2026-11-09 实测已经定位到 composite stage 栈，修复见“实测结论 2026-11-09”。）

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
   - **2026-11-09 更新：实测证明这一项就是主泄漏**（深度到 329，footprint 9.2 GB），说明 SkyLight 并不总经过被 hook 的 `EndCurrentComposite`，上面基于断言的推理不成立。见“实测结论 2026-11-09”。
4. 被以上路径 retain 住的纹理，如果来自 lease pool，会一直处于 busy 状态，reaper 不会回收（`Metal_hooks.x:13565`）。这和“REAP 只回收几十 MB”的现象吻合，但不能证明就是这个原因。

**候选修复（stage 那条已实现，见“实测结论 2026-11-09”；设备上未验证）：**
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
#### MACWS-MEMDIAG tick=N reason=timer|pressure-* phys_footprint= peak= internal= compressed= resident= purgeable_volatile_resident= graphics_footprint= graphics_compressed= malloc_in_use= malloc_allocated= ledger_purgeable_nonvolatile= ledger_purgeable_novolatile_compressed= media_footprint= media_footprint_compressed= media_nofootprint= neural_footprint= neural_footprint_compressed= neural_nofootprint= vm_info_count=
#### MACWS-MEMDIAG caches owned_scanout= wscd_map= composite_stage_contexts= composite_stage_depth= composite_pending= shadow_surfaces= shadow= disp_tracked= stages_drained_at_endupdate= stages_capped= stage_drain= stage_cap=
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

1. ~~打开诊断跑一次会话~~（2026-11-09 已完成，定位到 composite stage 栈）。
2. 装上带 stage drain/cap 的 libmachook，开 MEMDIAG 复跑同样的会话，确认 `composite_stage_depth ≤ 8`、footprint 平稳、final-composite 仍然 `published=YES`。
3. 定位 SkyLight 不经过 `EndCurrentComposite` 就结束 composite 的那条路径，看能不能在源头配对。
4. ~~如果修复后 footprint 仍然在涨，再看新增的 purgeable/media/neural ledger，以及 H2（CA 的 purgeable 和 `SetPurgeable` 路径）。~~（2026-11-09 已完成：根因是 IOSurface 释放 ABI，见“实测结论 2”。SetPurgeable ABI 两边一致。）
5. 在 iOS 16.3 设备上确认打出 `mode=selector1`，且释放不失败。
6. 长时间会话（>1 小时、多个 app）复跑，看剩下的 `untracked_nv`（约 70–80 MB，主要是没走 API 创建的 surface 和 IOAccelerator）是否平稳。
