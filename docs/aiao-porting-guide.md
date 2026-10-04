# AIAO 移植指导（Hi3798MV100 / EC6108V9C）

补 AIAO = 一个 ASoC CPU DAI 驱动 + CRG70 时钟域 + DTS 转正，**不引入其他硬件驱动**。
HDMI codec 侧（sii9134 bridge）已就绪。本文全部来自 SDK 与主线源码普查，除特别标注外
未经上板复测；标"推断"的条目给出上板判定方法。

## 1. AIAO 硬件事实（SDK 证据）

SDK 路径：`sdk/HiSTBLinuxV100R005C00SPC050/source/msp/drv/aiao/aiao_v1_1/`，
下文相对此目录。

| 项 | 值 | 证据 |
|---|---|---|
| 寄存器基址 | `0xF8CD0000`，窗口 0x10000 | `common/include/hal_aiao_common.h:87`；窗口大小 `AIAO_REG_SIZE` 定义在 `common/src/hal_aiao_priv.c` |
| 内部布局 | COM 0x0000 / CRG 0x0100 / RX 0x1000 / TX(I2S) 0x2000 / TXSPDIF 0x3000 / SPDIFER 0x4000 | `hal_aiao_common.h:91-96` |
| 中断 | GIC SPI 84，电平触发（INTID 116） | vendor 4.4 DTS 中断表 `<0 84 4>`（`linux-4.4.176/arch/arm/boot/dts/hi3798mv100.dts:509`）+ `hal_aiao_common.h` asic 分支 `(84 + 32)` 双证 |
| 数据通路 | 片内 AOE/AIP ring-buffer 引擎直驱 DDR，**不挂 SoC DMAC** | `AIP_BUFF_ATTR`/`AOP_BUF_*` 寄存器（`ao/hal_aoe_func.c:400` 起） |
| 通路分支 | mv100 用 **sw 分支**（ARM 软件通路）；hw 分支 = ADSP DSP 通路，不涉及 | `readme.txt`（"aiao_sw_vx_x: arm ao 通路支持"） |

**中断号更正**：`AIAO_IRQ_NUM 79`（`hal_aiao_common.h:32`）在 `AIAO_SW_SIMULAUTE`
分支里，不适用实板；实板按 asic 分支 `84 + 32`。此前的"IRQ 79"结论作废。

- AIAO 内部自带 3 个时钟发生器 `AIAO_TX_CRG0/1/2`（生成 I2S bclk/lrclk，master 模式），
  与芯片级 CRG70 是两回事（`common/src/hal_aiao.c:60-104`）。
- HDMI 音频：AIAO TX 口 → SiI9134 为芯片内部布线，驱动侧只需开对口；
  `HI_SND_HDMI_I2S_SPDIF_MUX` 控制口复用，HDMI I2S 默认参数见
  `common/src/hal_aiao.c:1496`（HBR）/`1532`（I2S）。

## 2. CRG70 时钟域（唯一的外部依赖）

`PERI_CRG70`（偏移 `0x118`，`source/common/drv/include/hi3798mv100/hi_reg_crg.h:941` 起）：

| 位 | 字段 | 说明 |
|---|---|---|
| [0] | `aiao_cken` | AIAO 工作时钟门控 |
| [4] | `aiao_srst_req` | 软复位 |
| [16..12] | `aiaoclk_skipcfg` | 音频时钟分频旁路配置 |
| [17] | `aiaoclk_loaden` | 分频装载使能 |
| [21..20] | `aiao_mclk_sel` | mclk 源选择：0 = 750/307MHz 宏分支，1 = 600MHz 宏分支 |

- vendor 由 aiao 驱动自己裸写这些位（`common/src/hal_aiao_priv.c:624-662`）。
- **陷阱：寄存器编号 ≠ 偏移**。CRG70 = `0x70 * 4 = 0x118`；
  `crg-hi3798mv100.c` gate 表里偏移 `0x70` 处是 CRG28/SPI0，勿混。
- `PERI_CRG69`（偏移 0x114）是 `adac_cken`，本板不用（cfg `CFG_HI_ADAC_SUPPORT is not set`）。

**mclk 源未定论（推断）**：`aiao_mclk_sel` 在固定 PLL 抽头间二选一，主线 CCF 的
fixed clock 表（`mv100-codec` 分支 `drivers/clk/hisilicon/crg-hi3798mv100.c`）没有音频
PLL，推断该 PLL 常开、CRG70 只做选择与分频。判定方法：上板配 48k 与 44.1k 各放一次，
bclk/lrclk 实测对得上即证。

## 3. 依赖判定

**需要补**：
1. ASoC CPU DAI 驱动（不搬 vendor UNF 框架，见 §4）；
2. CRG70 时钟域：进 CCF（`crg-hi3798mv100.c` 加 gate）或 DAI 驱动自管。现有 DTS 脚手架
   已选后者（节点带 `hisilicon,histb-crg = <&crg>` phandle），两条路二选一，勿同时做；
