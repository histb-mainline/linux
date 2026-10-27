# mv100 移植驱动 —— 上游化重写（clean worktree）

工作树：`/srv/blob/histb/linux-mv100-clean`，分支 `mv100-upstream`，基线 `upstream/master`（v7.3-rc5）。
起点：`linux-mv100` 的 `mv100` 分支（该分支上 HDMI/VO 部分按要求**排除**）。
范围：只重写 `mv100` 移植里的**驱动**；`mach-hisi`（SMP/IO map）与 `arch/arm/boot/dts` 属板级支持、非驱动，见文末「未纳入」。

## 提交序列（发送顺序，共 8 个提交 / 4 个可独立发送的 series）

按发送分组；分支上自底向上顺序即下面编号，保证每个提交都能独立编译（后一个只新增文件或改 Kconfig，不回改前一个已编译的对象）。

**Series A — net: phy**（1 patch）
1. `net: phy: add support for HiSilicon Festa PHYs` — `drivers/net/phy/hisi-festa.c` + Kconfig/Makefile + MAINTAINERS。

**Series B — regulator**（2 patch）
2. `regulator: dt-bindings: add hi3798cv200 regulator` — `hisilicon,hi3798cv200-regulator.yaml`。
3. `regulator: histb: add voltage regulator driver` — `histb-regulator.c` + Kconfig/Makefile + MAINTAINERS。

**Series C — clk**（3 patch）
4. `dt-bindings: clock: convert hisi-crg.txt to YAML` — 删 `hisi-crg.txt`，加 `hisilicon,hisi-crg.yaml`。
5. `dt-bindings: clock: add Hi3798MV100 CRG` — yaml 增列 mv100 + 新增 `include/dt-bindings/clock/hisilicon,hi3798mv100-{crg,sysctrl}.h`。
6. `clk: hisilicon: add CRG driver for Hi3798MV100 SoC` — `crg-hi3798mv100.c` + Kconfig/Makefile + MAINTAINERS。

**Series D — dmaengine**（2 patch）
7. `dt-bindings: dma: add HiSilicon Hi3798MV100 DMA controller` — 现文件 `hisilicon,hi3798mv100-dma.yaml`。改名史：2026-10-17 由 `hisilicon,hi3798mv100-dmac.yaml` 改成 `hisilicon,hi3798mv100-dma.yaml`（尾巴 `-dmac`→`-dma`，理由＝上游 `-dma` 是主流尾巴且同厂商既有串 `hisilicon,k3-dma-1.0`、`hisilicon,hisi-pcm-asp-dma-1.0` 都用 `-dma`，`-dmac` 在上游几乎只有 Renesas 用）；2026-10-18 按用户指令"dma 改为 histb"改成家族级文件名与串 `hisilicon,histb-dma`；**同日按用户指令"所有 compatible 全部降到芯片级"改回 `hisilicon,hi3798mv100-dma`**（代码侧 `drivers/dma/histb-dma.c`、`CONFIG_DMA_HISTB`、标识符仍是家族名）。提交标题里的 "Hi3798MV100" 因此重新与串一致，那条 reword 待办消失。
8. `dmaengine: add HiSilicon STB SoCs DMA controller driver` — 现文件 `histb-dma.c` + Kconfig/Makefile + MAINTAINERS。改名史：2026-10-17 降为芯片级（`histb_dma.c`→`hi3798mv100-dma.c`、`HISTB_DMA`→`HI3798MV100_DMA`、`histb_dma_*`→`hi3798mv100_dma_*`）；2026-10-18 按用户指令改回家族级（→`histb-dma.c`、`DMA_HISTB`、`histb_dma_*`，`.name`/`MODULE_DESCRIPTION`/binding title 同步；Kconfig 与 Makefile 的条目随之按字母序从 `HISI_DMA` 前挪进 `DMA_*` 组）。提交标题与正文里的 "STB SoCs" 字样与本轮一致，不用改。

依赖：C 系（clk）是 D 系（dma）binding example 的 `HI3798MV100_DMAC_CLK` 宏来源；A/B/D 相互独立。跨树投递时 clk 走 clk 树，phy 走 net，regulator 走 regulator 树，dma 走 dmaengine 树；binding patch 与对应驱动 patch 同一封 series 内一起送。

## 已跑的门（本 worktree 实测，非中间产物）

- **编译**：`make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- O=/tmp/obj-mv100 W=1`，multi_v7 基座 + 逐个打开 4 个新 CONFIG，四个 `.o` 全部干净通过，无告警。
- **checkpatch --strict**（逐 patch，`--no-signoff`）：8 个 patch 全部 **0 error / 0 warning / 0 check**；唯一剩下的是 binding/driver patch 的 `MAINTAINERS need updating?` 提示（对应 driver patch 已带 MAINTAINERS 条目，属误报）。
- **dt_binding_check**：`hisilicon,hisi-crg.yaml`、`hisilicon,histb-volt.yaml`、`hisilicon,hi3798mv100-dmac.yaml` 三份分别单独跑均 **clean**（含 yamllint/chkdt/style/example DTEX）。本机有 `yamllint` + `dtschema 2026.6`，门是真跑过的，不是静默跳过。**该记录对应 2026-10-17 之前的文件名**：binding 后经 `hisilicon,hi3798mv100-dma.yaml`、再到 `hisilicon,histb-dma.yaml`，这一门对新名重跑过（见下方"2026-10-18 家族级改名"节）。
- **dtbs_check（arm32 存量用户）**：CRG txt→yaml 转换后，`make dtbs_check DT_SCHEMA_FILES=...hisi-crg.yaml` 对 arm32 全部 dtb（hi3519-crg/sysctrl、hi3516cv300 分支）exit 0，转换不破坏既有板子。cv200 是 arm64，本机无 aarch64 工具链，未跑（见下）。
- **get_maintainer.pl**：4 个 driver patch 分别落到 ETHERNET PHY LIBRARY / VOLTAGE REGULATOR / COMMON CLK / DMA GENERIC OFFLOAD，且自定义的三条 HISTB MAINTAINERS 条目均被解析到 M: David Yang。

## 相对 `mv100` 分支的实质改动（不是搬运，是按上游标准重写）

