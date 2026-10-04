# HDMI（SiI9134 / histb-hdmi-14）移植等价性审查

审查对象：`port/hdmi-av` 分支 `drivers/gpu/drm/bridge/{sii9134.c,sii9134.h,histb-hdmi-14.c}` +
`arch/arm/boot/dts/hisilicon/{hi3798.dtsi,hi3798mv100-ec6108v9c.dts}`。
SDK 参照：`HiSTBLinuxV100R005C00SPC050/source/msp/drv/hdmi/hdmi_1_4/`（含 `hdmi_drv/` SiI9134 原厂驱动与 `drv_hdmi.c` 系统层）。
下文 `移植侧` 均为该分支 file:line；`SDK/` 为 SDK 根。

## 一行结论

**需修一处**：DTS 两根控制器复位线用了 mv200 的 CRG67 位号（bit7/bit6 在 mv100 是 reserved），控制器复位从未真正施加，等价于丢失 SDK `SI_HW_ResetHDMITX()` 整个复位序列；其余寄存器层（bank 映射、PHY、中断、音频、DE 数值）逐项与 SDK 对上，无第二处 blocker。

## Blocker 表（按 probe 链排序）

| 移植位置 | SDK 参照 | 现象 | 修复方向 |
|---|---|---|---|
| `hi3798.dtsi:473` `resets = <&crg 0x10c 7>, <&crg 0x10c 6>, <&crg 0x110 4>` | `SDK/common/drv/include/hi3798mv100/hi_reg_crg.h:886-899`（CRG67：bit0-5 cken，bit7..6 **reserved**，bit8=`hdmitx_ctrl_bus_srst_req`，bit9=`hdmitx_ctrl_srst_req`）；`:913-915` CRG68 bit4=`hdmitx_phy_srst_req` | bit7/bit6 在 mv100 是 reserved（那是 mv200 `hi3798mv200/hi_reg_crg.h` 的 CRG67 位号）。写入 reserved 位 → 控制器/bus 复位从未施加；SDK `si_hdmitx.c:164-197` 的复位序列（assert→1ms→先放 PHY→配 PHY→放控制器→10ms）整体丢失，寄存器态依赖 bootloader | 改回 `<&crg 0x10c 9>, <&crg 0x10c 8>`（phy bit4 正确）。注意这是 `0df482f31844 "fix HDMI resets"` 自己引入的回归：该提交把 dma-rebase 分支上本来正确的 9/8 改成了 7/6 |

## Major

1. **复位时序与 SDK 顺序相反、缺级间延时**（`histb-hdmi-14.c:62-79` vs `SDK/hdmi_drv/si_hdmitx.c:181-197`）。SDK：assert 控制器+PHY → 1ms → **先放 PHY** → 配 PHY → 1ms → 放控制器 → 10ms 再碰 TPI；移植：三路一次性 deassert（控制器先于 PHY），PHY 配置在任何复位保持期之外（`sii9134.c:1219-1220` 先 hw_init 后 phy_init），无 1ms/10ms。建议 deassert 顺序改为 phy→bus→base，phy 配置提前、base 放开后补 10ms。修正 blocker 后此条才会显现。
2. **HPD 无去抖/状态机**（`sii9134.c:550-571` vs `SDK/hdmi_intf_k.c:315-348` 100ms 轮询线程、`si_isr.c:340-373` 防弹跳注释、`si_timer.c:507-546` EDID 重读 5 次 + 200ms 延时再报）。移植线程化 ISR 清中断后立即单读 `TX_STAT` 报 hpd_notify，弹跳期会连发。建议加去抖窗口。
3. **RSEN 语义不等价**（`sii9134.c:559-568`）：RSEN 中断被当作 connector status 直接上报；SDK 区分 RSEN_CONNECT/DISCONNECT 且断开不改插拔状态（`si_isr.c:361-390`）。
4. **同步编程机制分歧（需上板/VO 侧确认）**：SDK 在 mv100 上电时**关闭 DE 生成器**（`drv_hdmi.c:4471` 写 0，`si_eeprom.c:151` 同），运行时走 656 同步再生路径（`drv_hdmi.c:4640` → `si_de.c:92-110` 写 0x3A/0x3C/0x40/0x42/0x44/0x46/0x47/0x3E）；移植走 DE 生成器（`sii9134.c:731-747` 置 0x33 bit6）。DE 表数值本身两边一致（1080p60：192/41/1920/1080；720p60：260/25/1280/720 逐值核对通过），但移植侧一个 656 寄存器都没写。若 VO 输出带内嵌同步的 656 风格数据，此条升级为 blocker；若 VO 送独立 sync+DE 则是有意裁剪。**未确认**——取决于移植树 VO 驱动的输出形态。
5. **色彩空间声明分歧**：SDK 强制 YCbCr444 进 HDMI IP（`drv_hdmi.c:4546-4587`），AVI infoframe 声明 YCbCr444；移植按 RGB 直通（`sii9134.c:711-713`、`:778`）。寄存器终值两边都是 0（旁路），差异在 AVI colorspace 字节。需确认 VO 实际送 RGB 还是 YCbCr。