3. dt-binding schema（脚手架 compatible `hisilicon,histb-aiao-i2s` 无 schema，
   dtbs_check 会挂，这是当初把它拆出显示系列的原因）。

**不需要**（均已核查）：
- SoC DMA 控制器（mv100-codec 分支已合并的 dmaengine 提交与 AIAO 无关，见 §1 通路）；
- ADSP（仅 hw 分支需要）；
- ADAC / 模拟输出（`CFG_HI_ADAC_SUPPORT is not set`，EC6108 音频出 HDMI）；
- VO/HDMI 侧改动——sii9134 bridge 音频已全（hdmi-audio-codec platform device、
  N/CTS 重算、infoframe，`drivers/gpu/drm/bridge/sii9134.c:1112` 起）；
- AI（录音）/vdec/adec：主线纯 ALSA，数据从用户态直接进 AIAO；
- MUTECTL GPIO 0x23（cfg:176-178，功放静音）：HDMI 出声用不到；若将来接模拟口需它。

## 4. vendor 源码地图（读什么、不搬什么）

```
aiao_v1_1/
├── common/src/hal_aiao.c        HAL 寄存器层：端口开关、默认参数（§1 引用）
├── common/src/hal_aiao_priv.c   CRG70 操作、ioremap、寄存器抽象 —— CRG 位序的权威
├── common/src/hal_aiao_func.c   口复用（HDMI_I2S_SPDIF_MUX）
├── ao/hal_aoe*.c                AOE buffer 引擎（ring buffer 编程，重点读）
├── ao/drv_ao*.c                 UNF 框架（track/aef/cast）—— 不搬
├── ai/                          录音口 —— 本板不用
├── alsa/dsp_mix_v2/             vendor 自有 ALSA 胶水 —— 不搬
└── timer/                       硬件计时器辅助 —— 不搬
```

寄存器位段定义集中在 `common/include/hal_aiao.h` / `hal_aiao_priv.h`。

## 5. 主线侧现状

树 `/srv/blob/histb/linux-mv100`（分支 mv100-hdmi）：
- DTS 脚手架已入库：`5b37e4ed93c5` [LOCAL]。内容：`hi3798.dtsi` 加
  `aiao: audio@8cd0000`（compatible `hisilicon,histb-aiao-i2s`，reg 0x8cd0000/0x10000，
  **无 interrupts 属性——转正时必须补 `interrupts = <GIC_SPI 84 IRQ_TYPE_LEVEL_HIGH>`**）；
  `hi3798mv100-ec6108v9c.dts` 加 simple-audio-card（format i2s、mclk-fs 256、
  bitclock-master/frame-master = cpu、codec 侧 `sound-dai = <&hdmi>`）。
  提交说明自带分工结论：AIAO 是 bitclock/frame master，transmitter 只收流。
- sii9134 bridge 音频已就绪（§3）。
- 内核 `.config:3479` `CONFIG_SOUND is not set`——整个 ALSA/ASoC 未开。

树 `/srv/blob/histb/linux-mv100-codec`（分支 mv100-codec，基于 7.3-rc5）：
- `crg-hi3798mv100.c` 已有，gate 表无 AIAO；
- 已合并 dmaengine/CRG/DT 提交是 SoC 通用件，与 AIAO 无关（勿误认为音频已备）。

## 6. 测试环境缺口（板测能力现状）

initramfs `mkbootimg/ramdisk.cpio.xz`（3.1 MB，标准 OpenWRT 用户态）不支持音频测试：
- 1073 项中 snd/alsa/aplay 零命中；
- 自带 `lib/modules/6.12.94/*.ko` 与 6.19.0 内核不匹配，全是死件；
  **不能走"往现有 initramfs 塞 kmod"的路**——新模块要么 built-in 进 zImage
  （uImage ~9.9 MB，16 MB 上限内有余量），要么重打 ramdisk。
- 显示测试不受影响（`CONFIG_DRM=y` + fbdev 内置；用户态无 fbset/modetest，
  一直靠内核日志+串口+看屏）。
- `/srv/blob/openwrt`（CR8818 树）只是 tftp 落点目录，不在那里给 mv100 构建音频包。

最小补齐路径：
1. 内核开 `CONFIG_SOUND / SND / SND_SOC / SND_SOC_SIMPLE_CARD / SND_SOC_HDMI_CODEC`，
   音频模块全 built-in；
2. 用户态：交叉编静态 aplay 单文件 + 测试 wav 推板（板上 busybox 有 wget，
   HTTP 推送链路现成，见 relay.md）；或重打 ramdisk 带 alsa-utils；
3. 打包上板按 relay.md 的 mkbootimg 配方（zImage+dtb → uImage → recovery-hdmiNN.img）。

## 7. 建议顺序