clk CRG：
- 时钟 ID 从共享 `histb-clock.h`（cv200 语义）迁到**每 SoC 独立头** `hisilicon,hi3798mv100-{crg,sysctrl}.h`，命名 `HI3798MV100_*`，对齐 `wip/hi3798mv2x` 里 Yang Xiwen 已评审的 mv200 惯例。
- 内部父时钟（fixed/mux）不再塞进 DT 可见 ID 空间：改用 `HI3798MV100_CRG_CLK_COUNT` 之后的匿名 enum。
- **裁剪到 mv100 实际有的时钟**：删掉从 cv200 抄来的 USB3/COMBPHY/GPU/PCIe/FMC/NAND/SDIO0 相位等无消费者或 mv100 不存在的项（SDK 侧 `hi_reg_crg.h`/vendor 4.4 dts 为证：mv100 无 COMBPHY，GPU 门在 CRG53 但 vendor 从不写、其 DT 时钟实为 CRG73）。
- 修值：`83p3m` 从 83333333 改回 vendor 约定 83300000（`CONFIG_PL011_CLOCK`、dts `clk_83p3m`）；删掉 mv100 上根本不存在的 `166p5m`（且其值 165000000 与名 166.5M 自相矛盾，是从 cv200 抄漏）。
- binding：`hisi-crg.txt` → YAML（新增 mv100 compatible）；Kconfig 补 `select COMMON_CLK_HISI`（原分支只 select RESET_HISI，靠传递依赖才编上）。
- CRG51「以太网」仍按分支保留为**位组时钟**（bus 门 + MAC 门 + 未文档化的 clk_sel[3:2] 一体使能，值 0xb），依据：vendor 使能写的正是 mask 0xb/value 0xb，且唯一消费者 `hisi_femac` 只 enable 不取速率——拆成 mux+gates 反而要凭空捏造 sel=2 的速率。

regulator：修 `devm_platform_ioremap_resource` 失败被吞成 `-ENOMEM`（改 `PTR_ERR`）；get_voltage 的 `(max-min)*duty` 在 duty≈4094 时 int 溢出，改 `u64` + `DIV_ROUND_CLOSEST_ULL`；probe 校验 `min_uV < max_uV`（否则 set 路径除零）；`FIELD_GET/PREP` 取代裸位移与 `0x00010002/0x00020002` 魔数；补 of 表 sentinel、`MODULE_AUTHOR`。
- **未采用**「改写成 pwm-core + 复用 `pwm-regulator`」：那会强制把开关周期固定（pwm 核需要一个确定 period），而现有驱动用 `rational_best_approximation` 让周期随占空比反约，直接改等于在**不上机**的前提下改模拟环路的开关频率——保留原算法更安全。此为可与 Mark Brown 讨论的设计点，留待 v2。

phy：`__phy_*` 返回值全链路传播（原 bulk 写忽略返回）；`fw->size<2` 前置校验（原 `fw->data[1]` 可越界）；`request_firmware`→`firmware_request_nowarn` 并区分「无镜像（-ENOENT，非错，PHY 跑自带码）」与「下载失败（真错）」；补 `lockdep_assert_held(mdio_lock)`。

dma：见下节。

## DMA（`histb-dma.c`，原名 `histb_dma.c`）：已修 vs 仍待办

对分支文件做了完整审查（子代理 + 自行逐行 file:line 复核）。**已改**（均可静态判定、无板子也能确证、且不改硬件时序）：
- `histb_dma_desc_free` 部分分配回滚越界（`nitems` 之前一步到位设成 n，kcalloc 失败即 NULL 解引用；pool 分配中途失败会把未分配项喂给 `dma_pool_free`）→ 现按成功计数递增 `nitems`。
- `sg_len==0`/`nsegs==0` 时 `kcalloc(0)` 返回 `ZERO_SIZE_PTR` 后 `segs[0]` 解引用 → 入口 `if (!nsegs) goto err_free`。
- debug 转储 `width[]` 只有 4 项却被 3-bit（0..7）域索引 → 补到 8 项。
- `device_pause/resume` 对 `DMAC_CHAN_CFG` 做 RMW 却不持 `vchan.lock`（与 chan_start/halt/IRQ 竞争）→ 加锁，且无活动 desc 时 `-EPERM`。
- 缺 `provider.rst` 必填字段：补 `src/dst_addr_widths`、`residue_granularity=DESCRIPTOR`、`max_burst=256`；residue 改走 `dma_set_residue()`。
- probe 生命周期：`dmaenginem_async_device_register` 之后又手动 `dma_async_device_unregister`（of 注册失败路径双注销→二次 `ida_free`/`free_percpu`/kref 下溢）→ 改显式 `dma_async_device_register`；runtime PM **enable 提前到注册之前**（否则 client 抢通道时 `pm_runtime_resume_and_get` 撞 -EACCES）；`remove()` 改为在仍有时钟时 `of_dma_controller_free → devm_free_irq → dma_async_device_unregister → 逐通道 tasklet_kill →` 再 PM/clk/reset 收尾（原顺序在复位/断钟后仍把 4 通道暴露、且从不调 tasklet_kill）。删重复的 `dev_set_drvdata`。
- binding example 的 `HISTB_DMAC_CLK`（目标树 `histb-clock.h` 无此宏）→ 换成 mv100 CRG 头的 `HI3798MV100_DMAC_CLK`（否则 `dt_binding_check` cpp 直接挂）。

**仍未改、发送前必须处理（需要板上时序/错误注入验证，本会话不上机故不动）**：
1. `histb_dma_terminate_all` 在 `spin_lock_irqsave` 下 `readl_poll_timeout_atomic(...,USEC_PER_SEC)`——关中断忙等最长 1s；`device_terminate_all` 契约允许异步、不必等停。应把等待搬到 `device_synchronize`（可用睡眠版）。
2. 出错分支（IRQ handler 内）调 `histb_dma_chan_halt()`（同样 ≤1s 忙等）+ `dev_err`，每错误通道一次，共享线最坏数秒关中断。应改 threaded IRQ。
3. 出错描述符不向 client 回报结果：只 `vchan_terminate_vdesc` 不 `vchan_cookie_complete`，client `complete()` 永久等；且用**通道级** `chan->error` 位，之后任何在跑传输都被 `tx_status` 误报 `DMA_ERROR`。应改 per-desc `vdesc.tx_result`（`DMA_TRANS_*_FAILED`）并仍走 cookie_complete。
4. `histb_dma_suspend` 用 `pm_runtime_force_suspend` 会强制走 runtime_suspend 无条件清 4 通道 EN——串口 RX 这类常开通道在系统挂起时会在传输中途被掐。应遍历通道，有活动 desc 返回 `-EBUSY`。
5. `DMAC_CFG_M1/M2` 注释写「big endian」，代码却同时置两 BE 位又用 `cpu_to_le32` 写 LLI（自相矛盾，SDK/vendor 树里查无此 DMAC 原厂源码，语义未证）。发前应查 TRM 或上板 memcpy 比对首 4 字节，至少改正注释。
6. `copy_align=ALIGN_4` 与 memcpy/memset 的 `tail8`（8-bit 尾节点补齐）矛盾：守 copy_align 的 client 永不触发 tail 分支，且不校验地址对齐。要么删 tail8、要么真做地址对齐（head/body/tail）。
7. `dma_addr_t` 到 32-bit 寄存器/LLI 的静默截断（现目标无 LPAE 不触发，但应 `dma_set_mask_and_coherent(DMA_BIT_MASK(32))` + `upper_32_bits` 检查）。
8. 可选：`device_prep_peripheral_dma_vec`、`device_release`、`dma-requests` binding 属性、17 个未引用寄存器宏。

结论：DMA 两 patch 现在**编译+checkpatch+binding 全绿**，可作 RFC 发；但 (1)–(4) 是并发/生命周期正确性问题，**没上板验证前不建议标 non-RFC**。

