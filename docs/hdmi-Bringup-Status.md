# HDMI 移植上机调试：状态与进度（下轮续接）

> 2026-10-01 结束时的快照。上板操作流程、取锁协议、推板方法见 `ec6108-board-test` skill；
> SDK 侧事实见 `histb-port-facts` skill 第 6 条（与本文互为补充，本文偏"过程与下一步"）。

## 当前状态（一行）

**mv100-hdmi 分支 21 提交，实板稳定运行 card0/HDMI-A-1/fb0（1024x768 fbcon），EDID 可实读；
唯一遗留：SiI9134 TPI 寄存器"写丢弃"未解 → TMDS PLL 不锁 → 无视频输出。**

## 已解决（本轮全部实板验证，提交在 linux-mv100 `mv100-hdmi`）

| 提交 | 内容 |
|---|---|
| fd84207b | DTS 复位位 mv200→mv100（CRG67 bit9/bit8，phy bit4） |
| 31d26db8 | 7.3 原子 API 适配（drm_atomic_commit / atomic_create_state） |
| 0197c25c | bridge 改 devm_drm_bridge_alloc()（refcount 0 use-after-free） |
| 2c5d31a4 | bridge.type=HDMIA（7.3 connector type 检查） |
| 5b638428 | Kconfig select DRM_DISPLAY_HELPER |
| 58f5c031+4cf73b1a+d67b94f3 | fbdev（DRM_CLIENT_SELECTION + drm_client_setup）、regmap 分 bank 命名、ARGB 格式撤除 |
| 1dedeaba | regmap max_register（debugfs 可全量 dump 三 bank） |
| 3bd23228 | **TPI 只接受 32 位写**（val_bits=8 全丢）；CRG67=0x3f/CRG68=0x1 补 SDK release 门控；INT 输出电平型会风暴→改轮询（INT_CNTRL=0，不注册 IRQ） |
| 1604f584 | 软复位后重写 TX_SYS_CTRL1 唤醒位；VO 设备 `hotplug` sysfs 触发器 |
| 5eac303c | vendor DISP_ResetCRG() 序列（反汇编闭源 vdp_driver.o 得魔数：CRG54=(旧&0x83e00f80)\|0x4411807f，5ms vou_srst 脉冲） |

实板能力：`echo on > /sys/class/drm/card0-HDMI-A-1/status` + `echo hotplug > /sys/bus/platform/devices/f8cc0000.vo/hotplug` → EDID 实读、modes 出 VESA 表、fb0 + fbcon 1024x768。fb0 写读 64KB md5 一致。

## 未解问题：TPI 写丢弃（排除清单已穷尽）

现象：bank0/bank1 任何寄存器**写后读回不变**（0x37/0x05/0xaa/0x5a 全丢），读是活的
（0x33 读到复位默认 0x93，INT1 锁存 0x64 为真值）。TX 不唤醒 → TMDS PLL 不锁
（"TMDS clock not stable"）→ 无视频输出、vblank 计数冻结在恰好 100000。

已排除：
1. ~~8 位写宽度~~（改 32 位 regmap 后 PHY 区写正常，TPI 区仍丢）
2. ~~CRG67/68 时钟门与复位~~（补 SDK release 值后 CRG54/PHY 写生效）
3. ~~CRG54 时钟选择~~（vo_hd_clk_sel 0-3、vo_sd_clk_sel×vo_hd_clk_div 16 组合、vendor 魔数全套，全无效）
4. ~~TPI 0xC7 Access Protect 写保护~~（双写 0 解锁无效）

下一步（二选一，推荐 B）：
- ~~A. 逆向闭源 VDP_DRIVER_Initial~~ **已做（2026-10-01），结论：静态无新增**。`vdp_v2_0/hal/3798m/vdp_driver.c` 其实有源码（原记"闭源"不实），`VDP_DRIVER_Initial` 的 vo_ck_gt_en/CBM_ATTR/DHD0 默认/RdOutStd 我们驱动已全部等价实现；DISP_ResetCRG 源码与已反汇编魔数一致。vendor 内核 TX 访问层（`hdmi_1_4/drv_reg_proc.c` DRV_WriteByte_8BA）与展平映射逐条等价（基址 0xf8ce0000、32 位写低字节）；bootloader `boot/product/driver/hdmi/hdmi_1_4/hdmitx.c` 的 CRG67/68 序列与 SDK 内核相同（RMW 形式，效果等价），且 bootloader 也是从零把 TX 拉起来（`HI_UNF_HDMI_Init` → HW/SW_Reset + TX_PHY_PowerDown(HI_TRUE)），说明"完整复位序列可从冷门控状态唤醒 TX"在 vendor 侧成立——静态解释不了我们的写丢弃。静态侧只挖出三个小事实：CRG67 bit14=asclk_sel 仅 SPDIF 音频路径用（drv_hdmi.c DRV_HDMI_SetPhyAudioClk 的 SI_HW_ResetCtrl(2)=0x403f）；bootloader 用另一套 boot 地址图 0x10170000 访问 TX（内核 0xf8ce0000 无需处理）；HDMI_PinConfig 是空函数。
- **B. 冷上电抓 eMMC 回落路径的 fastboot `Press Ctrl+C to stop autoboot` 窗口（软重启永远走 USB 路径，只有冷启动随机回落；`watch_interrupt.sh` 常驻可自动接住），U-Boot `md`/`mw` 直写 TPI 地址切分"互联丢写"vs"IP 内部门控"。可顺带在 U-Boot 里 `md.l 0xf8a2210c`（CRG67）/`0xf8a22110`（CRG68）确认 USB 路径把 TX 关进门的完整现场。**

## SiI-PR-0039-F（编程指南）核对结论（2026-10-01 续，§未解问题 的"下一步 B"之前先做 §3 的 E1-E4）

文档：`/srv/blob/histb/docs/SiI9134_SiI9135的datasheet和编程指南/HDMI发送器-SiI9134-编程指南-2008.PDF`
（134 页，`pdftotext -layout` / `-raw -f N -l N` 可直接取全文；地址表在文档 p.2 Table 2）。
展平映射与 SDK 一致：bank0=0x72 → `base+reg*4`，bank1=0x7A → `base+0x400+reg*4`。

### 1. 只读锚点（"读路径是否真的通向这颗 9134"的判据）

| 寄存器 | 地址 | PDF 复位默认 | 备注 |
|---|---|---|---|
| VND_ID | 0x72:0x00/0x01 | 0x01 / 0x00 | ROM 常量 |
| DEV_ID | 0x72:0x02/0x03 | 0x34 / **0x91** | 0x90=9034，0x91=9134 |
| DEV_REV | 0x72:0x04 | 0x01 | |
| SRST | 0x72:0x05 | 0x00 | bit0 SWRST、bit1 FIFORST ✓ 与 `TX_SWRST_ADDR` 一致 |
| SYS_CTRL1 | 0x72:0x08 | **0x34** | bit0 PD#（HIGH=正常工作）、bit1 EDGE、bit2 BSEL、bit4 HEN、bit5 VEN |
| SYS_STAT | 0x72:0x09 | bit7 VLOW=1 | bit0 P_STABLE、bit1 HPD、bit2 RSEN（✓ 驱动位号正确） |
| SYS_CTRL4 | 0x72:0x0C | 0x03 | bit4:1 PLLF=0b0001、bit0 PFEN=1（PLL 滤波默认开） |
| DPD | 0x7A:0x3D | **0x0F** | bit3 RSVD=1、bit2 PDIDCK#、bit1 PDOSC、bit0 PDTOT#，三者默认全 1 |