1. dt-binding schema（`hisilicon,histb-aiao-i2s`）；
2. CRG70：`crg-hi3798mv100.c` gate（或定死走驱动自管，与 DTS 二选一）；
3. ASoC DAI 驱动（I2S TX 口优先，SPDIF 可后置）；
4. DTS 转正：补 interrupts、去 [LOCAL]；
5. Kconfig/用户态按 §6；
6. 上板验收。

## 8. 验收判据

验收标准是板上出声：`aplay` 播 48k/44.1k 测试 wav，HDMI 端听到/分析仪看到音频流。
中间产物（寄存器读回、infoframe 十六进制）只作定位证据，不作为完成判据。
未定论项（mclk 源，§2）在验收时一并判定。
## 9. 移植进展（2026-10-06，h137/h143 实板）

### 9.1 内核侧已落地

- CRG 全部进 `drivers/clk/hisilicon/crg-hi3798mv100.c`，AIAO 驱动不碰 SoC 级寄存器：
  - `HI3798MV100_AIAO_CLK = 37` 的 gate 走 bits-clk 表（`CRG70[0] aiao_cken`），
    复位走 `resets = <&crg 0x118 4>`（`aiao_srst_req`，1=复位）。
  - 新增 `hi3798mv100_setup_aiaoclk()`：按 vendor `AIAO_HW_PowerOn()` 尾段
    （`msp/drv/aiao/aiao_v1_1/common/src/hal_aiao_priv.c:686-700`）写
    loaden=0 → `skipcfg=0x10`（注释"aiaoclk = 100M"）→ loaden=1。
    `aiao_mclk_sel[21:20]` 实测上板就是 0（= 492MHz 抽头），无需写。
- 驱动：`sound/soc/hisilicon/histb-aiao.c`（TX3 播放口）、Kconfig
  `SND_SOC_HISTB_AIAO`、bindings yaml、`hi3798.dtsi` 节点 + 板级
  `&aiao status="okay"`，机器侧用 `SND_SIMPLE_CARD` + `SND_SOC_HDMI_CODEC`。
  注：simple-card 的符号是 `SND_SIMPLE_CARD`，本指导 §6 里写的
  `SND_SOC_SIMPLE_CARD` 在 7.3 树里不存在。

### 9.2 已实测通过的点

- probe 干净，声卡实例化：`/proc/asound/cards` = `hi3798mv100hdmi`，
  `/proc/asound/pcm` = `00-00: aiao-i2s-i2s-hifi`，`/dev/snd/pcmC0D0p` 可 open
  （open 曾报 EINVAL：非 dmaengine 的 CPU DAI 必须在 component `.open` 里把
  `snd_pcm_hardware`（含 `period_bytes_min/max`）写进 `runtime->hw`，
  否则 `snd_pcm_hw_constraints_complete()` 直接失败）。
- 寄存器落点全部核对过（`histb_vodump` 加了 `poke_sel=3` = AIAO 窗口）：
  - TX 口基址 = `0x2000 + 3*0x100 = 0x2300`（mv100 HDMI0+I2S = TX3：
    `ao/drv_ao_op.c:64-70` + `common/src/audio_util.c:1103-1124` 的 S40 分支）。
  - CRG：TX_CRG3 在 COM+`0x140+3*8` = `0x158/0x15c`；48k@256FS 读回
    `CFG0=0x00333333`、`CFG1=0x00000133`（bclk_div=3、fsclk_div=3、cken=1）
    与 `g_u32MclkPLLTab`（`hal_aiao_priv.c:191-208`，AIAO_PLL_492MHZ）一致。
  - 主口还必须写 COM+`0x2c AIAO_SWITCH_TX_BCLK` 的
    `inner_bclk_ws_sel_tx_03 = AIAO_TX_CRG3 = 11`（不是 3！RX_CRG 占 0..7）；
    照抄 vendor：`AIAO_HW_SetIfAttr()` 的 MASTER 分支除了
    `AIAO_HW_SetI2SMasterClk()` 还调 `AIAO_HW_SetI2SDulicateClk()`
    （`hal_aiao_priv.c:1603-1607`、`:2029-2040`）。写后该 nibble 读回 0xb。
  - 端口使能位留住：`TX_DSP_CTRL(0x2304)` = `0x18000000`（tx_enable=1、bypass=1）；
    `TX_BUFF_RPTR(0x238c)` 随播放前进（字节偏移，实测到 0x400/0x1000）。
  - 缓冲阈值 `TX_BUFF_ALEMPTY_TH = 一个 period` 会让端口在领先量不足一个
    period 时自己清掉 tx_enable（实测 0x2304 从 0x18000000 回到 0x28000000）。
    vendor 同样取 period 值，但它启动前已有数个 period 的领先量。

### 9.3 未通的最后一段（下一个工作项）

现象：`pcmplay` 每约一个 period 报一次 xrun（6 s 内 282 次），`hw_ptr` 恒 0，
`/proc/interrupts` 里 `44: ... GIC-0 116 Level f8cd0000.audio` **计数 0**，
端口 `TX_INT_RAW(0x23a4)` 也读 0 ⇒ 周期中断从未产生。中断号 84/SPI 与 INTID 116
本身对得上（`hal_aiao_common.h:33-40` 的 `84+32`）。待查顺序：