### 2026-10-24 板测：这块 DMAC 自报 ARM PrimeCell ID（EC6108V9C，`fastboot#` 下 `md`/`mw`）

- 基址换算与手册一致：DMAC 窗口 `0xF987_0000–0xF987_FFFF`（64K，ID 段在窗口内）；DTS 经 `soc@f0000000` + `ranges = <0 0xf0000000 0x10000000>` 把 `reg = <0x9870000>` 落到 CPU 物理 `0xf987_0000`，所以 `md`/`/dev/mem` 一律用 `0xf987_xxxx`。
- 接手时的上电初始态：`PERI_CRG41`（`0xf8a220a4`）= **`0x00000010`** ⇒ `dmac_cken`(bit0)=0（时钟门掉）且 `dmac_srst_req`(bit4)=1（**被按在复位里**）。手册给的复位值 `0x0000_0001` 只成立在硬件复位那一拍，厂商 boot 阶段把它改成门关＋复位保持。
- 该状态下整窗读数一律 `0x0000_0000`（含 `0x30` 与 `0xFE0`），既不挂总线也不报错，但**不构成"ID 段不存在"的证据**。
- 写 `PERI_CRG41 = 0x1`（开时钟＋放复位）后 ID 段解出并逐字读回：

| 偏移 | 读回 | 字段 |
|---|---|---|
| 0xFE0 | `0x0000_0080` | PeriphID0 = 0x80 ⇒ ARM PrimeCell **PL080** 的件号 |
| 0xFE4 | `0x0000_0010` | PeriphID1 = 0x10 |
| 0xFE8 | `0x0000_0014` | PeriphID2 = 0x14 ⇒ 版本 1.4 |
| 0xFEC | `0x0000_000A` | PeriphID3 = 0x0A ⇒ custom 半字节 0xA（ARM 原件为 0x00）|
| 0xFF0–0xFFC | `0x0d`/`0xf0`/`0x05`/`0xb1` | 标准 PrimeCell Cell ID `0xB1F0050D` |

⇒ 32 位 periphid = **`0x0A141080`**。

- 与上游 `pl08x_ids[]`（`drivers/dma/amba-pl08x.c:3020`）对照：PL080 那条 `id=0x00041080 / mask=0x000fffff` **不匹配**（本块 PeriphID2 是 0x14 而非 0x04，该字节在掩码内）；三星 PL080S 那条 `id=0x0a141080 / mask=0xffffffff` **逐位全等**。即 periphid 本身消歧不了：若把节点改成 `arm,primecell` 交给 AMBA 总线枚举，本块会被认成 `vendor_pl080s`，从而取到错误的 CH_CONFIG 偏移（0x14）、8 词 LLI、"不支持外设流控"、传输上限 2^25−1；而本块实测形状是 plain PL080——CH_CONFIG 在通道偏移 0x10（手册表 3-48；`mw.l 0xf9870110 0xc000` 读回 `0xc000`，紧邻 0x114/0x118 保持 0）、4 词 LLI 链式（cv200 板证）、支持外设流控、单节点上限 4095 拍。
- 收尾：`0xf9870110←0`、`0xf9870030←0`、`0xf8a220a4←0x10` 全部写回并读回核对；板子留在 `fastboot#`，与接手时同态。
- 对投稿的影响：「寄存器逐位等于 PL080」现在有了硬件自报件号（PartID 0x80）支撑，所以"为什么不用 amba-pl08x"必须正面回答。可用理由：与三星变体 ID 重号、AMBA 枚举会绑错 `vendor_data`；本块还需 CRG 门控＋复位与 `dma-channels`（上游通道数硬编码 8/2）；`AMBA_PL08X` 是 `bool` 且 `depends on ARM_AMBA`，mv100 的 `ARCH_HISI` 未 select。另一条可选路线＝给 amba-pl08x 加一条 Hisilicon `vendor_data`，靠 compatible 而非 periphid 消歧，并让它支持 platform 设备。

### 2026-10-24 上机 A/B/C：把节点直接换成 `arm,pl080` 跑 dmatest（构建 `#128`，同一 zImage 换 dtb）

台架：RAM tftp（`h001/h002/h003/h004.img`，uImage 9.26 MB，未触 16 MB 红线），身份靠 `Image Name` ＋ cmdline `pl08xarm=` ＋ `uname -v`；dmatest 参数一律 `iterations/norandom=1/timeout=10/max_channels=4/device=f9870000.dma-controller`；本轮把 `power/control` 钉成 `on`（pl08x 自己不调 `pm_runtime`，而 amba 核心 probe 后会 runtime-suspend 掉 `apb_pclk`；`auto` 态未测）。

| 臂 | 驱动与形状 | dmatest 读数 |
|---|---|---|
| A | 上游原样＋真实 periphid（内核自读得 `0a141080`）⇒ 绑 `vendor_pl080s`，横幅 `DMA: PL080s rev1`，8 memcpy＋32 slave | 1024 B×4 通道各 `20 tests, 20 failures`；前 5 次 `test timed out`，之后 `prep error` ＋ `pl08x_fill_llis_for_desc no memory for llis`（超时描述符不还池子把池耗干）；`/proc/interrupts` 里 `GIC-0 62 pl08xdmac` **计数 0** ⇒ 硬件全程没跑 |
| B | 上游＋一条本颗 `vendor_data`（channels=4、`config_offset=PL080_CH_CONFIG`、4 词 LLI、上限 4095 拍），横幅 `DMA: PL080 rev1`，4 memcpy＋16 slave | 1024 B：4 通道 `20 tests, 0 failures`，1.6–1.9 MB/s（DT 未给 `memcpy-bus-width` ⇒ 默认 8 位单拍） |
| C | 控制组：同一 zImage 换回 `hisilicon,hi3798mv100-dma`（histb-dma） | 1024 B：`20 tests, 0 failures`，4.1–4.8 MB/s；65536 B（5 节点）：`4 tests, 4 failures` ＋ `channel N: cannot halt transfer`，其中一次部分拷贝停在 `dstbuf[0x401a]`≈16410 字节＝**第一个节点之后不再前进** |
| B2 | B 的驱动，width=1 链深梯子（每节点 4095 B） | 4095/12285/16380/20475/24570 B（1/3/4/5/6 节点）各 `2 tests, 0 failures` 全过；**28665 B（7 节点）首次失败** |
| B3 | B 的驱动＋ `memcpy-bus-width = <4>`（每节点 16380 B） | 16380 B（1 节点）`2/0 failures`；24576 B：3 通道 0 失败＋1 通道 2/2；28672 B：1 通道 2/2；32760 B：3 通道 2/2（**间歇**，失败样本里 TC 中断不涨、无 `not copied`） |

由此：