- 移植侧 `sii9134_probe()`（sii9134.c:1190）从未读过任何 ID/状态寄存器，也没有任何日志留锚点 ⇒
  **"读是活的"这条前提目前没有可复核的证据**。
- **更正上一轮记录**：本文与 skill 第 6 条把 0x72:0x33 读到的 0x93 记作"复位默认"。按 PDF p.12，
  DE_CTRL(0x33) 默认 0x00，且 bit7 是 RSVD0（定义见 p.2 约定：read-only 且读回 0）。
  0x93 含 bit7=1 ⇒ 它不可能是"没被写过的 DE_CTRL 默认值"（方法：按 PDF 位定义逐位比对）。
  两种解释都推翻现有框架：要么读到的不是这颗 IP 的该寄存器，要么 **bootloader 成功写过 0x33**
  ——后者直接反证"TPI 写无条件丢弃"。

### 2. 文档里唯二会导致"写进去不生效"的硬件门控

1. **下电位组**（p.123 Table 17/18）：`PDTOT#=0` = 整片下电；Table 18 列出的寄存器
   （0x7A:0x3E/0x3F、0x72:0x0F-0x16 HDCP）要求 PD#、PDIDCK#、PDOSC、PDTOT# **四者全在正常工作态**才可访问。
   PD#=0 时"TMDS core 与 PLL 下电，其余寄存器仍可访问（Table 18 除外）"。
2. **IDCK 条件写**（p.51 Note，明文）：0x7A:0x3E/0x3F 的 EN 位
   "**只有在 IDCK 活跃且发射机不处于下电态时才能被置位/清零**"；IDCK 即输入像素时钟。
   推荐的固件顺序是：先写其它寄存器 → 撤销所有下电位 → 再写 EN 位。
   另外 p.13 明确：DE 发生器的只读测量寄存器只有在 HSYNC/VSYNC 活跃时才有效，
   只有 IDCK 在跑而同步缺失时计数器溢出后写 0。
3. RESET# 拉低会把 PD# 清 0（p.22 注）⇒ 每次 ctrl 硬复位后都必须重新唤醒，与 1604f584 的实测一致。

⇒ 文档层面的"IP 内部门控"候选都指向同一个现场：**0x7A:0x3D 的三位下电位 + IDCK 是否真到达 TX**。
这两项都是纯读就能定论的，成本远低于冷上电抓 U-Boot。

### 3. 决定性实验（不需要冷上电；跑着的内核 + regmap debugfs 就够）

regmap debugfs 的 `registers` 文件**可写**：`regmap_map_write_file()`
（drivers/base/regmap/regmap-debugfs.c:302-334，`echo "<reg> <value>" > registers`，
两段都是 16 进制、按 regmap 的 reg 索引语义即展平后的索引；0600，需 root）。
驱动 probe 已把三个 bank 命名为 `bank0`/`bank1`/`phy`，路径 `/sys/kernel/debug/regmap/bank0/registers`。

- **E1 读锚点**：bank0 的 00 01 02 03 04 08 0C 33 3A-3D，bank1 的 3D。
  0x02/0x03 = 34/91 ⇒ 读路径通向真 9134，"写丢弃"成立才需要往下查门控；
  读 0 ⇒ 读路径本身只在"部分位"上有效，前面的全部结论（含 0x64 中断锁存为真值）都要重估。
- **E2 字节道测试**（区分"互联丢写"与"IP 门控"，成本一条命令）：
  `echo '08 12345678' > registers` 后读 0x08。读回 0x78 ⇒ 低字节道正确且**写其实是落地的**；
  读回 0x12/0x34/0x56 ⇒ 桥取的是别的字节道（推断，需实测）；读回 00 ⇒ 写确实被丢弃。
- **E3 下电位现场**：读 bank1 0x3D。若不是 0x0F（尤其 PDTOT#/PDIDCK#=0），
  按 PDF 顺序写 `echo '3d 07' > registers`（bit3 是只读 RSVD，写不动也不该写），再回头做 E2。
- **E4 IDCK 是否到达 TX**：在 fbcon 已经接管、模式已 active 的那一刻读
  bank0 0x3A/0x3B（H_RES，只读测量值）与 0x3C/0x3D（V_RES）。
  读到与当前 mode 相符的行/场计数 ⇒ 像素时钟与同步都到了 TX，IDCK 门控这条被否；
  读 0 ⇒ **TX 根本没收到像素时钟**，则"写不进去/PLL 不锁/EN 位不动"三条现象一次性有了同一解释，
  修复方向变成 VO→HDMI 像素时钟路由与编程顺序（不是 TPI 桥）。

### 4. 本轮新增的静态核对（无新增 blocker）

- SDK 生产路径的 TX 写集与移植驱动逐条对齐；`TX_TMDS_CTRL_ADDR`(0x72:0x82) 的两处写在 SDK 里
  已被注释掉（si_hdmitx.c:570、662，"TX_TMDS_CTRL_ADDR has del in 1.4 ctrl"）⇒ 移植不写是对的。
- SDK **生产路径完全不碰 0x7A:0x3D**，只有 /proc 测试钩子 `tclk` 用它做 TMDS 时钟反转
  （hdmi_intf.c:1538 写 0x1f、1543 写 0x17）⇒ SDK 依赖复位默认 0x0F，并靠 SI_HW_ResetHDMITX()
  的 **assert→PHY 配置→release** 硬复位周期（si_hdmitx.c:164-215：CRG67=0x23f → CRG68=0x10 →
  CRG68=0x01 → `SI_TX_PHY_INIT()` → CRG67=0x3f → 10ms）把下电位恢复。
  移植侧只在 probe 写 CRG67=0x3f/CRG68=0x01（histb-hdmi-14.c:238-239），**没有做完整
  assert→release 周期，且 PHY 配置发生在 ctrl 已出复位之后**（顺序与 SDK 相反）。
  这条差异能否解释现象取决于 E3 的读数，先不改代码。
- Table 2 划出的未文档化区间（0x72:0x2C-0x31、0x4E-0x6F、0xB3-0xEB）移植驱动没有任何写操作
  （PDF p.2 明确警告不要写未文档化地址）。

## 实板 #37 结果（2026-10-01 16:35-17:10，mv100-hdmi 工作区未提交改动）

镜像：`recovery.img` md5 `869319230a219509e704888dbbd1574a`（uImage 6.53MiB），经板端 wget 写到
板上 U 盘（/dev/sda1，vfat）后软重启走 USB 路径引导；宿主机侧不插 U 盘，HTTP 8681 起在 `mkbootimg/`。
板端通路：VLAN 2001（`enp5s0.2001` 192.168.2.1/24 ↔ 板 192.168.2.10/24，板 eth0 本次 MAC
`ea:80:0c:95:81:f3`）。首两条 ping 100% 丢是链路刚 up 的正常现象，后续 ping 0.36/0.55ms。

dmesg 关键读数（新增的两行锚点）：

```
CRG before-resume: crg67=00000300 (cken=0 srst=3) crg68=00000010 (cken=0 srst=1) crg54=0401007f
CRG after-cken:   crg67=0000003f (cken=3f srst=0) crg68=00000001 (cken=1 srst=0) crg54=0411807f
before-init: id=0101:0101 rev=00 ctrl1=00 stat=00 dpd=1800 hres=0 vres=0
after-init:  id=0101:0101 rev=00 ctrl1=00 stat=00 dpd=00 hres=0 vres=0
```