1. 生产指针语义：`ack` 里 `appl_ptr % buffer_size` 在"整缓冲写满"时等于 0，
   硬件据此判定空缓冲并自停；需要按 SDK 的 CIRC_BUF 处理满/空歧义
   （`common/src/hal_aiao_func.c:990-1019`、`hal_aiao_priv.c:2551-2561`）。
2. `TX_TRANS_SIZE` 与实际消费量的关系：端口只吃掉 0x400 字节就停，从未达到
   一个 transition（4096 字节），因此 trans_finish 不可能置位——先解决 (1)。
3. 若中断仍不来，用 `poke_sel=3` 读 `0x23a0 INT_ENA / 0x23a8 INT_STATUS /
   0x0 COM AIAO_INT_STATUS(0x4)` 逐级定位是端口级、汇聚级还是 GIC 级。

### 9.4 测试工具与用户态缺口（实况记录）

- `boardtest/pcmplay.c`：静态交叉编译的 raw-ioctl 播放器（open/HW_REFINE/
  HW_PARAMS/SW_PARAMS/PREPARE/WRITEI_FRAMES/STATUS/DRAIN），用于替代 aplay。
  注意两个坑：未请求的 `snd_interval` 必须留 `integer=0`（照 alsa-lib 的
  `snd_interval_any()`），否则 `HW_REFINE` 直接 EINVAL；`-M <hex>` 可单选 rmask 位。
- OpenWrt 25.12.5 的 alsa-utils 在
  `releases/25.12.5/packages/arm_cortex-a15_neon-vfpv4/packages/`
  （`alsa-utils-1.2.11-r1.apk`、`alsa-lib-1.2.11-r1.apk`、`alsa-ucm-conf-1.2.11-r1.apk`）。
  两条阻塞：APKv4(`ADBd`) 是 apk-tools 专有分段容器，宿主机 tar/gzip/zstd/xz
  都解不开（文件内无 gzip/zstd/xz 魔数、无 ustar）；板上 `apk add` 会因
  `kmod-sound-core`、`libncursesw6` 在可见仓库里找不到而拒绝解析，且板子无 HTTPS
  （`wget`/`apk` 报 `Operation not permitted`）。apk 也没有
  `--force-missing-dependencies` 这个选项（25.12 的 apk 3.0.5 只有
  `--force-missing-repositories/--force-non-repository/--force-overwrite` 等）。
  因此本板验证先用 pcmplay；重打 initramfs 需要先把包内文件弄出来
  （在板上用 `apk` 装完再 `tar` 回收是可行路径，但要先解决依赖解析）。
### 9.5 h144 实测追加（下一轮的起点）

阈值改成一帧后仍然不通，实测数据（`/tmp/pp` = pcmplay，h144，SLAPCHECK=il144）：

- 放音中读 `TX_INT_RAW(0x23a4) = 0x9a` = bit1 tx_empty、bit3 bfifo_empty、
  bit4 ififo_empty、bit7 dat_break；**bit0 trans_finish 从不出现**。
  `TX_INT_STATUS(0x23a8) = 0`、`COM AIAO_INT_STATUS(0x4) = 0`、
  `COM AIAO_INT_ENA(0x0) = 0`（尽管驱动 trigger START 里写了 `BIT(16+3)`）。
- `TX_BUFF_WPTR == TX_BUFF_RPTR`（观测到 0xc00/0xc00、0x400、0x800、0x1000 各次），
  即端口把已提交的量吃干后停住；提交量每次只到 256～768 帧，
  说明生产端每轮只写了一点点就再没推进（hw_ptr 不更新 → WRITEI 等不到
  `avail_min`，而 hw_ptr 只由周期中断推进）。
- 手动在放音中 poke `0x0000 = 0x00080000` 与 `0x23a0 = 1` **能写进并能读回**
  （读回 0x00080000 / 0x00000001）⇒ COM+0x0 不是只读，也不需要 SW_KEY 解锁
  （SDK 全树无 SW_KEY 写点）。但 poke 之后 `/proc/interrupts` 的
  `43: GIC-0 116 f8cd0000.audio` 计数仍为 0。
- `ALEMPTY_TH = period` 会提前自停（h143 实测：剩 2048/3072 字节时
  `TX_DSP_CTRL` 从 0x18000000 掉回 0x28000000），故 h144 已改为"一帧"。
  改后 `tx_enable` 能留住，但仍未产生 trans_finish。

下一轮按这个顺序查（都是免重编的 poke/读）：

1. 先确认 trans_finish 的触发条件到底是"消费够 TRANS_SIZE 字节"还是
   "读指针回绕/水位"：把 `TX_TRANS_SIZE(0x2394)` poke 成很小值（如 64 字节）
   再放音，看 `0x23a4` 是否出现 bit0、`43:` 计数是否动。
   若出现，说明只是阈值/节奏问题，把驱动的 TRANS_SIZE 与 period 关系调整即可。