1. **A 的失败是重号的直接后果，不是"PL080 语义不对"**：`vendor_pl080s` 把通道配置写到偏移 0x14（本块 `0x114` 不解码，见上一节），通道永远拿不到 EN ⇒ 0 中断。同一次启动里 `cat /sys/bus/amba/devices/*/id` = `00041061`×4（gpio）、`00141805`（sp805）、`00241011`（pl011）、`0a141080`（dmac）⇒ 内核 AMBA 核心自己完成了"取时钟＋解复位＋读 ID"（`amba_read_periphid()` 里有 `of_reset_control_array_get_optional_shared` ＋ `reset_control_deassert`），所以 DT 只要带 `clocks`/`clock-names = "apb_pclk"`/`resets`，就能在本颗"boot 门关＋按住复位"的块上把 ID 读出来，不需要在 bootloader 里手工开门。
2. **B 证明上游驱动能驱动这块硬件**（单节点 20/0），且它的两处写法在本块成立：LLI 词里放**字节地址原样**、CONFIG 用非 RMW 只写 ENABLE（M1/M2 保持 0）。对照 C 在 5 节点链上"第一节点后就停"，本分支 `histb-dma.c:376` 的 `(addr >> 2) | DMAC_CHAN_LLI_MST` 确实是缺陷（cv200 的修正 `e17457cff43d` 没带过来）。
3. **但 mv100 的多节点链有一层与驱动无关的失败面**：上游编码下 width=1 到 6 节点/24.5 KB 全过、7 节点起挂；width=4 时 2 节点就间歇挂。⇒ cv200 那次 5 节点/64 KB 通过（52821 tests/0 failures）**不能外推到 mv100**。mv100 链式还欠一次大样本、且要按"节点数"与"总字节"两种口径分别界定（本轮每档只有 2 迭代×4 通道＝8 样本，间歇失败的比例估不准）。

### 2026-10-24 追加：链深边界与"LLI 走哪个 master"（arm B2 vs arm D）〔**本节两条边界结论与下条"追加二"全部作废**，见"追加三"：量的是我自己设的 `timeout=10` 毫秒〕

同一颗 zImage（含 B 臂的 scratch `vendor_data`、width 默认 8 位、每节点 4095 字节）只在 dtb 上分叉，梯子脚本相同（`iterations=2`、`timeout=10`、`max_channels=4`、随机偏移、首次失败即停，避免 wedge 污染后续档）：

| 臂 | DT 里 LLI 取指母端口 | 1 节点 | 3 | 4 | 5 | 6 | 7 |
|---|---|---|---|---|---|---|---|
| B2 | 未声明 ⇒ `lli_buses` 默认含 AHB2 ⇒ LLI 词 LM=**1**（master 2） | 0/2 | 0/2 | 0/2 | 0/2 | 0/2 | **首败** |
| D | `lli-bus-interface-ahb1`（且无 `ahb2`）⇒ LM=**0**（master 1） | 0/2 | 0/2 | 0/2 | 0/2 | **首败**（chan1 2/2） | — |

D 臂前提在板上验证过：`ls /proc/device-tree/soc@f0000000/ahb/dma-controller@9870000/` 里有 `lli-bus-interface-ahb1` 且**没有** `…-ahb2`（LM 位只由 `pl08x->lli_buses & PL08X_AHB2` 决定，`amba-pl08x.c:1275`）；绑定横幅 `DMA: PL080 rev1`、4 memcpy＋16 slave、`power/runtime_status = active`。

- **判负**：把描述符取指从 master 2 换成 master 1 **没有**修好 mv100 的深链，边界仍在 ~24 KB 总量附近（B2 到 24570 B 过、28665 B 挂；D 在 24570 B 就有通道挂），6 vs 7 的差在每档 8 样本的噪声内。
- D 臂 6 节点那次的失败形态：`test timed out` ＋ `completion busy status`，**没有** error 中断、没有 `cannot halt`（中断总计 34 次全来自成功样本）⇒ 症状仍是"链走到一半不前进且不报错"，与"取指总线选错"预测不符。
- **公开更正（同日 arm B3A）**：上面"边界 ~24 KB 总字节附近（自变量是总字节还是节点数没定论）"与"链深"这套措辞，**自变量已确定为单次传输总字节数**，且"输入对齐"这条候选**判负**。判决实验：同一颗 `Linux-B3` 镜像（DT width=4）把偏移钉成 0（`norandom=1`，`iterations=6`，四通道）——
  - depth=1（16380 B）：4 通道各 `6 tests, 0 failures`；
  - depth=2（32760 B）：4 通道各 `6 tests, **6 failures**` ⇒ 对齐后不是变好，而是从"随机偏移下的间歇失败"变成**24/24 确定性失败**。
  - 也就是说"驱动/硬件假设输入对齐"这个模式在本颗上不成立：随机偏移时 pl08x 会先发字节宽度 LLI 补齐对齐（`amba-pl08x.c` 的 `early_bytes` 分支），这**改变了每个节点的字节数**，于是把同一个总长度阈值伪装成了"节点数边界"。
- 六个观测统一到"总字节"这一个自变量上：过＝16380（1 节点@w4）、24570（6 节点@w1）；挂＝28665（7 节点@w1）、32760（2 节点@w1 与 @w4 均挂，w1 下随机/对齐都挂）⇒ 阈值 **T ∈ (24570, 28665] 字节**，与节点数（2 vs 7 都能挂/过）、取指母端口（master 1 判负）、偏移对齐（判负）都无关。
- 仍待定的只剩"这个界是什么"：候选＝总长度（本颗）或"从固定起点能覆盖的地址窗口"（arm B3A 起点固定仍挂、随机起点也挂 ⇒ 若属地址类原因，窗口应小于测试缓冲间距）。要再收口就要在失败瞬间读回 `DMAC_CxSRC/DST/LLI/CTRL` 并与描述符内存逐字比对（本 ramdisk 无 devmem，需自备 `/dev/mem` 读件）。
- 对移植的直接影响：mv100 上**链式本身可用**（arm B 证明上游编码能推进 6 节点/24570 B），但总长度有个本颗特有的界；因此"按 SoC 设上限"的正确形态是**按字节上限**（例如 ≤16 KB 最保守、或 ≤24 KB 贴着板证安全区），而不是"关链式"。cv200 侧 65536 B/5 节点已板证通过 ⇒ 该界属于 mv100，不外推也不该套到 cv200。

### 2026-10-24 追加三：以上两节的"边界"作废——那是一把 10 ms 的秒表

仪器缺陷：`dmatest` 的完成等待是**一次性** `wait_event_freezable_timeout(..., msecs_to_jiffies(params->timeout))`（`drivers/dma/dmatest.c:846`），默认 `timeout = 3000`（**毫秒**，:66）。而我这几轮梯子全部显式写了 `echo 10 > timeout` ⇒ 每笔传输只给 **10 ms**。判据旁证：字节边界随 bus width 移动（w1 卡在 24570/28665 之间，w4 卡在 16380/32760 之间）——真正的"总长度上限"不会随传输宽度变，**只有时间上限会**。

用 `timeout=3000`、`norandom=1`、`iterations=4`、四通道重跑同一批尺寸（每节点＝4095×宽度）：