- **硬复位周期没有改变任何读数**（0x23f→0x10→0x01→0x3f 全套，CRG 读回正确）。
- 锚点读数与 PDF 完全不符：ROM 常量应为 `00:01 01:00 02:34 03:91 04:01`，实测
  `00-03 全 =00000001`、`04-13 全 0`；SYS_CTRL1 应默认 0x34，实测 0。⇒ **读回来的不是寄存器内容**。
- **完整 bank0 dump 暴露读几何异常**（regmap debugfs，index=CPU 偏移/4，32 位读）：

  | index | 读回 | index | 读回 |
  |---|---|---|---|
  | 00-03 | `00000001` | 1c-1f | `00003400` |
  | 04-13 | `00000000` | 20-23 | `00000034` |
  | 14-17 | `34000000` | 24-27 | `93000000` |
  | 18-1b | `00340000` | | |

  值每 4 个 index 换一次**字节道**、每约 16 个 index 换一次**内容**；0x34（DEV_IDL）与 0x93
  都出现过但不在预期位置。⇒ 之前的"写后读回不变"是在错误的地址/字节道上读，
  **"TPI 写丢弃"这个前提不成立，改为"展平窗口的读几何与 `base+reg*4` 假设不符"**。
- **反证读路径并非全废**：本次 `ddc-i2c-bus` 未就绪，EDID 走的是芯片自带 master DDC
  （写 bank0 0xED-0xF6 + 读 FIFO），结果拿到真 EDID：connector modes =
  `1024x768 800x600 800x600 848x480 640x480`，fb0 + fbcon 128x48（=1024x768）。
  校验和/头都过 ⇒ MDDC 的写和 FIFO 读都真实生效。
- `force on + hotplug` 触发链本身能跑，但 `hotplug_store` 打出一条 WARN 回溯
  （fbcon_init/do_take_over_console 路径，属遗留项 1 的 vblank/未就绪类问题）。
- 本轮新测量：`TMDS clock not stable (stat=00 hres=0 vres=0)` —— 但读几何已判定不可信，
  **不能据此断言"IDCK 没到 TX"**。
- 工具链限制：`CONFIG_REGMAP_ALLOW_WRITE_DEBUGFS` 未开（`registers` 只读，写报 EINVAL），
  `/dev/mem` 被 STRICT_DEVMEM 挡（`dd: /dev/mem: Bad address`），板内无 devmem/i2c-tools。
  本次软重启的 fastboot 日志里**没有出现 `Press Ctrl+C to stop autoboot`**（引导走
  `Entry usb bootstrap` → fastboot 3.3.0 → 直接读 recovery.img），所以 U-Boot md/mw 路线
  在软重启下没拿到窗口。

### 下一步（一次板级循环即可定论）

在驱动里加**临时读几何探针**（probe 时打印、验证后删除）：对 `base+0x00..0x40` 做 32 位与 8 位
两种粒度读、写 `0x11223344` 到 `base+0x20` 后回读邻域。目标是把"IP 寄存器号 = CPU 偏移的什么函数、
值在哪个字节道"实测出来；拿到映射后改 regmap 几何（reg_shift/val_bits/reg_base）或 DTS ranges，
"写丢弃/PLL 不锁/infoframe 不发"三条现象会被同一解释覆盖或推翻。

## 根因（2026-10-01 17:35，实板 #38 读几何探针 + #39 验证）

**regmap 的地址算错，不是硬件丢写。** `drivers/base/regmap/regmap-mmio.c:122` 是
`writel(val, ctx->regs + reg)` —— MMIO 总线的字节偏移只由 **`.reg_stride`** 决定
（core 用 `IS_ALIGNED(reg, map->reg_stride)` 过滤），**`.reg_shift` 对 mmio 的地址不起作用**。
两个 bank 的 config 只写了 `reg_shift = 2`、没写 `reg_stride`（默认 1），
于是每次 bank0/bank1 访问的真实字节地址 = 寄存器号本身（0x08 → 偏移 0x08 而非 0x20），
**全部是非 4 字节对齐的 32 位访问**；`CONFIG_ALIGNMENT_TRAP=y` 让内核静默模拟这些访问，
模拟时把相邻两个对齐字重新拼接 ⇒ 读出按字节道旋转、写入被拆残。
这正好解释了探针看到的"每 4 个 index 换一次字节道"现象，也解释了为什么 PHY 窗口
（config 里有 `reg_stride = 4`）与 TPI 窗口表现不同。

### 实板证据（#38，raw `readl(base + 4*reg)` / `readb(base + byte)`，绕过 regmap）

| reg# | 实测 | PDF 期望 | 结论 |
|---|---|---|---|
| 0x00/0x01 | 0x01 / 0x00 | VND_ID=0x0001 | ✓ 真是这颗 IP |
| 0x02/0x03 | 0x34 / 0x93 | DEV_ID=0x34/0x91 | 低字节 ✓，高字节实测 0x93（与 PDF 的 0x91 不符，待核） |
| 0x04 | 0x20 | DEV_REV 默认 0x01 | 硅后修订号，非异常 |
| 0x08 | **0x37** | SYS_CTRL1，PD#=1 | **TX 一直是唤醒态**，24 位总线、VEN/HEN 全开 |
| 0x09 | **0x86** | SYS_STAT | VLOW+RSEN+HPD ⇒ **接收端在位且上电** |
| 0x07 | 0x64 | — | 上一轮记的"INT1(0x71) 锁存 0x64"其实是这个偏移 |
| 0x0c | 0x80 | SYS_CTRL4 | PLLF/PFEN 与 PDF 默认 0x03 不同（bootloader 改过） |

写侧同样落位：`writel(0x11223344, base+0x20)` 后读回 0x04（bit6 是保留位被丢弃，
其余按 PDF 位定义生效）⇒ **"TPI 只接受 32 位写""写被互联静默丢弃"两条结论作废**，
它们都是用错误地址测出来的。

### #39 的 `reg_stride = 4` 不对，正确形状

`reg_stride=4` 时 regmap 只接受 4 的倍数作 index（非倍数直接 -EINVAL），
所以 #39 里只有 index 8 这类读到正确值（`ctrl1=34` = PDF 复位默认 ✓），
ID/DPD 等一律 -EINVAL（读回 0）。正确做法：**三个 map 都改成"index = 字节偏移"**
（`reg_stride = 1`、bank0/bank1 `max_register = 0x3fc`、phy 相应放大），
驱动侧把寄存器号 ×4：~70 个 `*_ADDR` define 换成偏移值（或统一在 helper 里 ×4），
`sii9134_read_word/write_word` 的第二字节从 `reg + 1` 改 `reg + 4`。

读几何探针（`histb_hdmi_14_geom_probe`）已完成使命、从代码中删除。
本轮另两处改动保留：CRG 完整 assert→release 周期（histb-hdmi-14.c:235+）、
`hw_init` 显式写 bank1 0x3D=0x07 撤销下电位、probe 前后各一行锚点 dmesg。

### #42（DPD 修正版）冷上电实测：寄存器层全对，但启动 3.2s 停在 femac panic

```
before-init: id=0001:9334 rev=20 ctrl1=37 stat=86 dpd=17 hres=0 vres=0
after-init:  id=0001:9334 rev=20 ctrl1=37 stat=86 dpd=17 hres=0 vres=0
```

`dpd` 从 #41 的 `17→07` 变成 `17→17`（改成 `sii9134_modify` 只置 bit2:0 后，bit3 保持）；
锚点、唤醒态、HPD/RSEN 全部与 PDF 一致。**"冷上电能躲开 panic"这句是我说错的**——
#42 冷上电同样在 3.2s panic，所以 #41/#42 合计 5/5 必崩，#37/#38/#39 是 0/6。