2. 若 bit0 仍不出现：查 `0x23a0` 是否被驱动 START 写后再被清（在 START 后
   立刻 peek 一次即可判定），以及 COM+0x0 在 START 之后是否真为 0x80000
   （h144 观察到的是 0，而 poke 能留住——怀疑 START 当时根本没跑到，
   或随后有一次 STOP 把它清了；pcmplay 的 xrun 恢复只做 PREPARE，不再 START，
   而 `.prepare` 目前不动 COM，需要核对）。
3. 若中断链都正常但端口饿死（WPTR 长期 == RPTR）：转查 ring 引擎的取数节拍，
   即 `TX_BCLK_CNT(0x2324)/TX_WS_CNT(0x2320)` 在 h137 那次读到的都是 0，
   需要确认这两个计数器在本 IP 是否需要使能位，或者主时钟根本没跑
   （若是这样，`CFG0=0x333333/CFG1=0x133` 的 CRG 设置仍不产出 bclk，
   要回看 `AIAO_SWITCH_TX_BCLK` 之外的端口时钟选择，例如是否需要
   `AIAO_HW_SetTxCrg...` 之外的另一级开关）。

### 9.6 h145/h146 追加（下一步的两个嫌疑）

- h145 把中断使能挪到 `.prepare` 后：放音中 `COM AIAO_INT_ENA=0x00080000`、
  `TX_INT_ENA=0x23a0=1` 都留得住；把 `TX_TRANS_SIZE(0x2394)` 手动压到 0x40 之后
  **`/proc/interrupts` 的 `44: GIC-0 116 f8cd0000.audio` 从 0 变 1**
  ⇒ 端口→汇聚→GIC→handler 整条链是通的，唯一缺口是 trans_finish 在
  `TRANS_SIZE = period_bytes(4096)` 下从不置位。
- h146 又给 `ack` 加了"提交量不超过 rptr + 缓冲-1 帧"的保护（消满/空歧义），
  现象不变：4 s 播放 188 次 xrun、`hw_ptr=0`、IRQ 计数 0、
  `TX_INT_RAW=0x9a`（只有 empty/dat_break 类），`TX_INT_STATUS(0x23a8)=0`。
- 因此下一步优先验 **WPTR/RPTR/TRANS_SIZE 的量纲**：观测到的
  `0x2388/0x238c` 一直是 0x400/0x800/0xc00/0x1000 这一串 2 的幂，
  且总与提交量恰好相等；若这两个指针（以及 TRANS_SIZE）实际以**帧**或**字(4B)**
  计数，那么按字节写就等于把提交量缩小了 4 倍，端口必然在跨过一个
  4096 字节的 transition 之前饿死 —— 与"trans_finish 从不置位、
  但 TRANS_SIZE=64 时立刻有中断"完全自洽。
  判据（都免重编）：放音中 `peek 0x238c` 两次，与同期 pcmplay 已写帧数对照；
  或直接把 `0x2394` 写成 0x400（=1024 帧=4096 字节）与 0x1000 各试一次，
  看 IRQ 计数动不动；再把 `0x2388` 按"帧"含义写（如 8192 帧写 8192）看 RPTR 是否随之走满一圈。

### 9.7 h148–h151 开环对照（下一步要跑的矩阵，含已测点）

手动开环（流关闭、只 poke 寄存器：`RPTR=0` → `WPTR=A` → `0x2304=0x18000000` → 读 RPTR）：

| announce A | TRANS_SIZE | RPTR 结果 | 结论 |
|---|---|---|---|
| 0x7fff | 0x400 | 0x7fe0（吃满）| 端口/时钟/取数正常 |
| 0x1000 | 0x400 | 0x1000（吃满）| 同上 |
| 0x4000 | 0x400 | 0x4000（吃满）| 同上 |
| 0x800 | 0x400 | **0**（不取数）| 存在绝对水位：A=2048 不取，A=4096 取 |

ALSA 流下（h148–h151 的 `.ack` 日志）：`state 2`（PREPARED）、`appl 1024 hw 0 rptr 0 wptr 4096 ahead 4096`
以约 0.6 ms 周期**重复同一组值** ⇒ 应用在 -EPIPE/`PREPARE` 里打转，第二轮 period 从未提交，
所以 announce 始终 ≤ 4096 字节，落在上面那个"绝对水位"边界上（4096 恰好是已测的最小可取数值，
而 2048 明确不取）。

下一步（顺序即优先级）：

1. 把开环矩阵补全成 `A ∈ {1024,2048,3072,4096,6144,8192}` × `TRANS ∈ {64,1024,4096}`，
   每次都在**全新 hw_params 之后、流未启动**时做（避免历史状态污染）。
   目的：定出"端口开始取数"的真实条件（绝对字节水位？还是 A 与 TRANS 的倍数关系？）。