## Minor / 观察项

- 位长 fallback：移植未知位宽用 0x02（16bit，`sii9134.c:951-953`），SDK 用 0x0f（N/A，`drv_hdmi.c:4383-4385`）。建议改 0x0f 或 -EINVAL。
- `INT_CNTRL`(0x7B)=0x02 只在 probe 写一次（`sii9134.c:630`）；SDK 每次复位/进中断都重写（`si_hdmitx.c:206`、`hdmi_intf_k.c:159`，注明"电平有效"）。建议 ISR 内防御性重写。
- `INT_STATE`(0x70)/INT2/INT3 及 BIPHASE/DROP_SAMPLE→`SI_Set_N_Fs` 路径未移植（以 audio_startup FIFO flush 替代，`sii9134.c:962-964`）。
- init 后 SDK 会 `SI_TX_PHY_PowerDown(TRUE)` 关 PHY 待 HPD 再上电（`drv_hdmi.c:939-944`）；移植 PWD 写一次不清（`sii9134.c:257,260-266`），无信号时 PLL 常开（功耗差异）。
- deep-color/dither 三寄存器（0x69、0x4F、0x4A[5]）SDK 在 24bit 路径仍写（`si_txvidp.c:156-235`），移植不碰；8bit 输入下通常不可见。
- `TX_VID_CTRL` ICLK（pixel replication，0x48[1:0]）定义了宏但从不写（`sii9134.c:71-72`）；非 repeat 下两边终值一致。
- clocks 完全不建模（DTS 注释掉 + 驱动 optional，自洽），但 CRG67 bit0-5 / CRG68 bit0 的 cken 门控没有任何使能路径，依赖 bootloader。
- `gpio4.4/4.5`（i2c_hdmi）的 pinctrl range 行被注释（`ec6108v9c.dts:129`），依赖复用默认值。
- runtime_suspend/resume 不对称（resume 不重跑 hw_init/phy_init），当前被常驻 `pm_runtime_get` 掩盖（死代码，`histb-hdmi-14.c:50-79,154-161`）。
- DDC_DELAY 写 0x14=20（`sii9134.c:495,633`），SDK 默认 0x1e=30（`drv_global.c:84`）。
- 640x350/640x400 的 CSCSEL：SDK 走 709，移植按 `hdisplay>720||vdisplay>576` 判 601（`sii9134.c:715-721` vs `drv_hdmi.c:4683-4703`）。

## 有意裁剪（非问题，注明功能面损失）