panic 现场：`Unable to handle kernel NULL pointer dereference at virtual address 0000000c`，
`PC is at irq_find_matching_fwspec+0xac`，出错指令 `ldr r2, [r4, #12]` 且 r4=0；
调用链 `hisi_femac_drv_probe → platform_get_irq → of_irq_get → irq_find_matching_fwspec`。
`struct irq_domain` 的 `link`(8B)+`name`(4B) 之后正好是 `ops`@0xc ⇒ 遍历
`irq_domain_list` 时拿到了 NULL 节点，或被读的结构基址为 0。

"自定义 bus 越界写"这条已按代码核实排除（`regmap.c:780` `buf_size =
BITS_TO_BYTES(reg_bits+val_bits+pad_bits)` → bank 5 字节、phy 8 字节，与回调看到的
`count` 一致；`bus->max_raw_*` 未设时 core 记 0，批量路径禁用）。
剩余首要嫌疑是**本系列新加的 assert 段**：`CRG67=0x23f` 的 bit8 是
`hdmitx_ctrl_bus_srst`，在其它驱动并发探测时按住 1-2ms 可能让共享显示总线段上的
在途事务出错；#39 只写释放值，故没这问题。

**#44 = 二分版**（`mkbootimg/recovery.img` md5 `bbd5d6f1f11c4686b8f7a1d2c2fc6759`）：
去掉 assert（只 `CRG67=0x3f`/`CRG68=0x01` + `msleep(10)`，启动多打一行
`bringup: ctrl/phy assert skipped` 作版本标记），同时给自定义 bus 的两个回调加了
窗口边界校验与 `pr_warn_once("histb-hdmi: write out of window: off=%#x len=%zu")` 自报。
三种读法：干净进 shell ⇒ 真凶是 CRG67 bit8；出现越界告警 ⇒ 是非法 offset；
仍 panic 且无告警 ⇒ 下一步用 DTS `status="disabled"` 把整个 HDMI 驱动摘掉，
把"驱动有任何动作"与"内核/DT 本身"分开。

### #44（去掉 assert 段）实测：假设被否，方向转到 VO 提交路径

#44 启动带标记 `bringup: ctrl/phy assert skipped`，锚点仍正确（`dpd=17` 保住 bit3），
**没有** `out of window` 告警，但 3.38s 仍在同一位置崩（`ldr r2,[r4,#12]`，r4=0，
链路 `driver_register → bus_for_each_dev → … → kernel_init_freeable`，femac 的
`of_irq_get → irq_find_matching_fwspec`）。⇒ "CRG67 bit8 总线复位"与"非法 offset"两条都排除。

新首要假设：**破坏 `irq_domain_list` 的是 VO 图层 DMA 写坏内核内存**，理由是自洽的时间线：

- #37/#39 启动时 EDID 读不出来 ⇒ connector 无模式 ⇒ 日志里就是那句
  `Cannot find any crtc or sizes` ⇒ **fbdev client 无从提交 ⇒ VO 时序/图层 DMA 从未跑**；
- #41 起寄存器读写正确 ⇒ EDID 真读通 ⇒ 有模式 ⇒ boot 时 `drm_client_modeset_commit`
  真的提交（#44 日志 2.4-2.5s 有这条回溯）⇒ VO 输出与图层 DMA 开始跑；
- 崩点在 3.3s、崩在**无关驱动**遍历链表 ⇒ 典型的"别人把内核内存写花了"。

⇒ #45 = `CONFIG_DRM_FBDEV_EMULATION=n` 的二分版（管线/debugfs/hotplug 触发器都在，
只是 boot 时不提交模式），产物 `mkbootimg/recovery-nofbdev.img`
md5 `9d9f1bb94960db9023a068bfc8a32f2a`（9957376 字节），建完已把 `.config` 还原。

### 查到一个独立的移植缺陷：VO 从不调用 `drm_vblank_init()`

`drivers/gpu/drm/histb/histb-vo.c` 只在 pdev probe 里 `devm_request_irq()`（histb-vo.c:714-716）
并在 handler 里 `drm_crtc_handle_vblank()`，但**整个驱动没有任何 `drm_vblank_init()` 调用**
（全文件搜 `drm_vblank_init` 命中 0 处），`drm_crtc_vblank_on/off` 却在使用（440/449）。
后果：`drm->num_crtcs == 0`，`drm_handle_vblank()` 的第一道 `WARN_ON(pipe >= dev->num_crtcs)`
就会命中并安全返回——这正是文档里遗留项 1 的"每周期 2 条 vblank WARN"，
也意味着 vblank event / wait-vblank ioctl / 正确翻页从来没工作过。
注意：因为是安全 bail，**不能**据此断言它就是内存破坏的来源；#44 那次 femac 崩溃的
`ldr r2,[r4,#12]`（r4=0）发生在**别的驱动的 irqdomain 链表遍历**里，性质不同。

已修（未提交）：`histb_vo_bind()` 里在 mode_config 配好后加
`ret = drm_vblank_init(drm, 1);`（带 dev_err_probe），并在 `drm_crtc_helper_add()` 之后
加 `drm_crtc_vblank_reset(&vo->crtc);`。IRQ 仍在 probe 里申请、随后立刻
`VO_INTMSK=~0` 全掩蔽，所以 bind 之前不会有中断进来。

## 其他遗留（优先级降序）

1. blank/unblank 每周期 2 条 vblank WARN（`drm_handle_vblank` 在 vblank core 未就绪时被 VO IRQ 调用；probe 时 `drm_crtc_vblank_reset` 或 handler 加守卫）；
2. HPD 电平语义（STAT 0x09 读 0 但 INT1 锁存过跳变）→ 自动热插拔不可用，现靠 force on + hotplug 触发器；
3. 显示器实际画面未确认过（fbcon 已接管但用户未回报屏上有无图像；无像素时钟，预期无图）；
4. port/hdmi-av 分支上的复位位/Kconfig 修复未同步回 6.19 基座；linux-hdmi 工作区有三个未提交的 clk include 修复（6.19 编译用）。

## 环境备忘（当次有效，下次重探）

- 推板流程成熟：串口配板端 IP（192.168.2.10/24，VLAN 2001 宿主机侧已建 `ec6108-vlan2001`）→ 板端 `wget -O /mnt/recovery.img http://192.168.2.1:8681/recovery.img` → md5 → reboot。宿主机 HTTP 服务需在 `mkbootimg/` 目录起于 8681。
- 板上 U 盘当前 recovery.img：md5 `26d037570c796aa83f8fac4c3770d519`（#35 扫描版；提交版 `5eac303c` 重编后需重推）。
- 22.03.3/25.12.5 ramdisk 均无 devmem/od/i2c 工具；诊断靠 regmap debugfs + dmesg。
- ftrace function/kprobe 在此内核不工作（实测 0 命中），dynamic_debug 可用。
## 2026-10-01 深夜：色块测试与 fbdev 尺寸异常（续 #49）

现场：板子跑 #49（`dee6d1e4…`），DHD0_CTRL=80004010（intf_en=1），SYS_STAT=0x87，
TX 测得 H_RES/V_RES=1344/806，显示器已脱离待机（认到 TMDS 时钟）但无画面。

色块测试进展：

- 板端 fb0：1024x768、32bpp、stride 4096；DRM plane 状态正常（XR24、
  dma_addr=0x3f100000、gem size 3145728、fbcon 已接管 128x48）。
- `dd if=/dev/fb0` 只能读 512 KiB，`cp` 3 MiB 到 fb0 在 512 KiB 处报 ENOSPC——
  fbdev 的 screen_size 只有 512 KiB，与 gem/显示模式 3 MiB 矛盾，成因未查
  （按 drm_fbdev_dma 代码路径应为 surface_height*pitch=3 MiB；疑点记录在案）。