2. 若结论是"A 必须 ≥ 4096 之类的绝对量"，驱动侧按 `start_threshold`/`avail_min` 处理不可靠
   （用户态可控），稳妥做法是把 `TX_TRANS_SIZE` 设为**小于一个 period**（如 period/2）
   并使 `.ack` 只在 `ahead >= 一个安全水位` 时才使能端口；
   `pointer()` 与 period 边界仍按 RPTR 计算，ALSA 允许一 period 内多次 `period_elapsed`。
3. 若结论是"A 必须 > TRANS"，则维持 `TRANS = period_bytes`，
   并在 `.ack` 里用严格 `ahead > period` 使能（h149 的做法），
   同时把 pcmplay 的 `start_threshold` 提到 3~4 period 复现真实播放器的领先量，
   验证 `appl` 能否连写两轮。

代码现状：`.ack` 门槛在 h151 是 `ahead >= period`；`pr_info_ratelimited` 的诊断行
**验证通过后必须删除**（连同 `histb-vodump.c` 的 sel=3 区域只用于诊断，模块本身不上游）。

### 9.8 根因定位（h153 实板，2026-10-06）：RPTR 是预取指针，不是播放指针

三条实测（都在 `mv100-hdmi` / h153，TX3 = 0x2300 窗口，48 kHz 立体声 16 bit，
period=1024 帧=4096 B，buffer=8192 帧=32768 B）：

1. **`.ack` 每次使能后端口立刻吃满提交量**。放音中 `TX_BUFF_RPTR(0x238c) = 0x1000`
   恒等于提交量，而 `.ack` 的调用节奏是 0.6 ms（同一组 `appl 1024 / wptr 4096`
   重复上千次）。4096 B 在 48 kHz 立体声 16 bit 下是 21 ms 的音频，
   所以 RPTR 记录的是"已从 DDR 预取的字节"，不是"已移出的帧"。
2. **这直接解释了 h143 以来的每次 xrun**。`pointer()` 当时按 RPTR 换算帧，
   于是 `hw_ptr` 立刻追上 `appl_ptr`，`snd_pcm_update_state()` 的播放判据
   `avail >= stop_threshold`（`sound/core/pcm_lib.c:223`，stop_threshold
   默认取 buffer 大小）在第一拍就成立 → EPIPE → 用户态 PREPARE 重来，
   形成 0.6 ms 一轮的抖动。第一轮 xrun 恰好发生在"写完 1 个 period"处，
   与"预取快于移出"这一条互相印证。
3. **trans_finish 中断本身是好的，但它量的也是预取字节**。开环使能后手动扫
   `TX_TRANS_SIZE(0x2394)`（每个值观察 2 s，取 `/proc/interrupts` 的 GIC 116 计数差）：

   | TRANS_SIZE | 2 s 内中断增量 |
   |---|---|
   | 0x20 | 25419 |
   | 0x40 | 20752 |
   | 0x100 | 16180 |
   | 0x400 | 6837 |
   | 0x800 | 3 |
   | 0x1000 | 0 |

   计数随 TRANS 变小而暴涨、在 0x800 处塌到近 0，正是"每预取 TRANS 字节发一次"
   的表现：抖动循环里端口每轮只被允许跑约 0.6 ms（≈115 B 的移出量），
   跨不过 0x800 的门槛，却能反复跨过 0x40。中断链（端口→COM 汇聚→GIC→handler）
   在 h145 已单独验通，本表不构成对该链的怀疑。

配套查到的 vendor 事实（决定性，改变了设计取向）：

- AIAO 的 ALSA 胶水在有 hrtimer 支撑时**根本不用 trans_finish 打周期**：
  `aiao_v1_1/Makefile:132-134` 只要 `CFG_HI_SND_ALSA_AO_SUPPORT=y` 就
  `-DHI_ALSA_HRTIMER_SUPPORT`，走 `SndPeroidCallback`（`alsa/dsp_mix_v2/hiaiao_dma.c:226`），
  其中 `snd_pcm_period_elapsed()` 由定时器驱动，位置按
  `u32HwPointer = u32HadSent % periods` 计数（同文件 :246）。
- 它的 `.pointer`（`hi_dma_pointer`，同文件 :642-665）返回的就是这个计数值
  （`bytes_offset = u32HwPointer * period_bytes`），**不读 RPTR**。
- 只有非 hrtimer 编译分支才把 trans_finish ISR 当作周期源，且那里推进的位置
  仍是"每中断加一个 period_bytes"（`IsrAIFunc` 里 `readpos = period_size * frame_size`），
  即两种分支都是软件计数。
- `TX_WS_CNT(0x2320)/TX_BCLK_CNT(0x2324)` 是 24 位调试计数器
  （`common/include/hal_aiao_priv.h:760-782`，SDK 只在 debug 读取处用
  `hal_aiao_priv.c:2591/2603`）。流关闭、`TX_DSP_CTRL=0x28000000` 时四个采样点
  全等（端口不工作则计数器冻结），重新使能后 BCLK 计数会回落，
  没有可靠的清零/回绕语义，故不当位置源。