- HDCP、EEPROM 自定义时序、SPD/MPEG/Vendor(3D/4K) InfoFrame：整层不移植，HDCP 损失为只能输出非加密内容。
- CEC（I2C slave 0xC0，`si_cec_reg.h` page 8）：无任何寄存器与 regmap，CEC 收发不可用。
- SPDIF/DSD/HBR/压缩码流音频、降采样：`sii9134.c:1025` 显式拒收非 I2S-PCM，SPDIF 的 PHY_AUD 调整随之合法省略（`sii9134.c:249-251`，前提成立：`drv_hdmi.c:4410-4413` I2S 路径确实不动 PHY_AUD）。
- 隔行（`sii9134.c:597-606` 显式拒绝）、Deep Color 30/36bit（165MHz/8bit 封顶自洽）、N_Val_4K 表（4K 不可达）。

## 已核对无误（逐条现场推导过，别 refactor 掉）

- **bank 展平映射**：bank0=base+reg*4、bank1=base+0x400、PHY=base+0x1800（32 位），与 `drv_reg_proc.h:27-29`、`drv_reg_proc.c:139-145`、`si_phy.c:17` 一致；regmap 8bit/shift2 只用于 bank0/1，PHY 独立 32bit regmap（`histb-hdmi-14.c:169-183`）——正确做法，PHY 值未被 8bit regmap 截断。
- **寄存器宏全表**：bank0/bank1 用到的每个宏（SWRST 0x05、SYS_CTRL1 0x08、STAT 0x09、DATA_CTRL 0x0D、DE 组 0x32-0x38、VID_CTRL 0x48、VID_MODE 0x4A、INT1 0x71/MASK 0x76/CNTRL 0x7B、MDDC 0xED-0xF5、ACR 0x01/N_SVAL 0x03、AUD_MODE 0x14、I2S 0x1D/0x24/CHST、AUDP_TXCTRL 0x2F、INF_CTRL1 0x3E、AVI_IF 0x40/AUD_IF 0x80）与 `si_defstx.h`/`si_defsmddc.h`/`si_infofrm.h` 逐值一致；位域 shift/mask（DE 极性 bit[5:4]、CSCSEL 0x10、INF en/rpt 掩码公式 `RptBitMsk=(bits>>1)&bits` 等）一致。
- **PHY**：init 序列值 OE=0/AUD=2/PLL1=2/PLL2=9/DRV=1/CLK=0/PWD=1（power up last）与 `si_phy.c:248-266` mv100 分支逐项一致；swing 分档 ≤27→0/≤165→1/else 3 一致。
- **音频**：N 表（6272/6144/4096/12544/12288/25088/24576）与 `si_vmtables.c:15-30` 逐项一致；Fs 码、位长码与 `drv_hdmi.c:4288-4392` 同表；CHST 组合式、AUD_MODE/SD_0_3_EN/layout 序列与 `si_audio.c:146-207` 一致；CTS 由硬件 N/CTS 生成（两侧都只写 N 使能 bit1），机制等价；Audio InfoFrame 用内核 `hdmi_audio_infoframe_pack`（CTA-861 同布局、同校验、14 字节写序）。
- **中断**：INTID 120 ≡ GIC_SPI 88（`hdmi_intf_k.c:138` `HDMI_IRQ_NUM=(88+32)`，vendor dts `<0 88 4>`），level-high 一致；W1C 语义（`si_isr.c:325-331` 读回写）与 mask 写 1 使能一致。
- **DTS 资源**：reg 0x10000 覆盖 bank0/0x400/0x800/0x1800；CRG67=0x10c、CRG68=0x110、基址 0xf8a22000、phy bit4 全对（仅 bit7/6 错，见 blocker）；`ddc-i2c-bus`/`#sound-dai-cells`/sound 节点引用自洽；remove/PM 配平（pm_runtime_get 常驻、devm 逆序释放）正确。
- **InfoFrame ready 轮询**：60 次超时 + 强推，与 `si_infofrm.c:25-65` 等价。

## 本次没做什么

未编译、未过 checkpatch/dtbs_check 等上游门、未上板。第 4/5 条 major（656 vs DE、RGB vs YCbCr444）需要结合移植树 VO 驱动的实际输出形态或上机才能定性；上板验收流程见 `ec6108-board-test` skill。