- /dev/mem 写 0x3f100000 报 Bad address（RAM 区拒绝；CONFIG_STRICT_DEVMEM 未开，
  成因未查）。
- 已把彩条图案（8 竖条+白框，fbtest.bin 前 512 KiB）经 dd 写入 /dev/fb0，
  读回 md5 一致（`2d763823…`），随后 `dmesg -n 1` 静默控制台，等用户看屏：
  顶部 1/6 若出现彩条 ⇒ 像素通路（G0 取数→GP0→DHD0→TX）通，问题在别处；
  仍全黑 ⇒ G0 取数/混合器路径问题，下一步在 plane update 里加 G0 寄存器回读打印，
  并核对 GP0 mux/CBM 混合器配置。
- 诊断工具备忘：板上 busybox 无 devmem applet；dd/md5sum/regmap debugfs 可用。
## 2026-10-02 凌晨：G0 写入被丢弃实锤（#50 上机）+ panic 修复（#51 待推）

接上文：用户确认 fb0 顶部彩条不可见 ⇒ 像素在 VO 内部丢失。

### 静态排除（本日完成）
- 偏移：按 hi_reg_vdp.h 机械累加复核，G0/GP0/CBM/DHD0 全部与 histb-vo-regs.h 一致
  （锚点 HDATE_VERSION=0xf000 复现；总长 0xf354）。
- 位域：U_G0_CTRL（ifmt[7:0]、surface_en[31]）、U_G0_STRIDE（surface_stride[15:0]）、
  U_GP0_CTRL、U_CBM_MIX1/ATTR、U_DHD0_CTRL 逐位核对一致。
- stride 单位：SDK hifb `G0_STRIDE = u32Stride / STRIDE_ALIGN`（16），驱动写
  pitch/16 正确；V0 层断言 `stride%16==0 且 >= iw*bpp/8` 佐证单位是字节后除 16。

### #50 上机发现（镜像 b0bf3776）
- DHD0_CTRL 读回 0xe0004010：intf_en + 新置的 cbar_sel/cbar_en（bit29/30）落上了。
- GP0_CTRL 读回 0x1032（mux1=2/mux2=3/aout=0/bout=1）落上了。
- **G0 全部寄存器读回为 0**（6000:40000000、6010/601c/6020/6030/6080/6084 全 0）
  ——`histb_vo_g0_set()` 的 IFMT/surface_en/ADDR/STRIDE/IRESO 写入全部没生效。
  这是 fb0 彩条不可见的直接解释：图层未配置，扫描无像素。
  待查方向：G0 子块时钟/复位（SDK 只有一个 VOCTRL.vo_ck_gt_en，已开）；
  #51 已加"写后立即回读"打印（pre/post/postupd/upd）一次上机即可切分
  "写入被丢"vs"回读的是 active 影子"。
- dump 缓冲 128B 不够（只打到 900c），#51 加宽到 256B。

### 34s panic（比 #49 的 WARN 更严重：init 被杀）
- #49/50 均在 33-34s 报 `WARN_ON(new_crtc_state->event)` 后，#50 直接
  init SIGSEGV → `Kernel panic - not syncing: Attempted to kill init!`。
- 修复（#51，未上机）：histb-vo.c 补 `atomic_flush`（spin_lock +
  `drm_crtc_send_vblank_event` 发掉 event，消灭 WARN 链）。

### 阻塞：板子卡死在 panic，无法远程复位
- SysRq-b（screen break + pyserial 0.8s BREAK）无效——内核未开 MAGIC_SYSRQ（推断）。
- DTR/RTS 翻转无复位效果；等 4 分钟无看门狗自重启。
- **需要用户手动断电重上电**。上电后板子会进 U 盘上的 #50（34s 后又 panic），
  只有 ~30s 窗口把 #51 推上盘：监控 console.log 出现新 boot 后立即跑推板
  （wget → cp /mnt/recovery.img → reboot）。
- 事故教训：本轮第一次推板时 /mnt 未挂载，cp 把镜像写进内存盘且 reboot 照跑，
  重启后 tmpfs 里的 /tmp/rec.img 消失，第二次 cp 静默失败 && 链断掉、无重启——
  **推板必须 mount+cp+sync+md5sum 对拍+umount 一条链**（现 push-image.sh 模板已是对的）。
## 2026-10-02 00:2x：G0 根因确认（commit 顺序）+ #54 推板，板子再被无关 oops 卡死

- 纠错：23:54 重建的 screen 没开 `-L`，console.log 停更造成"板子死在 panic"误判——
  实际 #50 panic 后 init 被 procd 拉起，板子一直活着。**screen 必须带 -L -Logfile。**

- #53（=#51 内容，g0 写后回读 + atomic_flush）上机，决定性读数：
  - 早期 plane update（commit_planes 先于 modeset_enables，见
    drm_atomic_helper.c:1989 commit_tail 顺序）时 G0 全 0；
  - atomic_enable 尾部重写后 `g0 rdbk: pre=40000068 post=c0000068 postupd=c0000068`
    ——**G0 写入在 VO pipeline 起来之后完全正常**。
  - 结论：G0 图层寄存器在 VO clock/pipeline 未就绪时丢写。修复：histb_vo_crtc_atomic_enable
    末尾用 `crtc->primary->state->fb` 重跑 histb_vo_g0_set。这是"无画面"的根因修复。
  - GP0_CTRL 读到 0x1032 与我们的写入值相同，无法区分"写入生效"还是"恰为复位默认"，
    无碍结论。

- #54（再补 `new_state->event = NULL`，修 hw_done WARN_ON(event)）：链式推板成功上盘
  （md5 6a36c86b），但 3.03s 在 **mmc percpu 路径**（pcpu_chunk_relocate ← bdev_alloc ←
  mmc_blk_probe）撞 `list_debug BUG`，随后 CPU3 NMI 洗板、console 无回显——与 DRM 改动
  无关（改动只有一行置 NULL），性质同 #44 时代的 femac/内存破坏线索，#53 同硬件可完整起
  到 shell，判随机或环境性。

- **屏上观察请求**：#53/#54 里 G0 已配置 + DHD0_CTRL=0xe0004010（cbar_sel/cbar_en 置位），
  若 DHD→TX 输出通路完好，屏上应显示**硬件彩条**（与 fb 内容无关）。

- 状态：板子死循环 NMI，需手动断电。U 盘上已是 #54，上电直接跑它；
  上电后第一件事：看屏（彩条?），再把 cbar_en 撤掉验证 fbcon。
## 2026-10-02 00:4x 会话收尾：进度与下次入口

### 已定案的根因（勿再排查）
- **G0 图层寄存器在 VO pipeline 未就绪时丢写**：commit_tail 顺序是
  commit_planes → modeset_enables（drm_atomic_helper.c:1989），首个 commit 的
  plane update 跑在 pipeline_init/像素时钟之前，G0 写入全部被丢（#50/#53 实测
  G0 块读回全 0）。修复：`histb_vo_crtc_atomic_enable()` 末尾用
  `crtc->primary->state->fb` 重跑 `histb_vo_g0_set()`；#53 实测
  `pre=40000068 post=c0000068 postupd=c0000068`（surface_en+IFMT 落上）。
- 静态侧已全排除：偏移（HDATE_VERSION=0xf000 锚点机械累加复核）、位域、
  stride 单位（SDK STRIDE_ALIGN=16，驱动 pitch/16 正确）。
- atomic_flush（spin_lock + drm_crtc_send_vblank_event）已加，修掉每周期
  `WARN_ON(new_crtc_state->event)`→33s init 被杀链。