### 9.9 h154 的周期源（当前代码状态）

- 删除 trans_finish 中断使用：不注册 IRQ、不写 `AIAO_INT_ENA/TX_INT_ENA/TX_INT_CLR`，
  DTS 的 `interrupts` 属性按 binding 里"optional（可轮询运行）"保留描述。
- 新增 `hrtimer tick`：`.trigger` START 起、STOP 停，间隔
  `period_frames * NSEC_PER_SEC / rate`，每次 `aiao->pos += period_size`
  （按 buffer 回绕）后 `snd_pcm_period_elapsed()`；`pointer()` 返回 `aiao->pos`，
  `.prepare` 把 `pos` 归零。与 vendor 的形状一致（软件计数 + 定时器）。
- `.ack` 仍负责喂环：按 `appl_ptr` 提交 `TX_BUFF_WPTR`（留 1 帧不提交以消
  满/空歧义），只要 `ahead` 非零就写 `TX_ENABLE|TX_BYPASS_EN`。
- `TX_TRANS_SIZE` 恢复 SDK 默认（= period 字节数，`AIAO_DF_PeriodBufSize 2048`，
  `common/include/hal_aiao_common.h:159`），`TX_BUFF_ALEMPTY_TH` 保持 1 帧，
  仅作端口自停水位。
- 删掉全部临时 `pr_info` 诊断（`.ack` 两行、`.trigger` 一行）。

板上判据（下一个工作项，按 §8）：48k/44.1k 连续播放无 xrun、`hw_ptr` 随时间线性推进、
`TX_WS_CNT` 在放音期间按 48 kHz 递增，最终 HDMI 端听得到音频。

### 9.10 对 §9.8 两条结论的更正（h154/h155 实板，2026-10-06）

先说结论：**§9.8 的第 1、2 条作废**，它们建立在被测试程序污染的证据上；第 3 条（TRANS 扫描表）
仍然成立，但对它的解释要改。

1. **pcmplay 的 `sw.stop_threshold = 0` 是测试程序缺陷，不是驱动现象**。
   内核原样采纳用户态给的阈值（`sound/core/pcm_native.c` 的 `snd_pcm_sw_params()`：
   `runtime->stop_threshold = params->stop_threshold;`，没有 0→buffer_size 的兜底），
   而播放侧 xrun 判据是 `avail >= stop_threshold`（`sound/core/pcm_lib.c:223`）。
   `stop_threshold = 0` 使该式恒真，于是**每一次 `update_state` 都判 xrun**：
   h143-h154 期间看到的"xrun at frame 1024/2048/…、hw_ptr 恒 0、wall≈0"
   全部由这一行造成，与端口是否吃数、中断是否来都无关。alsa-lib 的正常默认是
   `stop_threshold = buffer_size`，pcmplay 已改成从 refine 回来的 BUFFER_SIZE 取该值，
   并新增 xrun 计数与 wall 秒数输出（`boardtest/pcmplay.c`）。
   连带作废的还有 §9.8 第 2 条里"这解释了每次 xrun"的推断。
2. **RPTR 到底是预取指针还是播放指针，本指导目前没有证据判定**。
   §9.8 第 1 条写的"提交后立刻吃满"不成立：h153 的 `.ack` 日志里 `rptr 0 / wptr 4096`
   恰恰说明提交后 0.6 ms 内端口**还读不到数**，与"预取指针"相反；
   而开环表里 `announce 0x7fff → rptr 0x7fe0` 是在 poke 之后若干秒才读的，
   32 KB 在 48 kHz 立体声 16 bit 下是 170 ms 音频，那点延迟无法区分两种语义。
   判定方法已就绪（`boardtest/rptr-track.sh`：健康流上每秒同时采
   `TX_WS_CNT/TX_BUFF_RPTR/TX_BUFF_WPTR` 与 `substatus` 的 hw/appl 指针），
   下一版内核跑一次即可定论；定论前驱动不做"RPTR 当位置源"的假设。
3. **h154 把周期源改成 hrtimer 后出现 RCU stall，根因是在 trigger 持锁里等待回调**：
   `snd_pcm_*_ioctl` 的 trigger 路径带着 stream lock（即 `runtime->lock`），
   而 `snd_pcm_period_elapsed()` 也要拿同一把锁；`.trigger(STOP)` 里调
   `hrtimer_cancel()` 会等正在运行的回调，回调等锁 → 死锁。
   实板症状：uptime 480 s 起 `rcu: INFO: rcu_sched detected stalls on CPUs/tasks
   1-...0`，且 `After 10 seconds, these CPUS still haven't responded to the NMI: 1`，
   `reboot` 都跑不动，只能断电。
   h155 按树内参考实现 `sound/drivers/dummy.c:385-423` 改：
   定时改用 `HRTIMER_MODE_REL_SOFT` 到期，`running` 标志用
   `WRITE_ONCE/READ_ONCE` 读写，STOP 路径只做 `hrtimer_try_to_cancel()`（不等回调），
   `period_elapsed()` 之后再查一次 `running`（它可能已把流停下来），
   真正会等待的 `hrtimer_cancel()` 只放在 `remove()`（不持 stream lock）里。
   这条同时解释了为什么驱动里绝不能在这种上下文用 `synchronize_*`/`flush_work`。
