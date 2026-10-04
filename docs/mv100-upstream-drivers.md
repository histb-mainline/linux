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