| 臂 | 16380 B | 32760 B | 49140 B | 81900 B | 114660 B |
|---|---|---|---|---|---|
| B3T（上游＋本颗 vendor_data，DT width=4） | 4/0 过 | **4/0 过** | 4/0 过 | 4/0 过 | **4/0 过** |
| CT3（**histb-dma 自己的驱动**，同条件） | 4/0 过（7.0–7.6 MB/s） | **4/4 挂 ×4 通道** | 未跑（首败即停） | — | — |

据此，逐条更正：

1. **撤回**"mv100 存在本颗特有的链式总长度界 T ∈ (24570, 28665]"、"上游深链在 mv100 不行"、"cv200 的 64 KB 结论不外推"。mv100 硅片在上游驱动下链到 **7 节点/114660 B 全过**。
2. **作废**"master 1 判负"与"输入对齐判负"这两条比较——它们的自变量（挂与不挂）本来就是秒表读数，不构成对任一候选的判决。对齐与母端口目前状态＝**未测**（在上游臂上没有可复现的失败可供解释）。
3. **仍然成立且现在更干净**：histb-dma 在 mv100 上链式确实坏——同一块硅片、同一 `timeout=3000`、同一尺寸、同一 dtb 族，上游 2 节点过而**我们 2 节点 16/16 全挂**（`test timed out`，`len=0x7ff8`，偏移 0）。根因即 `histb-dma.c:376` 的 `(addr >> 2) | DMAC_CHAN_LLI_MST`（把地址除了两次），与 cv200 上已修的形态一致 ⇒ **把 cv200 的修正 `e17457cff43d` 带到 mv100**，并把 `histb_dma_runtime_resume()` 里置 `DMAC_CFG_M1/M2` 的 RMW 改成上游那种只写 ENABLE 的形式（`amba-pl08x.c:2281`）。
4. **"要不要干脆不实现链式"这个问题随之消解**：硅片没有那个界，所以没有"因硬件不行而拒绝大传输"的理由；上一轮我给的"停在单节点＝落在证据上"作废——正确动作是修 LLI 编码，不是设上限。
5. 仪器纪律（写进 skill）：跑 dmatest **必须显式核对 `timeout` 是毫秒**并按尺寸/实测吞吐设值（2 MB/s 下 128 KB ≈ 64 ms ⇒ 默认 3000 ms 才够），报告"边界/失败率"要同时报 `timeout`、`norandom`、每档样本数；我这次是同一个 `timeout=10` 污染了 A/B/C/B2/B3/B3A/D 六组数据里所有"超时"判据（注意：`cannot halt transfer`、`0 中断`、`prep error`、部分拷贝落在 `dstbuf[0x401a]` 这几类读数**不依赖**该 timeout，仍然有效）。

### 2026-10-24 追加四：arm E/F 实测——用 pl08x 替换 histb-dma 缺的两样东西

两臂同一颗 zImage（含 arm B 的 scratch `vendor_data`），dtb 只加一行 `arm,primecell-periphid = <0x00041080>`（⇒ 总线**不再读硬件 ID**，也就跳过 `amba_read_periphid()` 里那次解复位）：

| 臂 | 绑定 | 唯一差别 | 单节点 16380 B（`timeout=3000`、`norandom=1`、4 迭代×4 通道） |
|---|---|---|---|
| E | `id`＝`00041080`、`DMA: PL080 rev0`、8 memcpy＋16 slave（plain `vendor_pl080`） | 复位由 Linux 决定（没人放） | **4/4 挂 ×4 通道**，`test timed out`，TC 中断 0 |
| F | 同一镜像 | `bootimg` 前在 fastboot 里 `mw.l 0xf8a220a4 0x1`（CRG41 `0x10`→`0x1`，随后 `0xFE0…` 立刻读出 `80 10 14 0a`） | **4/0 过 ×4 通道**，TC 中断 16 |

⇒ `amba_probe()` 本来就会 `amba_get_enable_pclk()`，**E 臂缺的不是时钟而是复位**；arm A/B 之所以能跑，是 `amba_read_periphid()` 顺手做了 `of_reset_control_array_get_optional_shared()` ＋ `reset_control_deassert()`（`drivers/amba/bus.c:106-140`）这件事被当成了前提。

第二条：E/F 用的 plain `vendor_pl080` 把通道数硬编码成 8，而本颗只有 4。`max_channels=8` 实测 **chan4、chan5、chan7 各 2/2 超时**（同一次启动里 chan0–3 是 4/0 过，chan6 侥幸通过＝未实现通道的寄存器别名/回绕）。⇒ 通道数必须来自节点（`dma-channels`，`dma-controller.yaml` 里已有该属性）或本颗专用 `vendor_data`。

**替换配方**（都以上面两条实测为依据）：

1. **复位**：在 AMBA 总线 probe 期解复位（`amba_read_periphid()` 已有同样代码，只是绑在"读 ID"这条路上），或让 pl08x 自己拿 `resets`。不做这条，声明 ID 的那条路必挂（arm E）。
2. **通道数**：pl08x 解析 `dma-channels` 覆盖 `vd->channels`（通用改法），本颗节点写 `<4>`。
3. **变体消歧**（硅片自报 `0x0a141080` 与三星 PL080S 条目逐位全等，且三星是 mach-s3c 板文件注册 `AMBA_AHB_DEVICE(s3c64xx_dma0, "dma-pl080s.0", …)`，periphid 表分不开）：
   - 路线甲＝DT 声明 `arm,primecell-periphid = <0x00041080>` 走 plain `vendor_pl080` 形状（本颗实际形状就是 plain PL080：CH_CONFIG 在通道偏移 0x10、4 词 LLI、支持外设流控），需配合第 1、2 条；不动 id_table，不抢三星绑定。代价：声明值与硅片 ID 不符，得在 binding/注释里写清"本颗 ID 与三星变体重号，按可用形状声明"。
   - 路线乙＝在 `pl08x_ids[]` 里给本颗加条目并压在三星之前 ⇒ **会夺走 s3c64xx 的绑定**，不可上游。
   - 已实测支持：plain 形状＋复位已放＝arm F 过；plain 形状＋`channels=4`＝arm B，且链式推到 **7 节点/114660 B 全过**（arm F 用的是 `channels=8`，深度未单独复测）。

### 上游怎么处理"periphid 撞号／ID 不够用／被 boot 按在复位里"（调研，全部给本树 file:line 或提交号）