### 当前开放问题（下次从这里继续）
1. **DHD0→HDMI TX 输出通路不出画面**：#53 4 分钟健康运行期间 G0 已配置、
   DHD0_CTRL=0xe0004010（cbar_sel/cbar_en bit29/30 已置），屏上仍无画面
   （用户确认）——连硬件彩条都出不来。嫌疑：cbar 位语义（仅位名证据，SDK
   驱动无人写过）、DHD0_DATA_SEL 路由、TX 侧 DATA_CTRL/GCP 配置。TX 侧
   此前实测同步计数正常（H_RES=1344/V_RES=806，SYS_STAT=0x87）。
   下一步：shell 里读 `/sys/kernel/debug/regmap/f8ce0000.hdmi-*` 全量 +
   DHD0_DATA_SEL(0xc02c) 读回，对照 SDK VDP_DISP/HDMI 初始化序。
2. **内存损坏线索**（旧案+新案）：#54 连续 2/2 在 3s 死于
   `pcpu_chunk_relocate ← bdev_alloc ← mmc_blk_probe` 的 list BUG 并杀 init
   楔死；#53 同硬件起到 shell。#54 唯一差异是 atomic_flush 里
   `new_state->event = NULL` 一行（已撤）。注意与 #44 时代 femac/KASAN 线索
   并案观察；#55（event=NULL 撤销版）若又复现，说明不是这一行引起的。

### 镜像状态
- **U 盘上现在是 #54**（md5 6a36c86b…，3s 死，不能用）。开机 2/2 楔死，
  **下次上电必须走 U-Boot 重刷**：`Press Ctrl+C to stop autoboot` 窗口发
  `\x03`（实板验证过），然后 `setenv serverip 192.168.1.2; tftpboot
  0x42000000 hdmi55.img; usb start; fatwrite usb 0:1 0x42000000
  recovery.img 0x981000`（文件已放在宿主机 atftpd 目录
  /srv/blob/openwrt/bin/targets/qualcommax/ipq50xx/hdmi55.img，#55=
  2c034606…，= #53+G0重写+flush，无 event=NULL）。刷完 `reset`。
  看守脚本模板：/tmp/ec6108v9c-ttl/uboot-reflash.sh（可复用）。
- 或 #54 若某次侥幸起到 shell（不指望），直接网络推 /tmp 下流程不变。

### 环境/工具备忘
- screen 重建必须 `-L -Logfile`（漏了会以为板子死了）；panic 后 init 常被
  procd 拉起，板子多数情况还活着，hardcopy 探活优先于看 console.log。
- 板上 busybox 无 devmem applet；/dev/mem 写 RAM 被拒（Bad address）；
  dd/md5sum/regmap debugfs 可用。
- fbdev smem 只有 512KiB 与 3MiB gem 不符——未解之谜，彩条只写进了顶部
  128 行。读回 `2d763823…` 校验通过。
- 宿主机：enp6s0=192.168.1.2/24（tftp:69 是 cr8818 的 atftpd，serve
  /srv/blob/openwrt/bin/targets/qualcommax/ipq50xx）；enp5s0.2001=192.168.2.1
  （HTTP 8681 推板，serve mkbootimg/）。U-Boot 默认 ipaddr 192.168.1.10。
- 裸 tftp 需 root 起 atftpd，本会话 sudo 无免密，走现成 192.168.1.2 服务。
- 本会话新建的杂项目录 /tmp/ec6108v9c-ttl/tftp/（拼错的空目录）留在 /tmp，
  无害。

### 未提交改动（linux-mv100 工作树，4 处）
- drivers/gpu/drm/histb/histb-vo-regs.h：+DHD_CTRL_CBAR_SEL/EN 位定义
- drivers/gpu/drm/histb/histb-vo.c：atomic_flush、enable 尾部 G0 重写、
  cbar 调试位置（撤画面验证时删）、g0 rdbk/vo-regs 一次性调试打印
  （整理提交时删）、regs_dumped/g0_rdbk_done 字段
- drivers/gpu/drm/bridge/histb-hdmi-14.c、sii9134.c：上一会话的移植修复
  （regmap 32 位 bus、偏移窗口校验、DPD bit2:0 等，见上文 2026-10-01 节）

### 本轮收尾（2026-10-02 00:50）
- 已清理：本会话看守脚本、上一会话遗留的 watch_interrupt.sh、串口 screen；
  板锁 board-ec6108v9c 与 ttl-usb-1a86_USB_Serial 已释放（记录移入回收站）。
- 保留：宿主机 192.168.2.1:8681 HTTP 服务（推板用）；tftp 目录里的
  hdmi55.img（下次 U-Boot 重刷要用，刷完可删）。
- /tmp 下本轮误建的空目录（ec6108v9c-ttl/tftp 等拼错名）未删，无害。

## 最终构建（h159）上的显示链复核（2026-10-06）

- 这次 sink 是 MS2130 采集卡（不是 BOE105HDR 面板）。`video=1920x1080i` 与不带参数
  两种 boot 实际都编程成**逐行**：读回 `DHD0_CTRL=0x8000c010`（bit4 IOP=1=progressive、
  bit14 hdmi_mode=1、bit15、bit31），`DHD0_VSYNC=0x00328437`（VACT=1080、Vbb=132、
  Vfb=50），`DHD0_HSYNC1=0x00bf077f`（HACT=1919→1920、Hbb=191），
  `DHD0_PWR=0x0004002b`（VPW=43），`DHD0_DATA_SEL=0x0000000e`。
  原因：fbdev 的 `video=` 取向匹配不认 "i" 后缀，取的是 EDID 模式表里第一条
  1920x1080，而 MS2130 的首选就是 1080p。
- 想在这块采集卡上跑隔行需要换 sink 或注入 EDID，但
  `cat edid.bin > /sys/class/drm/card0-HDMI-A-1/edid` 返回 EINVAL
  （`echo disconnect > status` 之后仍写不进），**未继续深挖**——按"非核心阻塞不死磕"处理。
  隔行分支本身在 2026-10-05 的 BOE105HDR 面板上已实测过（上文任务 2–4 的图案计数与
  人眼判定），本轮只动过 `sound/soc/`、`drivers/clk/`、`docs/`，
  `drivers/gpu/drm/histb/` 的隔行代码与那次验证时逐字相同。
- 色彩复核（同一 boot，`scr=7` 四象限 + 采集卡取帧 + `boardtest/capcheck.py`）：
  TL 白 (255,255,255)、TR 黑 (0,0,0)、BL 红 (228,0,0)、BR 蓝 (0,0,232)。
  黑不偏紫、白不偏青绿、红蓝无色偏——修色结论在最终构建上没有回归；
  228/232 是 limited-range 色度裁切的预期值（见上文色偏一节）。
- 音视频同时成立：该 boot 下 48k/44.1k/32k 与 256 帧小周期全部 0 xrun、
  wall 与时等长；真实 wav（440 Hz → 静 → 880 Hz）在采集卡上按秒复现三段结构，
  峰值 19660 = 文件幅度 0.6×FS。判据与数据见 `docs/aiao-porting-guide.md` §9.11。

## 采集卡可验模式全测 + 4K 寄存器证据链（2026-10-07，板上 h165i = 内核 `#81 SMP Wed Oct 7 02:12:11 CST 2026`）

### 仪器判据（先定"能测什么"）