4. 驱动侧仍保留的软件周期源，其正当性来自 vendor 生产路径本身
   （`aiao_v1_1/Makefile:132-134` + `hiaiao_dma.c:226/246/642-665`），
   不是来自 §9.8 那条已被推翻的测量；这一点在代码注释里也已改成按出处陈述。
5. 待办不变：h155 上板跑健康播放（xrun=0、wall≈时长）、
   用 rptr-track.sh 定 RPTR 语义（若确认是播放指针，就改回 `pointer() = RPTR` +
   trans_finish 中断，那才是上游会接受的形式），最后 HDMI 端听声。

### 9.11 验收通过（h159 实板，2026-10-06）

周期源与位置源的最终形状（都是"定时器节拍 + 软件计数位置"，与 vendor 生产路径一致）：

- `.trigger` 起 `hrtimer`（`HRTIMER_MODE_REL_SOFT`），间隔
  `period_frames * NSEC_PER_SEC / rate`；每次 tick 让 `pos += period_size`
  并按 buffer 回绕，然后 `snd_pcm_period_elapsed()`。
- `pointer()` 返回 `pos`；`.prepare` 归零。
- STOP 路径只做 `hrtimer_try_to_cancel()`（不等回调）+
  `readl_relaxed_poll_timeout_atomic()` 等 `tx_disable_done`。
- 不注册 IRQ、不碰 `AIAO_INT_ENA/TX_INT_ENA/TX_INT_CLR`；
  binding 里 `interrupts` 保持 optional 并写明实测原因。

板上判据（`boardtest/atest4.sh`，`/tmp/pp` = `boardtest/pcmplay.c`）：

| 请求 | 实际 refine | wall | xrun | 结束状态 |
|---|---|---|---|---|
| 48k 1024x8，10 s | 48000/1024/8 | 10.008 s | 0 | running，hw_ptr 472064，delay 7 us |
| 44.1k 1024x8，4 s | 44100/1024/8 | 4.020 s | 0 | running，hw_ptr 168960 |
| 48k 256x4（buffer 1024 帧），4 s | 同 | 4.004 s | 0 | running，hw_ptr 190976 |
| 32k 512x8，4 s | 同 | 4.004 s | 0 | running，hw_ptr 123904 |

dmesg 无 `invalid position`、无 `timeout`、无 `bad: scheduling from the idle thread!`、
无 RCU stall（h154 的那次死锁已由"try_to_cancel + soft 到期 + 原子轮询"三项消掉）。

HDMI 端出声的客观判据（§8 要求的完成判据）：

- 采集路径：板子 HDMI 输出 → MACROSILICON MS2130 采集卡 → 宿主机 ALSA
  `hw:1,0`（48 kHz 立体声），用 `boardtest/capaudio.py`（ffmpeg 取 raw s16le）测峰值、
  RMS、过零数。
- 静默基线：板子不放音时 `peak 0 / rms 0`（数字零）。
- 放音时（`/tmp/pp -d 5 -v 20000`，前半 440 Hz、后半 880 Hz）：
  **L/R 峰值正好 20000 = 命令幅度**，20 s 窗口内过零总数 6581；
  理论值 = 440 Hz×2×2.5 s + 880 Hz×2×2.5 s = 6600，吻合到 0.3%。
  ⇒ 音频确实经 AIAO TX3 → SiI9134 → HDMI 到达 sink，且频率结构与激励一致，
  不是噪声或串扰。

测试台自身的一条重要更正（避免下次再踩）：`pcmplay` 原先写
`sw.stop_threshold = 0`，而内核 `snd_pcm_sw_params()` 原样采纳
（`sound/core/pcm_native.c`），播放判据是 `avail >= stop_threshold`
（`sound/core/pcm_lib.c:223`）⇒ 恒真，任何驱动都会被判 xrun。
alsa-lib 的默认是 `stop_threshold = buffer_size`，脚本已照此修正并输出 xrun 计数与 wall 秒数。

代码现状：驱动 `sound/soc/hisilicon/histb-aiao.c`（463 行）
`checkpatch --strict` = 0 error / 0 warning / 0 check；
`make W=1` 目录级编译 0 warning；CRG 侧改动仍在 `drivers/clk/hisilicon/crg-hi3798mv100.c`
（`HI3798MV100_AIAO_CLK` gate + `aiaoclk_skipcfg/loaden` 初始化），驱动不碰 SoC 级寄存器。