1. **总线只按 periphid 匹配**：`amba_match()` → `amba_lookup()`（`drivers/amba/bus.c:49-60`、`:180-209`）。primecell 驱动虽然带 `.of_match_table`（`amba-pl011.c`、`i2c-nomadik.c`、coresight 各家），但**不参与总线匹配**，所以"靠 compatible 消歧"在 AMBA 总线上没有现成通道。
2. **上游遇到"ID 不足以判别"时的既有解法＝在总线层加第二个寄存器维度**：`amba_cs_uci_id_match()`（`bus.c:30-46`）用 CoreSight 的 DEVARCH/DEVTYPE 做二次判别，`amba_id.data` 被改造成 `struct amba_cs_uci_id`（`include/linux/amba/bus.h:51-56`，注释自己承认 "As we have usurped the original pointer…"）。**对我们不适用**：本块 CID 装配值是 `0xb105f00d`＝`AMBA_CID`（`bus.h:22`），不是 `CORESIGHT_CID`（`0xb105900d`）。注：先前我在报告里把 CID 写成 `0xB1F0050D` 是字节序拼错，已更正。
3. **实践中的通行做法＝DT 声明要绑的 ID**：绑定文本就一句 `arm,primecell-periphid: Value to override the h/w ID value`（`Documentation/devicetree/bindings/arm/primecell.yaml`），没有"仅限 ID 寄存器损坏"的限制。树内先例三处——BCM63xx `pl081_dma` 声明 `0x00041081` 并注明 `// The magic B105F00D info is missing`（`arch/arm/boot/dts/broadcom/bcm63138.dtsi:324-335`，同一节点还给了 `memcpy-bus-width = <32>`、`memcpy-burst-size = <256>`、`#dma-cells = <2>`）；BCM283x/2711 的 PL011 变体声明 `0x00241011`/`0x00341011`；Gemini 的 Faraday 变体声明 `0x0003b080` 并带 `resets`。**结论：我们的"路线甲"就是上游既成习惯，而且三星那条目（`da1b6c05b8b5 "dmaengine: PL08x: Add support for PL080S variant"`，Tomasz Figa）用的是不同的 PID——即上游从未处理过"两个形状不同的 IP 共用同一 PID"，我们这个是新增情况（lore/web 检索未见相关讨论，如实记为未找到）。**
4. **复位**：动机与我们一模一样的提交是 `79bdcb202a35 "ARM: 8906/1: drivers/amba: add reset control to amba bus probe"`（Dinh Nguyen；Rob Herring、Philipp Zabel、Linus Walleij 评审）——正文原话是 U-Boot 开始"把没用到的外设留在复位里，交给 Linux 放"。但它把 `of_reset_control_array_get_optional_shared()`＋`reset_control_deassert()` 写在 `amba_read_periphid()` 里，于是**声明了 periphid 就跳过**（arm E 实测 4/4 挂、arm F 手工放复位后 4/0 过）。上游同期还有另一条更普遍的做法＝**驱动自己拿 `resets`**：同为 AMBA DMA 驱动的 `pl330.c:3048` 用 `devm_reset_control_get_optional(&adev->dev, "dma")`＋`reset_control_deassert()`（还有第二条 `"dma-ocp"`），`mmci.c:2331/2336` 同理 ⇒ 给 pl08x 补这两行是照抄邻居驱动，不是自造机制。后续修正见 `e963408e8ff4`、`1f44de0f5e30`。
5. **通道数**：树内 32 个 dma 驱动解析 `dma-channels`（`dma-controller.yaml` 定义该属性），pl08x 却把 `channels` 写死在 `vendor_data`（只有 FTDMAC020 从 feature 寄存器取）⇒ 加 `dma-channels` 解析属通用改进，不新增 ABI。
6. **slave 侧还没核的一点**：pl08x 的 `of_xlate` 里 args[0] 实际是 **signal 号**（`pl08x_find_chan_id()` 比的是 `chan->signal`，`:2524`），DT 下按 `vd->signals` 每个 signal 建一条虚通道；而 `pd->get_xfer_signal`（外部 mux）只有平台数据路径有。signal → `DMAC_CxCONFIG` 的 SRC/DST 选择域是否与我们 mv100 的请求线编号同序，**尚未核对**（做音频 slave 前必须先量）。

### 关于"链式能不能干脆不实现"

接口层面的实测结论（本树 7.3-rc7）：

- **没有"不支持 LLI"的能力位**。`dma_device` 里与容量有关的只有 `src/dst_maxburst`、`min_burst`、`max_burst`、`max_sg_burst`、`descriptor_reuse`（`include/linux/dmaengine.h:508-530`），其中 `max_sg_burst` 是"一次提交的 SG 条目数上限"（只有 `stm32-dcmi`、`spi-dw-dma`、`spi-ingenic` 三个 client 会读），既不是单描述符字节上限，也不作用于 memcpy。
- **core 不替 client 切分**：`device_prep_dma_memcpy()` 收到的 `len` 是任意长度，驱动要么自己切成多节点，要么返回错误；`drivers/dma/` 里也没有 `-EF2BIG` 的先例可引。
- 硬件侧没有余地：`DMAC_CX_CONTROL` 的 TC 域 12 bit ⇒ 单节点上限 4095 拍＝**16380 字节**（4 字节宽度），>16 KB 的搬运只能靠 LLI。
- 所以"关掉链式"只能表达成"对超限返回 `-EINVAL` ＋ 不注册 `DMA_MEMCPY/DMA_MEMSET` 能力"，并且**必须按 SoC 分叉**（`of_device_id.data` 带 quirk）：cv200 的链式已板证（52821 tests/0 failures、64 KB 5 节点通过），全局关等于砍掉 cv200 的能力。
- 当前 mv100 树内**没有任何 DT 消费者**（`arch/arm/boot/dts/hisilicon/` 里 `dmas =` 零命中，唯一命中的 hi3660/hi6220 用的是别的引擎），所以关掉不改变现有行为——但这恰恰是它难辩护的地方：目标用途（音频、解码器搬码流）迟早要 >16 KB。

实验改动已全部还原：`amba-pl08x.c` 的 scratch `vendor_data`/ids 条目与 dtsi 的 primecell 段都已删除（`git status` 里该文件剩余的 17 行改动全是另一会话的 SD pinctrl 在途工作，与本档无关），`.config` 回到 `7c0ef3fe7de2eba995f1bd05658b7712`（撤掉 `CONFIG_DMATEST`）；arm D 的镜像全程只在宿主 `/tmp` 里做（`/tmp/armD`、`/tmp/h005.img`），没有再动共享树。板子留在 `fastboot#`，两把 ex 锁已释放。

### 2026-10-25 落码状态（pl08x 替换）＋板验

已落地（临时提交 `8f76f73425a1`，在已推送的 `e92ca2afc9bc` 之上，**未折叠、未推送**）：