- `v4l2-ctl -d /dev/video0 --list-formats-ext`（MJPG）只枚举 15 档输出尺寸，
  640x480 … 2560x1600，**没有 3840x2160**：4K 输入下白/黑/红三帧的 luma 全为
  `0..0`、同一尺寸的字节数三帧完全相同（`sw-3840x2160-*.jpg` 的 16522/33145/2602
  各自一致）→ 卡对 4K 不锁，画面层面没有任何判据，4K 只能给寄存器证据。
- 卡在"卡的自身尺寸"档会滞后一帧，缩放档（1920x1080、640x480）跟踪稳定，
  所以 `capany.sh`/`modesweep.sh` 把 1920x1080 排在第一个。
- regmap debugfs 读 bank0 返回全零，而驱动同地址读到 0x37 → debugfs 的缓存读
  不可信，本轮所有寄存器读数都走 `histb_vodump` 的 peek（`poke_sel=2` = HDMI TX
  绝对偏移，`1` = CRG，`0` = VDP）。

### 逐模式色彩（`scr` 纯色 + 四象限，`capcheck.py` 对留存 jpg 的复算值）

| 模式 | 白 | 红 | 黑 |
|---|---|---|---|
| 1920x1080 | 254.9–255.0 | 227.7–227.8 | 0.1–0.6 |
| `video=1920x1080i` 请求（实际逐行，见下） | 254.9–255.0 | 227.6–227.7 | 0.3 |
| 1280x720 | 254.3–255.0 | 227.7–227.9 | 0.3–0.9 |
| 1024x768 | 254.3–255.0 | 227.6–228.0 | 0.4 |
| 1600x1200 | 254.6–254.8 | 227.8–228.0 | 0.1–0.6 |
| 640x480 | 255.0 | (253, 7, 0) | 0.0 |

HD 与大 VESA 档彼此一致，228 是 limited-range 色度裁切的预期值；黑不偏紫、
白不偏青绿，`TX_VID_DITHER(0x158)=0x0e` 与 colorimetry 声明在这些档上无回归。
640x480 是唯一异类：红既不裁到 228、又漏 G=7，改 AVI 声明的组合不改变该读数
→ 判为卡在该档缩放/解映射路径上的行为，不是端口声明问题；残留已记录，
按"非核心阻塞不死磕"不再消耗板时。

**`video=1920x1080i` 这一行不是隔行证据**：该 boot 读完寄存器是
`DHD0_CTRL=0x8000c010`（bit4 IOP=1=逐行）、`DHD0_VSYNC=0x00328437`、
`DHD0_HSYNC1=0x00bf077f`、`DHD0_HSYNC2=0x0000020f`、`DHD0_PWR=0x0004002b`，
与 2026-10-06 那次的"请求 i 仍编成逐行"完全一致。本轮确认这块采集卡的 EDID
确实列出 `1920x1080i`（`card0-HDMI-A-1/modes` 里 3 条），所以原因不是"sink 不声明隔行"，
而是 fbdev 的 `video=` 取向匹配选不出 i 模式；隔行的实测证据仍只有 2026-10-05 在
BOE105HDR 面板上的那次（图案计数 + 人眼），要在新 sink 上复走得换能显式选 1080i 模式的
DRM 客户端。

### DATA_SEL

各模式回读 `DHD0_DATA_SEL=0xe`，端口只做 RMW 改 bit3（与 SDK
`VDP_DISP_SetDataSel()` 一致：bit1 是 vga_data_sel，bit2 在 mv100 保留）。

### YCbCr 接口腿被实测否证（全模式保持 RGB 的依据）

把 bit3 清成 0 走 SDK 的 YCbCr 腿，实测红 (77, 250, 30)、蓝 (177, 0, 0)、
黑 (0, 134, 0)，灰精确 → Cb/Cr 两路互换。最可能的原因是 HDATE 的 lane mux
（`video1_sel=2`、`video2_sel=3`、`video3_sel=1`、`src_ctrl=2`，
`vdp_driver.c:10589-10598`），**未验证**（验证方法是改这几个 select 再取一次
四象限）。所以端口在每条模式上都送 RGB、由 transmitter 做 YCbCr 编码。

### 4K：板子上跑出来的是 3840x2160@25（内核 CEA 表第 94 项）

- 前提 `cma=64M`：默认 `CmaTotal=16 MiB`，而 `drm_fbdev_dma` 要 31.6 MiB，
  结果是 hotplug `ret=-12`、无 fb0 —— 配置问题，不是代码缺陷。
- 时钟：驱动日志 `pixel clock 297000000 Hz for mode clock 297000 kHz`；
  CRG 回读 `0x28=0x11000000`、`0x2c=0x02002063`（正是新增的那一行），
  `0xd8=0x0c01007f`（bits19:18=0，即 `CLOCK_DIV_2`）。
- 发射机：`TX_STAT(0x24)=0x87`，bit0（TMDS clock stable）=1，无 "not stable"
  告警；输入几何计数器 `MEAS_HRES(0xe8/0xec)=0x14a0=5280`、
  `MEAS_VRES(0xf0/0xf4)=0x8ca=2250` → 5280×2250×25 = 297.0 MPixel/s，
  与内核 CEA 表 4K@25 的 total 逐字相符，即 VO 送出的就是该模式的完整帧时序。
- 信息帧：AVI 槽头 `82 02 0d`、checksum `0x59`，payload `0x30`、`0x88`
  （colorimetry=709、YCC=4:2:2）、`0x00`、VIC=`0x5e`=94；VSDB（MPEG 槽 0x680）
  `81 01 05 48 03 0c 00 20` + 4K_VIC=`0x02`，即 SDK `SI_Set_VSI_4K2K()` 编号里的
  "2 = 3840x2160@25"，与 `sii9134_hdmi_4k_vic(94)=2` 一致；`INF_CTRL1` 的
  enable/repeat 位为 0xc3。
- 音频：48 kHz 流运行中 `N_SVAL(0x40c/0x410/0x414)=00 14 00` = 5120 =
  SDK `N_Val_4K[2]`；无流时同槽是 6144 = `N_Val[2]`（端口只在 atomic_enable 与
  hw_params 里写它），`ACR_CTRL(0x404)=0x06`（N/CTS 使能 + 内部 MCLK）。
  选表用的就是 `sii->video_4k`，所以这条读数同时证明 4K 判定真的生效。
- 仍未验证（代码注释里已标）：4K 的画面，以及"4K 帧里的声音真到 sink"
  （卡不锁 4K，该 boot 下采集卡录不到音频流）；`296703000` 那一对（1000/1001 档，
  本 EDID 不提供）；`241500000`/`268500000` 两对（vendor 表自身标 "not support"，
  且本面板 EDID 无这些模式，`video=` 造的 CVT 模式命不中精确时钟，板上取不到
  该时钟）。

### 音频验收收口：440 与 880 之争是发生器自己的换频

- 板上 `pp` 默认前半段发 `-f` 频率、后半段发 `-F` 频率
  （`pcmplay.c:308`、`:342-345`），所以"第 0 秒 440、随后 880"是它自己的换频点，
  不是链路异常。
- 对照实验：`-f 440 -F 440` 全程 ch0/ch1 都读 440 Hz、峰值 12000（-8.7 dBFS）；
  `-f 880 -F 880` 全程 880 Hz。
- 声道映射：播 L=440/R=880 的 wav（`boardtest/audio/tone-l440-r880.wav`），
  卡上 ch0=440、ch1=880、峰值 16384（-6.0 dBFS）→ AIAO→HDMI→sink 左右声道无错位。

### 注释改动的等价自证