- `drivers/dma/amba-pl08x.c`：①`probe` 里 `devm_reset_control_get_optional_exclusive()` ＋ `reset_control_deassert()`（照 pl330/mmci 的做法，放在任何寄存器读之前）；②解析 `dma-channels` 覆盖 `vd->channels`（只降不升，升了 `dev_warn`）。
- `arch/arm/boot/dts/hisilicon/hi3798mv100.dtsi`：dmac 节点改成 `arm,pl080`,`arm,primecell` ＋ 注释说明撞号理由的 `arm,primecell-periphid = <0x00041080>` ＋ `clock-names = "apb_pclk"` ＋ `resets` ＋ `dma-channels = <4>` ＋ `#dma-cells = <2>` ＋ `memcpy-bus-width = <32>`/`memcpy-burst-size = <256>`（后两条照 BCM63xx 节点的写法）。
- 不需要改的：`ARCH_HISI` **本来就 `select ARM_AMBA`**（`arch/arm/mach-hisi/Kconfig:5`；我先前"配置面进不去"的说法是只 grep 了 `arch/arm/Kconfig` 造成的错判）；`arm-pl08x.yaml` 不用动（`resets`、`dma-channels`、periphid 覆盖都被它接受，见下）。
- histb-dma 已摘除：驱动 `drivers/dma/histb-dma.c`（1402 行）与 binding `hisilicon,hi3798mv100-dma.yaml` 移出树（原件按本机规则移入 `回收站/` 而非删除，md5 `4c9fbc159207fb0f2f84b42c8e515a88`／`92107e53e9f2c4e6cf3a2a526f25ed79`，git 历史里也还在），`drivers/dma/Kconfig` 的 `config DMA_HISTB`、`Makefile` 的 `obj-$(CONFIG_DMA_HISTB)`、`MAINTAINERS` 的 `HISILICON STB DMA ENGINE DRIVER` 段一并删；摘后 `make zImage dtbs` 0 error，`auto.conf` 里 `DMA_HISTB` 消失、`AMBA_PL08X=y` 仍在。
- **折叠时的归属**（临时提交里混着别人的活）：`amba-pl08x.c` 两处＝两枚独立上游补丁（probe 解复位、认 `dma-channels`）；dtsi 的 dmac 节点＝改 dts 那枚；binding/驱动/`Kconfig`/`Makefile`/`MAINTAINERS` 的删除＝把原来两枚 histb-dma 提交（`aff0edc2707e`、`7c8d206cbd5a`）从系列里去掉；而本临时提交里 **`hi3798mv100.dtsi` 的 `sd` pinctrl 块与 `hi3798mv100-ec6108v9c.dts` 的 28 行是另一会话在途的活**，要折回它们自己的提交，不进 DMA 系列。

门（本轮真跑了的）：`W=1 drivers/dma/amba-pl08x.o` 看到 `CC` 行且 0 告警；`make dtbs zImage` rc=0；`checkpatch --strict` 对改动 patch ＝ **0 errors/0 warnings/0 checks（106 行）**；`dt_binding_check`（`DT_SCHEMA_FILES=arm-pl08x.yaml`）四步 LINT/STYLE/DTEX/DTC 全过；`dtbs_check dtstree=arch/arm/boot/dts/hisilicon` 在 `touch` 强制重建后真跑了（43 行既有噪声），**dmac 节点 0 次点名**；牙的验证＝临时注入 `bogus-probe = <1>;` 会被点名 `Unevaluated properties are not allowed`，删掉后干净。注意 `dtbs_check` 只在 dtb 重建时才校验，dtb 未变时会静默跑零个文件。

板验（2026-10-25，构建 `#132`，镜像 `Linux-PL2`，板子网络已移到宿主 `enp5s0.2001`＝`192.168.1.2/24`、板端 `192.168.1.45`；串口换到 `usb-1a86_5523`→ttyUSB0，Prolific 那条当时被 mv200h 会话以新鲜 ex 锁持有）：

- 绑定面：`cat /sys/bus/amba/devices/f9870000.dma-controller/id` ＝ `00041080`（覆盖生效）；`/sys/class/dma/` ＝ **dma0chan0–3 ＋ dma1chan0–15 共 20 条**（plain `vendor_pl080` 原本是 8＋16＝24 ⇒ `dma-channels` 覆盖生效）；`dmesg | grep -ic histb-dma` ＝ **0**（histb-dma 不再绑定）。
- 功能性 dmatest（`norandom=1`、`iterations=4`、`timeout=3000`、`max_channels=4`、`device=f9870000.dma-controller`；每节点 16380 字节）：

| 节点数 | 1 | 2 | 3 | 5 | 7 |
|---|---|---|---|---|---|
| 字节 | 16380 | 32760 | 49140 | 81900 | 114660 |
| 结果 | 4/0 | 4/0 | 4/0 | 4/0 | **4/0** |
| MB/s | 3.6–5.1 | 6.5–8.5 | 9.6–13.5 | 18.7–24.1 | 21.6–25.7 |

  零数据错误（无 `not copied`），TC 中断正常上涨（该 boot 末计数 68）。**depth=2/32760 B 这一档正是 arm E（未解复位）必挂的那一档**，现在过 ⇒ 驱动侧 `resets` 这条路成立；`memcpy-bus-width=<32>` ＋ `memcpy-burst-size=<256>` 也是吞吐从 arm B 的 1.8 MB/s 提到 20 MB/s 级的原因。
- `.config` 已还原到 `7c0ef3fe7de2eba995f1bd05658b7712`（`CONFIG_DMATEST` 是唯一差异，已撤）；板子留在 `fastboot#`；两把 ex 锁释放进 `回收站/`。

还欠的两条：**（a）** slave 侧 `#dma-cells` 第一格是 **signal 号**（`pl08x_find_chan_id()` 比 `chan->signal`），它与 mv100 请求线在 `DMAC_CxCONFIG` SRC/DST 选择域里的编号是否同序，**未测**——接音频之前必须先量这条；**（b）** `vd->channels` 写的是全局 static 表（沿用 FTDMAC020 的旧做法），上游大概会要求改成驱动内副本。

## 未纳入（板级支持，非驱动）

- `arch/arm/mach-hisi`（headsmp.S 保持环、platsmp `hi3798_smp_ops`、hotplug `hi3798_set_cpu` 写 CRG18/20、S40/S5 `DT_MACHINE_START` + 32MB 静态 iomap）。这些不是驱动；且上游 ARM 多平台方向明确不欢迎新增 `map_io` 静态映射与在 boot_secondary 里写 CPU 频率（CRG18=0x306「400MHz」），SMP 起核/热插拔的时序**只有真板能验**，用户要求不上机 → 不在本次重写内，保持原分支未动。
- `arch/arm/boot/dts/hisilicon/*`（hi3798.dtsi 等）。DTS 要过 `dtbs_check` 就得让每个 compatible 都有 binding，而里面大量外设（femac、histb-rng、hinfc610、usb2-phy、mali、dw-mshc…）无对应驱动/schema，HDMI 又排除——无法自成「可发送」系列。另记：现有 hi3798.dtsi 有 CRG reg `0x2000` 与 PMOC 调压器 `0x8a23018` **地址重叠**、ethernet 节点里的 `mac-address` 占位 hack、`phy-mode="mii"` 等需在真正做 DTS 系列时处理。

## 复现命令