`drivers/clk/hisilicon/crg-hi3798mv100.c` 只把 4K 那段从"transcribed, not
measured"改成实测结论（净增 3 行注释）：section 尺寸与内容除 `.strtab` 外全等
（`.strtab` 同尺寸、只有 1 个符号名变），`arm-linux-gnueabihf-objdump -d` 的
514 行全等，`nm` 唯一差异是 `__LINE__` 派生的 initcall 名
`__initcall__kmod_crg_hi3798mv100__204_698_...` → `..._701_...`（+3 行）。
故板上这份 h165i 与提交内容等价（除该符号名）。比对脚本 `/tmp/objeq.py`，
改前后对象留在 `/tmp/cmt-proof/`（按禁删规则未清理）。

### 本轮工具与现场

- 新增（`boardtest/`，一律 HTTP 取到板上跑）：`pack-ram.sh`（自动取下一个
  `hNNNi` + 16MB 红线）、`onboard-check.sh`、`display-up.sh`、`modesweep.sh`、
  `capany.sh`、`ifpeek.sh`/`ifpeek2.sh`、`meas.sh`/`meas2.sh`、`crgread.sh`、
  `nval-check.sh`、`pp`（裸 ALSA ioctl 播放器）；宿主机侧 `/tmp/bothch.py`
  （逐声道主频）、`/tmp/objeq.py`。板上输出用 `nc 192.168.2.13 9999` 推回
  （`/tmp/recv.py`），原始读数留档在 `/tmp/nval-4k.txt`、`/tmp/ifpeek2.txt`、
  `/tmp/meas2-4k.txt`、`/tmp/crg-4k.txt`。
- 8681 的 HTTP 服务在 netns `ec6108tftp` 里是单进程，本轮出现过一次"连接挂住 →
  板上 wget 报 `Operation not permitted`"，重启该服务后恢复；服务与 screen 会话
  `ec6108-ttl` 收尾时保留，板锁 `board-ec6108v9c.lock` 收尾释放。
- 板上 femac 的 PHY 仍需 `down/up` 数次才应答（`onboard-check.sh` 里那段循环就是
  为此），本轮冷启后是第 2 次 cycle 才通。

## v7.3-rc6 之上的按子系统重折（2026-10-07 07:0x）

### 触发：rebase 到 rc6 后开机即 oops

`#83`（分支 rebase 到 `a90ee4305c4a Linux 7.3-rc6` 之上）启动 2.7 s 即挂：

```
Unable to handle kernel NULL pointer dereference at virtual address 00000178
PC is at phy_link_topo_add_phy+0x60/0x244
of_phy_get_and_connect from hisi_femac_drv_probe+0x1f4/0x558
Kernel panic - not syncing: Attempted to kill init! exitcode=0x0000000b
```

不是显示驱动踩内存。`hisi_femac_drv_probe()` 在 `hisi_femac.c:833` 先挂 PHY，
`ndev->netdev_ops` 到 `:853` 才赋值；`phy_attach_direct()` →
`phy_link_topo_add_phy()`（drivers/net/phy/phy_link_topology.c:45）里内联的
`netdev_need_ops_lock()`（include/net/netdev_lock.h:33）在 `CONFIG_NET_SHAPER=y`
时无条件读 `dev->netdev_ops->net_shaper_ops`。偏移逐条对上：`ldr r3,[r0,#8]` 取
`net_device` 偏移 8 = `netdev_ops`（前面是 8 字节 priv_flags 位域组，
netdevice.h:2169-2174），`ldr r3,[r3,#0x178]` 是 `net_device_ops` 第 95 个指针
`net_shaper_ops`（94×4=376，netdevice.h:1716）。旧基线不炸只因为那份 `.config`
里 `NET_SHAPER` 没被 select 起来（该符号无 prompt）。

处置边界：**不动上游逻辑**。我先改的 `include/net/netdev_lock.h` 已 `git checkout`
撤回，只把板子驱动的 ops 赋值挪到 attach 之前 —— `phy_attach_direct()` 自己的注释
（phy_device.c:1820-1828）说在 `register_netdev()` 前挂 PHY 是允许的，那驱动就得先
把 ops 发布出来。

### 重折形状

`scripts/fold-series4.py` 在 detached worktree `/tmp/refold-wt` 里按子系统重放整条链，
把夹在显示系列里的其它驱动改动折回各自提交：

- `dt-bindings: clock: add Hi3798MV100 CRG` ← 再含 HD 像素时钟 id + AIAO 时钟 id
- `clk: hisilicon: add CRG driver` ← 再含 HD 像素时钟 + AIAO 时钟（733 行）
- `ARM: dts: hisilicon: add Hi3798MV100 device trees` ← 再含 VO 节点、HDMI 使能、
  AIAO 声卡（1151 行），并把原来错挂在 dts 提交里的"发射机色彩空间处理"段落删掉
  （那段属于 bridge 提交，bridge 消息里本来就有）
- net 修复紧跟在 Festa PHY 之后
- display/sound 的 schema 排到 dts 之前（provider 先于消费者）

28 提交 → **21 提交**，`tip tree identical: 7b9242a052b661cf9f5294ca9e7c6d0ecd17a892`
（折前折后 tip 树哈希一致 ⇒ 内容逐字节未变）。分支
`mv100-hdmi-format/rc6-by-subsystem`。逐提交的文件归属已不跨子系统（只有各自的
MAINTAINERS 条目）；规模分布：2227（bridge）/1408（dma）/1397（VO）/1151（dts）/
733（clk）/481（AIAO）/其余 ≤ 426。

### 门（跑在重折后的链上）

- 逐提交编译 **21/21 OK**：每步先恢复 pristine `.config` 再 `olddefconfig`（否则该步
  树里没有本驱动符号时会静默把 `=y` 删掉、编出空日志假通过），并数 `CC` 行确认目标
  .o 真编；`dtbs` 每步都编。
- checkpatch --strict 逐补丁：21 个补丁全 **0 error**，除 9 条
  "MAINTAINERS need updating?"（新增绑定/dts/驱动的固有提问，对应 MAINTAINERS 条目
  在配套驱动补丁里）外没有别的发现；0002 提交消息里那条 78 字符的 oops 引用行已改写。

### 上机（按指示只测启动 + 出图）

`h167i.img` = 内核 `#85 SMP Wed Oct  7 06:47:58 CST 2026`，`video=1920x1080 cma=64M`。

- 启动：无 Oops/panic；`0.712 [drm] Initialized histb-vo`、`0.897 fb0: histb-vodrmfb`、
  `1.976970 phy_id=0x20669823, phy_mode=mii`、`6.07 eth0: Link is Up - 100Mbps/Full`。
- 出图（采集卡 1920x1080 档，`scr` 纯色 + `capcheck.py`）：白 254.5/254.6/254.3、
  黑 0.2、红 227.8/0/0、蓝 0.1/0/231.8 —— 与 rc5 上那份记录同档同值，无色偏回归。
- 踩过的坑：`scr=7` 四象限那一张里"白"象限读到 44.7 灰，是 fbcon 把敲进串口的命令
  回显画在左上有效区（cmdline 带 `console=tty0`），不是色彩问题；纯色三张全对。
- 配置修复：rebase 后那份 `.config` 丢了 `HISI_FESTA_PHY`、`REGULATOR_HISTB`、
  `DRM_HISTB_VODUMP`（#83/#84 上 PHY 因此落到 Generic PHY、`histb_vodump` 不可用），
  已 `scripts/config --enable` 补回并 olddefconfig 复核在案。

### 留着不做的

bridge 补丁 2227 行仍偏大。再切需要把 `histb-hdmi-14.o` 与 `sii9134.o` 拆成两个
Kconfig 符号，那是改内容而不是折叠历史，按"提交内容=板上验过的那份"的边界没动，
要拆请单独决定。