```
cd /srv/blob/histb/linux-mv100-clean
# 编译
./scripts/config --file /tmp/obj-mv100/.config -m COMMON_CLK_HI3798MV100 -m REGULATOR_HISI_STB -m HISI_FESTA_PHY -m DMA_HISTB
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- O=/tmp/obj-mv100 olddefconfig
make -j4 W=1 ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- O=/tmp/obj-mv100 drivers/clk/hisilicon/ drivers/dma/ drivers/net/phy/ drivers/regulator/
# 门
git format-patch upstream/master..HEAD -o /tmp/series
for p in /tmp/series/*.patch; do scripts/checkpatch.pl --strict --no-signoff "$p"; done
make ARCH=arm CROSS_COMPILE=arm-linux-gnueabihf- O=/tmp/obj-dt dt_binding_check DT_SCHEMA_FILES=Documentation/devicetree/bindings/clock/hisilicon,hisi-crg.yaml
```

分支尚未 push（仅本地 worktree）。**2026-10-18 那 7 个改名路径已全部折进各自提交**——
`drivers/dma/histb-dma.c` 进驱动提交、binding 文件（现名
`Documentation/devicetree/bindings/dma/hisilicon,hi3798mv100-dma.yaml`）进 binding 提交、
`drivers/dma/Kconfig` 与 `Makefile`、`arch/arm/boot/dts/hisilicon/hi3798.dtsi`、`MAINTAINERS`
与本文件按同样归属。折叠只改链顶与提交号，工作树不留改动。

## 2026-10-18 家族级改名（DMA；用户指令"dma 改为 histb"）

只动 DMA，HDMI/codec 未动：

| 项 | 改名前（分支内容） | 改名后 |
|---|---|---|
| 驱动文件 | `drivers/dma/hi3798mv100-dma.c` | `drivers/dma/histb-dma.c` |
| 标识符 | `hi3798mv100_dma_*`（210 处） | `histb_dma_*` |
| DT 串 | `hisilicon,hi3798mv100-dma` | `hisilicon,histb-dma` |
| binding | `.../bindings/dma/hisilicon,hi3798mv100-dma.yaml` | `.../bindings/dma/hisilicon,histb-dma.yaml`（title 同步，并新增 description 声明"四通道是 mv100 实例"） |
| Kconfig | `HI3798MV100_DMA`（在 `HISI_DMA` 之前） | `DMA_HISTB`，条目按字母序挪进 `DMA_*` 组（`DMA_BCM2835` 与 `DMA_JZ4780` 之间），Kconfig 与 Makefile 一起挪 |
| `.name` / `MODULE_DESCRIPTION` | `hi3798mv100-dma` / `Hi3798MV100 DMA Controller` | `histb-dma` / `HiSilicon STB DMA Controller` |
| MAINTAINERS | `HISILICON HI3798MV100 DMA ENGINE DRIVER` ＋ 两条 F: | `HISILICON STB DMA ENGINE DRIVER` ＋ 两条 F: 同步 |
| dts | `arch/arm/boot/dts/hisilicon/hi3798.dtsi` 的 compatible | 同步 |

故意不动：时钟 ID `HI3798MV100_DMAC_CLK`（CRG 头是芯片级命名空间）、寄存器宏 `DMAC_*`（手册块名）、
板级 `simple-audio-card,name = "hi3798mv100-hdmi"`（声卡名属板不属驱动）。

门（本轮重跑，不引用旧结果）：`.config` 与 `auto.conf` 都只剩 `CONFIG_DMA_HISTB=y`；
`W=1 drivers/dma/histb-dma.o` 看到 `CC` 行且零告警；`Image`+`vmlinux` 重链，
`nm vmlinux | grep -c hi3798mv100_dma` = 0；`dtbs` 里该节点串是 `hisilicon,histb-dma`；
`DT_SCHEMA_FILES=...hisilicon,histb-dma.yaml dt_binding_check` 的 LINT/STYLE/DTEX/DTC 四步都按新名执行、
无告警；`make dtbs_check` 103 行输出里没有 `histb-dma`/`dma-controller@9870000` 的抱怨，而同轮仍报
`['hisilicon,hi3519']`、`hisi-femac-mdio`、`hisi-femac-v2`、`hix5hd2-ir` 四条 failed-to-match
⇒ 校验确实跑了；`checkpatch --strict -f drivers/dma/histb-dma.c` = **0/0/0（1387 行）**，对照改名前
HEAD 版同命令下是 11 条 `Alignment should match open parenthesis`（长标识符撑破续行对齐，短名复原）。
多文件 `DT_SCHEMA_FILES` **必须冒号分隔**：`Documentation/devicetree/bindings/Makefile:33` 把它经
`subst :," -e ",…` 喂 `grep -F`，空格分隔会让文件列表为空、工具拿到 0 输入仍返回 0（假干净）。

### 通道数改为从设备树取（2026-10-18，折进上面两枚 DMA 提交）

家族名要成立，通道数就不能是驱动的常量：`DMAC_CHAN_NUM 4` 换成 `DMAC_MAX_CHANS 32`（依据是中断状态与
使能回读寄存器每位一个通道），`struct histb_dma_priv` 尾部改成柔性数组 `chans[]` 加 `num_chans`，probe
用 `of_property_read_u32()` 读 `dma-channels`、按 `struct_size()` 分配，`0` 或超过 32 直接 `-EINVAL`；
binding 里 `dma-channels` 从 `const: 4` 改成 `minimum: 1 / maximum: 32` 并加入 `required`，描述改成"每颗
SoC 实例的通道数不同，节点必须自己写明"。`histb_dma_of_xlate()` 走 `dmadev->channels` 链表取通道、
不按号索引，所以柔性数组的边界只由 probe 那一条路径决定。

门（2026-10-18 重跑）：`W=1` 重编译看到 `CC drivers/dma/histb-dma.o` 且零告警；`Image` + `dtbs` 重链；
`DT_SCHEMA_FILES=Documentation/devicetree/bindings/dma/hisilicon,histb-dma.yaml dt_binding_check` 强制
重跑后 SCHEMA/CHKDT/LINT/STYLE/DTEX/DTC 全部执行、无告警；`make dtbs_check` 103 行输出里对
`dma-controller@9870000` 零抱怨，同轮仍有 4 条别的 compatible failed-to-match（门有牙）；
`checkpatch --strict -f drivers/dma/histb-dma.c` = 0 errors / 0 warnings / 0 checks（1404 行）。
**仍欠**：mv100 板上 `dmatest` 复跑——`hi3798.dtsi` 里 `dma-channels = <4>` 已存在，行为不该变，但这条
改动是功能性的，得上机确认才算完。

本轮折叠时把 `dt-bindings: dma` 的标题改成家族级（已随上面那次改名落地），另外四枚提交的标题/正文
也一并 reword：`dt-bindings: display: add HiSTB VDP2 and hi3798mv100 HDMI schemas`、
`drm/bridge: add SiI9134 HDMI transmitter core and hi3798mv100 glue`、
`drm/histb: add VDP v2 display engine driver`、`ASoC: hisilicon: add the HiSTB AIAO I2S transmit port`，
正文里 `histb-vo.c`/`histb-vo-regs.h`/`histb-hdmi-14.c` 这些已不存在的文件名同步改掉。折叠时顺带 reword 提交标题
`dt-bindings: dma: add HiSilicon Hi3798MV100 DMA controller`（家族级名已不含该芯片号）。
