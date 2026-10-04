## 2026-10-02 凌晨（续）：双会话 relay 推进 #58→#72，TX 侧全对、锁定 GP0 数据通路差异

> 本节由 hdmi-code 会话（改码/编译）与 hdmi-board 会话（上板）经 `/srv/blob/histb/relay.md` 协同产出。
> 镜像产物均在 `mkbootimg/recovery-hdmiNN.img`（不覆盖 recovery.img 原位）+ tftp 目录 `hdmiNN.img`；
> 每版 `KBUILD_BUILD_VERSION=NN` 使 uname 显式带 #NN，判读以 uname+md5 双对。

### 提交外工作树新增（在 4 处既有改动之上）
- `sii9134_bridge_atomic_enable()` 重排：等输入时钟稳定(≤50ms) → SWRST → 唤醒(SYS_CTRL1) →
  视频路径/DE 配置 → phy → 等 PLL → AVI → unblank → **`sii9134_clear_avmute()`**
  （GCP CLR_AVMUTE=0x7A:0xDF[4] + INF_CTRL2 CP_ENABLE|CP_REPEAT 重发；SDK
  si_hdmitx.c:732 生产尾步，移植原先完全缺失）。
- TX 从 `SI_UpdateTX_DE+SI_SetDE(ON)` 切到本平台生产分支 **`SI_UpdateTX_656`**
  （si_eeprom.c:151 出厂 `EE_TX_DE_ENABLED=0`；DE_CNTRL 保持复位默认 0；
  0x3a/0x3c/0x40/0x42/0x44/0x46/0x47 按 DRM mode 通用式编程，1024x768@60 与
  si_vmtables.c:122 表值逐位一致）。IADJUST 用 SDK 保留式 RMW。
- histb-vo.c：irq 风暴修复（**probe 先清+全掩蔽再 request**；handler ACK 全部已锁存位，
  非 vblank 位限流告警）——修掉每 boot 必现的 `irq 32: nobody cared`（用户点名）。
- histb-vo.c：atomic_flush 补 `new_state->event = NULL`（hw_done WARN 标准修法；
  #54 当时回滚它是错误归因——#56 无此行同样 3s 复现 list_del）。
- 观测件（一次性，整理提交时删）：`vo-regs-post` 36 寄存器（含 VO_MUX/VO_INTSTA/
  DHD0_STATE/DEBUG/DEBUG_STATE/HDATE 族）、`vo-pix-src` tap 8 路扫描、`vo-vcnt`
  同 boot 双采样、`hdate: en/ver` 即写即读、sii9134 `enable: enter/after-config/
  enable-done` 三行、EDID `edid:` 与 `mddc block N timeout` 诊断。
- 白填充实验（判后删）：crtc atomic_enable 末尾 memset gem 全白。

### 已定案（勿再排查）
1. **DE/写持久性**：#58 enable 三行证明写入全落位；#55 起 SWRST"抹配置"假设被 dump 驳回
   （DE_CNTRL=0x71 存活）；#61 起 de=00 复确认写路干净。
2. **dump 行号=字节偏移**（reg N 在行 4N）：#56 的"整片复位"与"0x33=0"两条均为行号误读，
   已收回（方法学教训：先乘 4 再判读）。
3. **irq 风暴根因**：probe 里 request_irq 先于清/掩蔽 + handler 只 ACK bit0，其余锁存位
   （如 g0rr）返回 IRQ_NONE 累积 → 2.1s 关线。#66/#67 修复后不再出现。
4. **内存破坏并案现状**：#59 dma-rebase 基线 2 轮干净 vs HDMI 系多点复现 ⇒ 嫌疑在
   HDMI 系列；但 `VO_INTSTA 无 m0_be`（#70）排除了"G0 取数被 AXI 拒"这条腿；
   femac probe NULL deref 随机复现，独立观察。
5. **DDC/EDID 独立挂账**：`edid: read failed -110` 确定性复现（MDDC 读超时）；modes
   五条是 DRM 无 EDID 兜底集，不是真 EDID（#37 时代"EDID 实读"记录作废）。非黑屏主因，
   修复另开工作项。
6. **cbar 判据无效**：SDK 唯一 cbar 先例是 /proc 钩子（等价于我们的位），从未在本机
   验证能出图 ⇒ "cbar 不出⇒线死"推理作废，不得再引用。
7. **pix_src tap 全 0**：8 路扫描读不出任何级（#68），tap 观测在本 IP 形态无效，弃用。
8. **vcnt 在走**（#71 同 boot 双采样）：DHD0 时序引擎活着。
9. **HDATE_EN 写 1 读回 0**（#71，dump 顺序在写后，排序排除）；SDK mv100 源码有
   HDATE_Set* 全家族但全树+所有 .o/.a 零调用者——HDATE 大概率不在 HDMI 通路上，
   #72 的 `hdate: ver=` 读数（0xf000 可读性）做最终裁决。

### Ground truth（fastboot console `md.l`，已证可用）
boot 阶段现场：`VO_MUX=0x1`（复位默认，无人写）、`HDATE 块全 0`、
`DHD0_CTRL=0x80008010`（iop+bit15+intf_en，**hdmi_mode=0**）、`G0 与内核同`、
**`GP0_CTRL=0x1203`**、`CBM_BKG1=0x04080200`（≈黑背景，忽略）。
- **0x1203 = SDK `VDP_GP_ORDER_ZME_CSC`**（mux1=3,mux2=0,aout=2,bout=1，
  vdp_driver.c:5465-5471 case 表）；我们的 0x1032 = `ORDER_NULL` 旁路档。
  ⇒ 第一个货真价实的 GP0 数据通路配置差异（producer 用 ZME_CSC，我们旁路）。
- 待复采 27 字扩容清单（见 relay 21:31 条）后整套搬 GP0/ZME 差异 → #73。
- 注意：fastboot 无画图命令/无显示分支（只读写 logo 分区元数据），其 GP0 值同属
  "非显示路径遗留"，但它是唯一另一 producer 的写集；且它**恰好是 SDK case 表里的
  合法档**，比我们的旁路档更有资格是"通路正确态"。

### 版本索引（md5 前 8）
#58 5013b3a0（enable 重排前置版，作废）→ #59 基线 920d1878 → #60 5551b6c9 →
#61 DE→656 切换 → #62 DATA_SEL=0（判据半作废：YUV 侧本无内容源）→
#63 IADJUST+EDID 诊断 → #64 白填充+VO_MUX → #65 修正版 → #66 irq 修复 →
#67 tap 布防 → #68 src 扫描 → #69 bit15 → #70 观测扩展 → #71 vcnt/hd_en →
#72 hdate ver + event=NULL（刷机窗口暂停，待放行）。

---

## 2026-10-02 00:45 停止待命时的快照（#73→#78）

### 上文"Ground truth"里被后续读数推翻的部分
- `HDATE 块全 0` / `HDATE 出局`：**不成立**。后续 fastboot 复采到
  `0xf000=0`、`0xf004=1`（= hd_en 开），且 **U-Boot 里写 0xf004=0 屏立刻变黑** ⇒
  开机的白是 HDATE 图形发生器直出 HDMI，不是任何 framebuffer 内容。
  因此"boot 白 = G0 内容经 DHD0"这条推断作废，#62/#65 的"全白"也不再是 GP0 链的证据。
- `G0 与内核同`：不成立。boot 态 `6000=0`、`6010=0`（G0 关），内核活体才是
  `6000=c0000068`、`6010=3f100000`。

### 本轮坐实的静态事实（都带 file:line）
1. **mv100 的寄存器结构里没有 G1，也没有 GP1。** 机械累加整张
   `hi_reg_vdp.h` 全局结构（锚点 `HDATE_VERSION == 0xf000` 校验通过）：
   0x6800-0x6a00、0x9800-0x9a00、0xc400-0xc600、0x140-0x160 全部无命名成员，
   整个 GP1 只有一个 `GP1_PARARD`（0x494）。板上"gp1 全写读回 0"与之一致。
2. **mixer 路由不在 GP0_CTRL 里。** `U_GP0_CTRL`（hi_reg_vdp.h:5174-5197）字段只有
   mux1_sel[1:0]、mux2_sel[5:4]、aout_sel[9:8]、bout_sel[13:12]、ffc_sel[26]、
   mute_en[30]、read_mode[31]；`VDP_GP_SetIpOrder()` 五个分支也只改这四处
   （vdp_driver.c:5427-5467）。surface 进哪个 mixer 由 **`CBM_ATTR(0xb440).sur_attrN`**
   决定，语义在 SDK 注释里逐位写明：attr0/1 "ga to mixer1"、attr2 "v3 to mixer2"、
   attr3 "gb to mixer2"、attr4 "v4"（vdp_driver.c:113-118）。boot 的 b440 = 0xc。
3. **HDMI 取 DHD0**：`VDP_DISP_SetHdmiClk(hd_id,div)` hd_id=0 → `CRG54.hdmi_clk_sel=1`，
   hd_id=1 → 该位=0（vdp_driver.c:2566-2588）；boot `CRG54=0x0401007f` ⇒ bit26=1、
   bit27=bit29=0。旁证：hifb 的 GP→display 映射
   `enDisp = (enGPId == GP_0) ? DISPLAY_1 : DISPLAY_0`（hifb_v1_0/drv_hifb_adp.c:549），
   而 `GetChID(DISPLAY_1)=0`、`GetCbmMixID(DISPLAY_1)=VDP_CBM_MIX0`（drv_disp_hal.c:240-263）。
   ⇒ "开机白出自 SD/DSD 路"这个说法静态不成立；SD(DSD) 在 HDMI 之后，boot 的图是 HDATE。
4. **boot 的 GP0_CTRL=0x1203 = `ORDER_ZME_CSC`**（mux1=3,mux2=0,aout=2,bout=1，
   vdp_driver.c:5461-5467），不是 ORDER_CSC(0x1230) 也不是 ORDER_NULL(0x1032)；
   两档的 bout_sel 都是 1，差别只在 mux1/mux2 互换。
5. **`mux: wrote daf reads 0` 还不是"写不落"的结论**：0x140 的写一直排在
   `VO_CTRL.vo_ck_gt_en`（pipeline_init 的最后一笔）之前，属于未控变量；
   与 HDATE `f004`、`VO_INTMSK` 同族的读数都来自这种窗口。晚窗实验尚未做。
6. **fb0 与被扫描缓冲是同一个对象**：`plane fb: id=40 1024x768 pitch=4096 XR24
   dma=0x3f100000` 与 `G0_ADDR(0x6010)=0x3f100000` 同址；
   `drm_fbdev_dma.c:213-220` 只在非 vmalloc 且未隐藏 smem_start 时才填 `fix.smem_start`。
   "两块 buffer"（0x2800000 vs 0x3f100000）的疑虑出局。

### 版本索引（续）
#73 9000=0x1230（CSC 档，屏黑）→ #73' 91c0 bypass=0（mv100 无此位，无效实验）→
#74 0x140=daf + fill-cmp（WC 映射失败，`ioremap_wc` 对 3MB CMA 返回 NULL）→
#76 9000=0x1203 + b428=0x21 + cbar 全关（屏黑；mux 读回 0）→
#77 GP1 全镜像 + b428=0x21 + SD mirror（md5 30c63810，未上板）→
**#78 单变量 `b440 |= sur_attr0`（gfx A 也送 mixer2），
md5 6dd55e99d10bb3774e730360d9f9adf6，9969664 B，tftp `hdmi78.img`，未上板**。

### 恢复时的下一步顺序
1. 上 #78：读 b440（应 0000000d）+ 看屏 —— 判 mixer2 是否 HDMI 在采的一路。
2. 若仍黑：晚窗单变量版（ck_gt_en 提到 pipeline_init 第一笔，mux 写移到门开之后）。
3. 若晚窗仍不落：把嫌疑收到 probe 之后我们重配 SiI9134 的那一段
   （boot 的图出自 TX 原始配置；对照实验 = 整轮不动 TX，只配 VO）。
4. 未动挂账：DDC/EDID `-110`、`vo irq status=0x6` 洪水、随机崩溃家族
   （WBC 0xac00/0xa800 观测始终没编进版）。

## 2026-10-04：随机内存破坏家族定案 = plane state 欠分配导致的 slab 越界写

### 结论

`drivers/gpu/drm/histb/histb-vo.c` 的 primary plane 同时用了

- `DRM_GEM_SHADOW_PLANE_HELPER_FUNCS`（`.begin_fb_access = drm_gem_begin_shadow_fb_access`，
  include/drm/drm_gem_atomic_helper.h:125）
- `.atomic_duplicate_state = drm_atomic_helper_plane_duplicate_state`（`histb_vo_plane_funcs`，修复前 :408，现 :411，只分配
  `sizeof(struct drm_plane_state)`）

`drm_gem_begin_shadow_fb_access()` 用 `to_drm_shadow_plane_state()`（同文件 :85，纯
`container_of`，无长度校验）把 state 上转成 `struct drm_shadow_plane_state`，随后
`drm_gem_fb_vmap(fb, shadow_plane_state->map, shadow_plane_state->data)` 往 `fmtcnv_state`、
`map[]`、`data[]` 写数据 —— 这些字段全部位于基结构之后，于是每次 fbcon 接管都在
kmalloc-192 槽位内做越界写，并溢出到右 redzone。修法是删掉那对 helper funcs
（本驱动按 `gem->dma_addr` 直接配硬件，不做 CPU 侧像素访问；`drm_fbdev_dma` 的
damage blit 用的是 `fb_helper->buffer->map`，见 drm_fbdev_dma.c:161，也不依赖 shadow state）。

### 决定性读数（同一 boot，#93，cmdline 带 `slub_debug=FZPU,kmalloc-192 loglevel=1`）

```
[1.780182] [Right Redzone overwritten] 0xc3ab3500 @offset=5376. First byte 0x0 instead of 0xcc
[1.780219] BUG kmalloc-192 (Not tainted): Object corrupt
[1.780235] Allocated in drm_atomic_helper_plane_duplicate_state+0x34/0x70 age=11 cpu=1 pid=1
             ← drm_client_modeset_commit_atomic ← __drm_fb_helper_restore_fbdev_mode_unlocked
             ← drm_fb_helper_set_par ← fbcon_init ← register_framebuffer ← histb_vo_probe
[1.780712] WARNING mm/slub.c:1257 check_bytes_and_report ← check_object ← free_to_partial_list
             ← kfree ← drm_atomic_commit_default_clear ← __drm_atomic_commit_free
             ← drm_client_modeset_commit_atomic ← drm_fb_helper_pan_display ← fbcon_switch
```

对象字节（左 redzone 全 `cc` ⇒ 对象在使用中；`cc` 段是 kmalloc redzone）：

| 槽内偏移 | 值 | 归属 |
|---|---|---|
| 0x00-0x93 | plane/crtc/fb 指针、1024x768、src 16.16 定点 | `struct drm_plane_state` 本体（orig_size≈0x9c=156B）|
| 0x9c-0xa3 | `00 00 00 00 00 00 00 00` | 越界写 1：`fmtcnv_state`（应为 `cc`）|
| 0xbc-0xbf | `00 70 a2 f4` = 0xf4a27000 | 越界写 2：`map[0].vaddr`，fb 的内核映射地址 |
| 0xc0 | `00` | 越界写 3：`map[0].is_iomem`，已在 kmalloc-192 槽位之外 |

同点位每 boot 复现；上一轮（HDMIFREE2）同一偏移处是 0xf4a23000，随映射变化 ⇒ 该字是
运行期映射地址，不是常量。

### 对既往结论的更正

- "随机破坏家族 = DRM plane state **双重释放**" —— 不准确。第一现场是**越界写**：slub 在
  free 时校验右 redzone 失败，走 `FIX ... Object at 0x... not freed` 把对象**泄漏**而不归还
  （mm/slub.c:4326），于是 freelist/后续 commit 的引用状态错乱，才表现出 double-free、
  `list_del` 抱怨、RCU stall、femac probe NULL ×N、initramfs unpack 也在 free inuse 对象等
  下游症状。这些都发生在同一个 `kmalloc-192` 槽位被踩之后。
- 10-03 记录的"vo irq 洪水 + FZPUT 风暴"里，风暴有一半是调试参数自己造成的：本树
  `parse_slub_debug_flags()`（mm/slub.c:1830）的字母表中 **`T` = `SLAB_TRACE`**（每次 alloc/free 都
  `print_section` + `dump_stack`，见 mm/slub.c:1679 `trace()` 里的 `dump_stack()`），不是对象追踪；打印
  `Allocated in` / `Freed in` 归属栈需要 **`u` = `SLAB_STORE_USER`**。正确组合是
  `slub_debug=FZPU,kmalloc-192`。

### 复现/取证流程（本会话实测可用，免 U 盘、免反复断电）

1. 宿主机 tftp（**非 root 也行不了 69 端口**：`ip_unprivileged_port_start=1024` 且 AppArmor
   仍拒绝，必须 `sudo -n /sbin/atftpd --daemon --bind-address=192.168.2.1 --port=69
   --user=nobody --verbose=5 --logfile=/tmp/atftpd-root.log <目录>`；`--verbose 5` 的空格写法
   会把 5 当成位置参数、服务目录被顶掉 ⇒ `File not found`，必须写 `--verbose=5`；atftpd 不跟
   服务目录之外的符号链接 ⇒ 镜像要是真实副本）。
2. 板上 fastboot#：`setenv ipaddr/serverip/bootargs` → `tftp 0x42000000 hNN.img` →
   `bootimg 0x42000000`。fastboot **确实优先用 env 的 `bootargs`**，再把 eMMC bootargs 分区里的
   `blkdevparts=` 拼在后面（已用 `HDMIFREE*` 标记在 `/proc/cmdline` 里核实），改调试参数不必写 eMMC。
3. 取全量 dmesg：板上 `dmesg > /tmp/d.txt` 后 `nc 192.168.2.1 9999 < /tmp/d.txt`
   （busybox `nc` 有，无 `-w`；`wget` 只能下不能上）。板上 PHY 要 `ifconfig eth0 down/up`
   一到两次才通（2.17s 那次 boot 直到 ~1.9s 之后 link 才稳）。
4. `panic_on_warn=1 panic=N` 在这块板上**不可用作自动重试**：panic 后
   `Rebooting in N seconds..` → `Reboot failed -- System halted`（无可用 restart handler，
   #93 预武装的 sp805 也没兜住），只能断电。

### 本轮同时修的移植缺陷（显示主线）

`histb_vo_g0_set()` 里 4 处 `vo_write(vo, G0_CTRL|G0_CBMPARA, ...)` 漏加 `gfx_off`，而读侧都带
`+ gfx_off` ⇒ `gfx_off=0x800`（G1，出图态真正在用的层）时所有写落到 G0，读回来自 G1。
现统一为带偏移，含 `g0 rdbk` 打印里的 `G0_UPD` 读。

## 2026-10-04（续）：显示主线的前提校正与已排除项

内存破坏闭合后（#94 起 7 次连续启动 `slub_debug=FZPU` 全缓存零报告），把历史"黑屏"结论
在稳定内核上重测了一遍。**四条前提被推翻**：

1. **"tftp boot 起内核时 VO 已被 fastboot 关掉"（10-03 vo-quench 结论）作废。**
   实测：只关显示驱动、开 vodump 的内核（#101）起来后 **fastboot 的 logo 一直在屏上**
   （摄像头 ROI R117/G152/B185）。也就是说 tftp 引导路径下，内核接手时 VDP 仍在扫 logo，
   G1 仍使能、缓冲仍是 0x3f3b9000。这条把"活体出图态"变成了内核里可读写的前提，
   是今天所有对照实验的基础。
2. **"DHD0_DEBUG 像素探针 (512,384) 读数=0 ⇒ 合成器输出就是黑"作废 —— 仪器本身不出数。**
   在 logo 明确可见的那次启动里扫 `pix_src` 0..7（`c0f8 = src<<28 | 384<<16 | 512`），
   `c0fc` 八个源全 0。mv100 上这个抽头不能用来定位像素去向。
3. **"绿屏 = 图层内容"误判。** 摄像头看到的纯绿（R41/G171/B120）是**显示器无信号画面**：
   它出现在我把 `VOCTRL(vo_ck_gt_en)` 写成 0 之后。我们驱动跑起来时屏是**真黑**
   （R15/G13/B19），说明 TX 一直在出有效同步信号、面板在显示黑内容。
   判屏必须看 ROI 的 **R/G/B 三通道**，灰度均值会把绿(126)和 logo 白(145)混掉
   （`boardtest/screen-check.py` 已改成三通道）。
4. **"CRG54 被我们改坏是黑屏原因"排除。** #100 起 `crg54_reset` 默认关（不再清
   `vo_sd_hdmi_clk_sel(bit27)`、不再脉冲 `vou_srst_req`），实测 CRG54 保持出图态的
   `0x0c01007f`，屏仍黑。

### 今天做实的正向结论

- **G0 这条路是通的。** 在活体出图态里把 G1 关掉（`6800=40000000` + `6804=1` 锁存）屏立刻变黑；
  随后把**同一块 logo 缓冲**接到 G0（`6010=3f3b9000 601c=140 6020=002cf4ff 6030=11ff
  6084/608c=002cf4ff 6000=c8000368 6004=1`），**logo 重新出现**（R122/G159/B194）。
  所以不需要"必须用 G1"，也不需要 MIXG0/CBM 之外的额外接线。
- 出图态 `CBM_ATTR(0xb440)=0xc` 的语义查清了（vdp_driver.c:113-119）：attr0/1=0 已经是
  "ga→mixer1"，attr2/3=1 是 v3/gb→mixer2 ⇒ **CBM_ATTR 不是 G0 的开关**；在驱动开着的启动里
  把 `b400(CBM_BKG1)` 写成出图态的 `0x04080200` 并读回确认，屏无变化（背景也没出来）。
- 出图态 `GP0_CTRL(0x9000)=0x00001203`，**bit31 read_mode 实际是 0**（旧文档记的 0x80001203
  不对）；在活体状态把 `9000` 写成 `0x80001203` + `9004=1`，画面不变 ⇒ read_mode 不是杀手。
- 我们驱动的最终态里 `G0_CTRL` 一度是 `0x4c000360`（**surface_en=0**），而编程完那一刻是
  `cc000360`：控制台空白（DPMS）关掉图层是**推断**（未直接抓到那次关闭的调用）；
  重绑后立刻读回是 `cc000360`、`dpms=On`，屏仍黑 ⇒ 稳态下"层被关"不是黑屏根因。
- 手工把 GP0 几何按我们驱动的值写一遍并补 `9004=1` 锁存，再往 `/dev/fb0` 灌随机噪声（读回
  确认已落进被扫缓冲），屏仍黑 ⇒ **G0 使能 + 缓冲有内容 + 时序在跑 + TX 锁定，画面仍不到面板**。
  断点收敛在"G0/GP0 组合输出→CBM→DHD0"这一段，且与 ifmt(0x60/0x68)、PALPHA_EN、
  clip 块、CBM_BKG1、GP0 read_mode、CRG54 都无关（逐个试过，见下"排除清单"）。

### 排除清单（都在稳定内核上实测，非推断）

| 变量 | 测法 | 结果 |
|---|---|---|
| `dhd_clip_en=1`（0xc080..0xc0c0 抄出图态） | 旋钮+重绑 | 黑 |
| `gp_order` 0x1203 / 0x1230 / 0x1030 | 旋钮+重绑 | 黑 |
| TX `sys_ctrl1_v=0x36 vid_mode_v=0x20 vid_dither_v=0x0e` | 旋钮+重绑 | 黑；TX `stat=0x87` 与出图态一致 |
| `gfx_off=0x800`（改用 G1，写偏移修复后首次真正生效） | 旋钮+重绑 | 黑 |
| CRG54 保持 boot 值（不跑 DISP_ResetCRG） | #100 默认 | 黑 |
| ifmt 0x68→0x60、PALPHA_EN 开→关 | 活体出图态 poke | 画面变暗但**仍有内容** ⇒ 非黑屏根因 |
| clip 块整块清零 | 活体出图态 poke | 画面不变 ⇒ 不承重 |
| GP0 read_mode、GP0 几何+锁存 | 活体出图态/驱动态 poke | 画面不变 |
| `pixprobe` 抽头定位 | 8 个 pix_src 扫描 | 仪器不出数，不可用 |

### 下一步的抓手（按性价比）

1. 现在有了"活体出图态 + poke"这台实验台，正确方向是**反向**做：从出图态出发，
   逐项改成我们驱动的整套值（含 1024x768 时序 + 我们 TX 重配后的状态），
   每次一项并拍照，直到画面消失 —— 第一次让它消失的那一项就是根因。
   今天只做了单项（GP0/clip/ifmt/CBM_BKG1），**没有做过"整套时序换成 1024x768"这一项**，
   而它是最大的差异面（`c004/c008/c00c/c014/c020` 全组 + TX PLL 重配）。
2. 顺带值得单独试的：出图态 `DHD0_SYNC_INV=0x2000`（`date_hs_inv`）vs 我们按 DRM 极性写的
   `0x0600`（`hdmi_hs_inv|hdmi_vs_inv`）—— 在活体状态写 0x0600 + `c000|=1` 锁存看画面。
3. 工具已就位，不必重编即可跑：`boardtest/vdp_off.py`（偏移→SDK 寄存器名，锚点
   HDATE_VERSION=0xf000 已过）、`boardtest/vodiff.py`（两份 vodump 逐寄存器差）、
   vodump 新增 `poke`（写 VDP 寄存器并读回）、`scr`+`fb_addr/fb_stride/fb_width/fb_height`
   （往任意物理缓冲写图案，注意改了 override 要先 `echo 1 > dump` 重新建映射）。

### 追加：HD 像素时钟分频（CRG54）实测——承重但救不了我们

`vo_hd_clk_div`（CRG54 bits[19:18]）实测**是承重的**：在活体出图态（驱动关、logo 在屏）
写 `0c0d007f`（div=3）画面立刻消失，写回 `0c01007f` 画面立刻回来（#102 那次，
ROI R116/G152/B183）。这也再次说明"活体出图态"这台实验台是可信的。

但在**我们驱动的那套**（#103，1024x768，G0 使能、缓冲灌了随机噪声并读回确认）扫
div=0/1/2/3：ROI mean 分别 16.2 / 18.6 / 25.4 / 15.7，全黑，没有哪一个分频能把画面带出来。

⇒ 分频不是根因（至少单独调它不够）。**但这条线索本身成立且未解**：驱动里
`clk_set_rate(vo->clk_pix, ...)` 依赖 DTS 提供 `pix` 时钟，而 `drivers/clk/hisilicon/`
至今没有 CRG54 的 VO 时钟门/分频树（histb-vo.c probe 里的 `TODO(crg)`），
所以现在跑的是"fastboot 留下的 297MHz 时钟 + 我们写的 1024x768 时序"这套不自洽组合。
下一步该做的是**把 CRG54 的 HD 时钟树建模进 clk 驱动并按模式配速率**，
而不是继续用 poke 猜分频值——猜已经证伪了一次。

（板上 CRG54 已恢复 `0x0c01007f`，读回确认。）

---

## 2026-10-04 第二轮：fbcon 上屏定案 + 3 倍放大根因（#105→#107）

### 1. 图层被关掉：`plane_state->visible` 从未计算（已修，#107 板上验证）

`histb_vo_plane_atomic_check()` 不调 `drm_atomic_helper_check_plane_state()`，`visible` 恒为
false，于是每次 plane commit 都走 `histb_vo_g0_set(enable=false)` 清 `G0_CTRL.bit31`。
`crtc_atomic_enable()` 里硬编码的 `true` 只掩盖了第一次 commit；启动期**第二次** commit 没有
crtc enable 跟着，层就永久关了。用一次性 `dump_stack()` 抓到调用者：

```
histb_vo_g0_set  (enable=false)
drm_atomic_helper_commit_planes
__drm_atomic_helper_set_config / drm_client_modeset_commit_atomic
drm_fb_helper_set_par
fbcon_init → visual_init → do_take_over_console → register_framebuffer
```

修复：plane check 调 helper 算 visible（`DRM_PLANE_NO_SCALING`, can_position=false，因为驱动
恒按 fb 尺寸摆在 0,0），crtc enable 改读 `drm_atomic_get_new_plane_state()` 的 `visible`。
#107 实测：`plane update visible=1` 两次、`peek [6000] = cc000360` 稳定不回落、屏上直接出控制台
文字，不再需要手工 poke。

### 2. 画面放大 3 倍：GP0 缩放引擎的步进寄存器没人写（根因定位，未修）

判据（人眼 + 可计数图案，相机不可信，见 §4）：

| 图案 | 1:1 应为 | 实测 | 倍率 |
|---|---|---|---|
| 一行 60 个 `#`（8px/字） | 1/4 屏宽 | 3/4 屏宽 | 3x |
| 64px 棋盘格 | 横向 30 个白块 | 10 个 | 3x |
| 四象限（白/黑/红/蓝） | 四块 | 均匀一色 | 3x 抹平 |

根因在 boot 出图态的对照里：`GP0_ZME_HSP(0x9140).hratio = 0x055555 = 2^20/3`、
`GP0_ZME_VSR(0x9150).vratio = 0x555 = 2^12/3` —— 正是 fastboot 把 1280x720 放大到 3840x2160
的 1/3 步进。驱动改了 `GP0_IRESO/ORES O/DLPOS` 却从不写这两个 ratio，硬件仍按 boot 的系数放大。
定点精度由同族 IP 佐证：`msp/drv/vpss/vpss_v4_0/hal/hi3798cv200/vpss_hal_3798cv200.h:22`
`#define VPSS_ZME_HPREC (1<<20)`。GP0 的 CSC/系数块 0x9120-0x913c 同样还是 boot 3x 那套，
所以残留的色偏（黑显紫、白显青绿、三原色单独却正常）大概率同源，修好 ratio 后复测。

### 3. 本轮排除项（别再走）

- `CBM_BKG1(0xb400)` 涂白（0x3ff80200）屏上无变化 → 图层不透明覆盖背景，背景寄存器不是"紫"的原因；
  字段布局 Cr[9:0]/Cb[19:10]/Y[29:20] 与 SDK `U_CBM_BKG1` 一致，驱动值 0x04080200 就是视频黑。
- `DHD0_SYNC_INV(0xc020)` boot=0x2000 是 `date_hs_inv`（HDATE 通道），与 HDMI 输出无关。
- TX `0x48/0x4a/0x4f` 现写回 boot 值，红场读数 192/45/54 vs 192/45/57，无差别。
- DHD0 时序逐字段核对：hact 1920 / hbb 120 / hfb 160 = 2200，vact 1080 / vbb 30 / vfb 15 = 1125，
  与 EDID 原生 1920x1080@148.5MHz 完全一致。
- `G0_UPD` 读回 0x816 与 boot 出图层相同 → 取像引擎无错误标志。
- CRG 差异 0x60/0x68/0x9c/0xb8/0xbc 分别是 NF/UART/USB2 的门控与复位，与显示无关。
- EDID 通路本身已闭环：2 blocks、校验和正确、解出 `BOE105HDR`。

### 4. 判屏设施教训

宿主机 Vimicro `0ac8:307b`（gspca_zc3xx）本轮多次掉线（`URB error -71`），且 `VIDIOC_QBUF`
报错时**仍返回上一帧**：全白填充与全黑填充的照片 ROI 数值逐字节相同。它的 AWB 还会逐帧漂移，
绝对色相不可用。本轮"四象限是均匀色"的最初误判就来自卡帧。可信判据：人眼 + 可计数图案
（多少字符占多少屏宽、能数出几个格子）+ EDID 自洽性。

### 5. 现场与下一步

板子跑 #107（tftp 引导，未写 eMMC），锁已释放。代码改动全部未提交，集中在
`drivers/gpu/drm/histb/histb-vo.c`、`histb-vo-regs.h`、`drivers/gpu/drm/bridge/sii9134.c`。
下一步按顺序：(1) 驱动写 `GP0_ZME_HSP/VSR` 的 ratio 并确认 1:1 时 `hsc_en/vsc_en` 使能位怎么给
（查 `msp/drv/vo/vdp_v2_0/hal/s5v100` 的 ZME 路径）；(2) 复测色偏与"顶部错行一条"；
(3) 把 CRG54 VO 时钟树建模进 `drivers/clk/hisilicon/`（任务 #4）。

## 2026-10-05 提交系列重整（f47bed4..mv100-hdmi，9 个提交）

把 `stash` 那一坨（`aec64f73d56f`）与之后的清理改动一起重放成"新驱动投稿"形状；
基线 `f47bed498582`（histb_dma runtime PM 修复），旧 tip 全量备份在分支
`backup/pre-restructure-20261005`（重整前树）与 `backup/pre-restructure-20261005b`
（补上 YAML/MAINTAINERS 之后）。

上游可投的 5 个：

1. `dt-bindings: display: add HiSTB VO and HDMI transmitter schemas`
   —— 新增 `display/hisilicon/hisilicon,histb-vo.yaml`、
   `display/bridge/hisilicon,histb-hdmi-1-4.yaml`。两份都过
   `make dt_binding_check DT_SCHEMA_FILES=…`；`dtbs_check` 对
   `hi3798mv100-ec6108v9c.dtb` 也过。注意本树的 `Documentation/devicetree/bindings/`
   根下没有 `graph.yaml`，`/schemas/graph.yaml` 由 pip 的 dtschema 包提供，
   所以 `$ref` 照样能解析。
2. `drm/bridge: add SiI9134 HDMI transmitter for HiSilicon STB SoCs`
   —— `sii9134.{c,h}` + `histb-hdmi-14.c` + bridge Kconfig/Makefile + MAINTAINERS 条目。
   两个 .o 同属一个模块，所以 `sii9134.c` 里的 `MODULE_*` 与
   `EXPORT_SYMBOL_GPL` 删掉了（同模块内不需要导出）。
3. `drm/histb: add VO display controller driver`。
4. `ARM: dts: hisilicon: add the VO node and link it to HDMI`。
5. `ARM: dts: hisilicon: enable the HDMI output on EC6108V9C`。

明确不投的 4 个（`[LOCAL]` 前缀）：aiao 节点 + sound card（驱动未移植，
`hisilicon,histb-aiao-i2s` 没有 schema，混进显示提交会直接挂 dtbs_check）、
sp805 预挂 watchdog（改了所有 SP805 使用者的行为）、`histb-vodump.c`
（快照/poke 调试台）、`docs/`。

验证：每个提交单独 checkout 后强制重建它自己引入的产物，9/9 通过
（`/tmp/series2.txt`：`sii9134.o`/`histb-hdmi-14.o`/`histb-vo.o`/`histb-vodump.o`/
`sp805_wdt.o` 与 `dtbs` 都在自己的提交上真编过）；`checkpatch --strict`
除"新增文件是否要更 MAINTAINERS"这条信息性警告外全 0。
板上跑 `h115.img`（tip 构建）：`VERDICT h115: bug=0 corrupt=0 stalls=0 panic=0
mark=4 vo=2 femac=1 fb=1`，采集卡 1920x1080 抓到 fbcon 首行
"Please press Enter to activate this console."，44 字符墨迹宽 348 px
（7.9 px/字符，8x16 字体 1:1）。

## 2026-10-05 第二轮：时钟树进 CRG 驱动 + 720p 打通（系列 10 个提交）

分支 `mv100-hdmi` 改为落在干净重写分支 `mv100-upstream`（12 提交）之上，
`mv100-upstream..HEAD` 共 **10 个提交**：6 个可投 + 4 个 `[LOCAL]`。
rebase 前的旧系列（落在 whaway 转储之上）备份在 `backup/mv100-hdmi-pre-upstream`，
本轮改动前的树备份在 `backup/mv100-hdmi-720p`。

可投 6 个的顺序按依赖排：
`clk: hisilicon: add the Hi3798MV100 HD display pixel clock` →
`dt-bindings: display: …` → `drm/bridge: add SiI9134 …`（含 MAINTAINERS 条目）→
`drm/histb: add VO display controller driver` →
`ARM: dts: … add the VO node and link it to HDMI` →
`ARM: dts: … enable the HDMI output on EC6108V9C`。
`[LOCAL]` 4 个：aiao/sound card、sp805 预挂狗、histb-vodump、docs。

### 720p 的根因与修法（推翻上一版本节的"TX 需要 pixel-repeat"）

真因在 CRG：驱动原先把 CRG54[19:18] 当"÷2^n"的位移写，而它是**编码值**
（SDK `CLOCK_DIV_1=3 / CLOCK_DIV_2=0 / CLOCK_DIV_4=1`，
`msp/drv/vo/vdp_v2_0/hal/3798m/hal_specific_config.c:1073`），
且 HD PLL 的 VCO 固定 594MHz、后分频在 `PERI_CRG_PLL10`(0x28)[26:24] 里一位热码。
通道时钟 = PLL 输出 ÷ 2，所以厂商表 1080p60 写 0x12000000、720p 写 0x14000000。
按 `docs/720p调查/00-汇总.md` 的配方，这段时钟树写进
`drivers/clk/hisilicon/crg-hi3798mv100.c`（新增 `hpll_vco` 固定 594MHz +
`hpll_div` + `vo_hd_div` + 暴露门控 `clk_vo_hd`，id `HI3798MV100_VO_HD_CLK`），
VO 驱动只留 `clk_set_rate(clk_pix, mode->clock*1000)` 与
`mode_valid()` 里的 `clk_round_rate()` 判据；驱动内不再 ioremap CRG、
不再直写 PLL10/11/CRG54。PLL10 bit28 与 CRG54 bit26/29 未建模（无人清除、
boot 值即所需值），理由写在驱动注释里。

分频器表按 {/2, /4, /1} 顺序给 CCF 选择，实测结果：
720p60 选到 n=4 + /2 = 74.25MHz，与厂商表逐位一致；
1080p60 选到 n=1 + /4 = 148.5MHz（厂商用 n=2 + /2，同频率不同组合，
这个组合正是 #115/#116 出过正确 1:1 画面的那套）。

### 板上测量（h117，`video=` 强制模式）

| 运行 | cmdline | fb | CRG_PLL10 | CRG54 | TX STAT | TX H_RES |
|---|---|---|---|---|---|---|
| h117p | `video=1280x720` | 1280x720 stride 5120 | 0x14000000 | 0x0c01007f | 0x87 | 0x0672=1650 |
| h117f | `video=1920x1080` | 1920x1080 stride 7680 | 0x11000000 | 0x0c05007f | 0x87 | 0x0a50=2640 |

`STAT=0x87` 的 bit0 就是 TMDS/IDCK 稳定位 —— 74.25MHz 这次是真锁住了。
采集卡侧：720p 运行下 44 字符墨迹宽 522px（8px 字体被采集卡按 1.5 倍放大到
1920 输出），1080p 运行下同一行 348px（1:1），两者都能从几何上区分源模式。
两次运行 `VERDICT … bug=0 corrupt=0 stalls=0 panic=0 mark=4 vo=2 femac=1 fb=1`。

TX 侧一行未改（`sii9134.c`/`histb-hdmi-14.c` 与上一轮相同），
bridge 的 `SII9134_MAX_TMDSCLOCK=165MHz` 仍在，所以 297MHz 的 4K 模式仍被挡在
`mode_valid` 之外；37.125MHz 以下（480i/576i）不在这张表的分频组合里，也不做。

### 配置与工具

`linux-mv100/.config` 无 `=m` 项、`CONFIG_MODULES` 关闭（本轮复核：`=m` 计数 0，
olddefconfig 后无符号丢失），所以"改 =m 为 =n"这一步实际无事可做；
为让 `video=` 生效检查了 DRM 的 cmdline 解析在本树是无条件编进 drm 的
（`drm_client_modeset.c` 用 `connector->cmdline_mode`，无独立开关）。

## 2026-10-05 第三轮：时钟树进 CRG 驱动、VESA 逐格式表、隔行与屏幕控制台

### 时钟建模位置

驱动内直写 CRG 的临时做法已撤掉，HD 像素时钟改由
`drivers/clk/hisilicon/crg-hi3798mv100.c` 提供：新增内部时钟 `hd_pix`
（自定义 `clk_ops`：`recalc/determine/set`）+ 暴露门控 `clk_vo_hd`
（CRG54 bit4 `vo_hd_cken`，`CLK_IS_CRITICAL`，id `HI3798MV100_VO_HD_CLK`），
VO 节点在 DTS 里 `clocks = <&crg HI3798MV100_VO_HD_CLK>` / `clock-names = "pix"`，
驱动只剩 `clk_set_rate()` 与 `mode_valid()` 里的 `clk_round_rate()` 判据。
驱动不再 ioremap PERI_CRG。

### 为什么是"逐格式寄存器对表"而不是"固定 VCO + 分频"

先按 594MHz 固定 VCO + 一位热码后分频建模，实测 720p60 得到
`PLL10=0x14000000`、`CRG54=0x0c01007f`、TX `STAT=0x87`、`H_RES=1650`（=720p60 行总像素），
采集卡画面按 1.5 倍放大（源确为 1280x720）——CEA 家族成立。
但厂商表里 VESA 条目**同时改 `PERI_CRG_PLL11`**（1024x768@60 =
`0x15000000/0x201954a`、1280x1024@60 = `0x15000000/0x20198c9`、
1440x900@60RB = `0x14000000/0x2006163`、1600x1200@60 = `0x12000000/0x2001036`），
即 VCO 本身可编程且 `PLL11` 位段无文档 ⇒ 固定 VCO 模型是建模不足，
改成照抄 `s_stDispFormatParam[]` 的 (rate, PLL10, PLL11) 三元组表（15 条，
rate 用该格式自身时序算出的像素时钟，如 1024x768：htot 1344 × vtot 806 × 60 = 65.12MHz）。
`PLL10` bit28/bit29 与 `CRG54` bit26/29 仍不建模（无人清除、boot 值即所需），理由写在驱动注释。

### 修掉一个真 bug：`mode_valid` 容差溢出

原式 `abs(rate - mode->clock * 1000) * 1000 > mode->clock` 在 32 位 `long` 上溢出，
时钟差 8% 的模式被误判合法：1280x1024 曾以 `PLL10=0x12000000/PLL11=0x2018493`
（146.2MHz）出图，按 `DHD0` 实测 htot 1688 / vtot 1068 即 81.2Hz。
改成 Hz 域 0.5% 判据后：1280x1024@60 走 `0x15000000/0x20198c9`（107.855MHz），
`H_RES=1688`、`STAT=0x87`；1280x1024@75（135MHz）正确地被拒。

### 隔行：已实现，未验证（2026-10-06 已实测通过，见文末"第四轮：隔行实测"）

VO 侧按厂商 1080i 行值推导并逐位对上：`vact/2=540`、`vbb/2=20`、`vfb/2=2`，
下场 `bvbb=vbb-vbb/2=21`（1125 行帧的奇数行落在下场后肩），
`hmid = htotal/2 - hbb`（1080i60 得 908、1080i50 得 1128，与表内值一致）；
`DHD0_CTRL.IOP=0`，TX 侧 `FIELD2_HSYNC_OFFSET = htotal/2`、`VTOT/2`、`vfp/2`。
两处 `MODE_NO_INTERLACE` 已删。**但采集卡的 EDID 模式列表里没有隔行模式**
（`card0-HDMI-A-1/modes` 24 条无 `i` 后缀），`video=1920x1080Mi` 也只解析成逐行，
实测寄存器仍是 `DHD0_CTRL=0x80008010`（IOP=1）、`FIELD2=0` ⇒ 无验证手段，
已按 A/B/C 三选一在 `ask.md` 提问（默认 A：隔行代码先标 `[LOCAL]` 待验证）。

### 屏幕显示 dmesg 与 tty（只改内核配置）

`CONFIG_CMDLINE="console=tty0"` + `CONFIG_CMDLINE_EXTEND=y`：fastboot 的
bootargs 只有 `console=ttyAMA0,115200`，EXTEND 把 `console=tty0` 前置进去，
两个 console 都收 printk。fbcon 在 0.90s 接管时 `redraw_screen()` 把 VT 缓冲里的
整段日志重画出来，实测画面从 `0.66s` 的 vodump 行一直显示到 `9.79s urngd`，
含 `Press the [f] key…` 与 `Please press Enter to activate this console.`；
`printf 'HOST-WROTE-TO-TTY1-42\n' > /dev/tty1` 出现在画面末行 ⇒ VT 可写、tty 在屏上。
initramfs 未改（OpenWrt 的 askfirst 本来就起在 VT 上）。

### 配置与系列状态

`linux-mv100/.config` 复核：`=m` 计数 0、`CONFIG_MODULES` 未开（"m 改 n"无事可做），
本轮新增的只有上面两条 console 相关配置；`.config` 不入 git，改动记在这里。
分支 `mv100-hdmi` = `mv100-upstream`（12 提交）+ 显示系列；本轮把
`clk: hisilicon: add the Hi3798MV100 HD display pixel clock` 的提交说明与实现
换成逐格式表，并把容差修复并进 VO 驱动提交、隔行并进 bridge/VO 提交。
备份分支：`backup/mv100-hdmi-pre-upstream`（rebase 前）、`backup/mv100-hdmi-720p`（本轮前）。

## 2026-10-06 第四轮：1080i60 隔行实测通过（BOE105HDR + MS2130 采集卡）

结论：VO 与 TX 的隔行时序编程在硬件上逐项对上厂商 `si_vmtables.c` 第 5 行（VIC 5），
采集卡量到 1920x1080 满幅 1:1、四象限内容齐全 —— 逐场取像成立，"未验证"这条挂账闭合。

### 寄存器判据（h129i，md5 `4b5ccc45…`，uname `#129`，bootargs 带
`drm.edid_firmware=HDMI-A-1:boe-il.bin video=1920x1080i`）

第二列"逐行参照"取自两次对照运行：`DHD0_*` 是 h126int + 面板真实 EDID 的 1080p60 读数
（驱动代码同，仅无 EDID 读改造）；`TX HTOT/VTOT` 是同一份 h129i 去掉 override 后的读数，
那次采集卡自报首选模式，fbcon 落在 **1280x720p60**（1650x750），不是 1080p。

| 寄存器 | 隔行运行 | 逐行参照 | 厂商表 / 推导值 |
|---|---|---|---|
| `DHD0_CTRL(0xc000)` | `80008000`（IOP=0，场顺序） | `80008010`（IOP=1） | `si_vmtables.c:58` VIC 5 |
| `DHD0_VSYNC(0xc004)` | `0011321b` = 540/20/2 | `00e1d437` = 1080/30/15 | 表内上场 540,20,2 |
| `DHD0_VPLUS(0xc010)` | `0011421b` = 540/21/2 | `00e00437` | 表内下场 540,21,2 |
| `DHD0_HSYNC1(0xc008)` | `00bf077f` = hact1920/hbb192 | `0077077f` | 44+148 |
| `DHD0_HSYNC2(0xc00c)` | `038b0057` → hmid=908, hfp=88 | `0000009f` | 表内 hmid 908 |
| `DHD0_PWR(0xc014)` | `0009002b` = vpw10/hpw44 | `000e0013` | 表内 44 / 5(场) |
| TX `HTOT(0xe8/0xec)` | `0x0898` = 2200 | `0x0672` = 1650（720p） | 表内 `{2200,562}` |
| TX `VTOT(0xf0/0xf4)` | `0x0233` = 563（写的是 562） | `0x02ee` = 750（720p） | 见下"VTOT 读回" |
| TX `FIELD2(0x108/0x10c)` | `0x044c` = 1100 | 0 | 表内 Field2Offset 1100 |
| TX `VBIT(0x118)` / `VLENGTH(0x11c)` | 2 / 10 | 15 / 15 | vfp/2=2、vsync 5…10 |
| `PERI_CRG_PLL10(0x28)` | `0x14000000` | `0x12000000` | 74.25MHz 档 |
| TX `STAT(0x24)` | `0x87`（TMDS 锁） | `0x87` | bit0 稳定位 |

VTOT 读回的归属（已测，不再是疑问）：在逐行对照运行（720p）里把 `0xf0` 用 `poke` 写成 `0x5a`，
读回 `0xee`（= 0x02ee = 750，当前模式的 vtotal），随后连读三次仍为 `0xee/0x02`。也就是说这对
寄存器**不保持写入值、随当前时序回填**——SDK 同样写它（`si_de.c` 的 `SI_UpdateTX_656()` 用
`WriteWordHDMITXP0(0x3a/0x3c, …)`），是"写入后被硬件按帧重载"的形态。据此，隔行运行里读到的
563 是硬件侧的**当下场行数**：上场 562、下场 563，合计 1125，正是我们期望的场结构。这条反过来
成为"VO 确实在逐场输出"的独立佐证（面板 OSD 报 1920x540 也与"每场 540 有效行"一致）。

### 采集卡侧几何与取像判据（`boardtest/cap.sh`，直采 HDMI 输出）

- 棋盘格（`histb_vodump.scr=6`，64px）：水平 29 次翻转 = 30 格（1920/64）✓，垂直 16 次翻转
  = 16 行方格 ✓ → 满幅 1:1，无 3x/2x 放大残留。
- 四象限（`scr=7`：上白/黑、下红/蓝）：采集到的 1920x1080 帧里四块**位置齐全**，
  分界在 y≈540 → 两场各自取帧的上半/下半行并织成整帧；若是"每场都从帧首取像"（bob/重复），
  下半屏应只出现上半内容。这条推翻了提交说明里"取像可能只出一场"的担忧。
- 逐帧差（1fps 取三帧）：帧间差异均值 ~0 → 两场内容一致、无场序错位。
- 面板 OSD 报 **1920x540**：本树 `struct hdmi_avi_infoframe`（`include/linux/hdmi.h:184-205`）
  **没有 interlaced 成员**，`hdmi_avi_infoframe_pack_only()`（`drivers/video/hdmi.c:136-163`）
  里 CTA 的 `I+` 位无处可写；报文实测 `VIC=5` 已上线（TPI packet RAM
  `0x400+0x100+4*7 = 0x51c` 读回 `0x05`，头 `0x82 0x02 0x0d 0x32`）。也就是说 sink 收到正确的
  VIC 却收不到 `I+` 标志。采集卡仍能按 VIC 织成整帧，面板则把每场当 540 行显示。
  要修的是"把 `I+` 送出去"，与隔行时序本身无关。
- 采集到的红块 = `(237,0,255)`、蓝块 = `(2,41,0)`（应为纯红/纯蓝）：**与隔行无关**——同一镜像
  去掉 override 的逐行运行（720p，IOP=1）采到同一组数值，白/黑两块两侧都正确。属旧账
  "黑显紫/白显青绿"的色偏，与本次隔行改动无关，另行处理。

### 手段与一次必要改动

- 造隔行 sink：`boardtest/gen-edid-1080i.py patch --base <真实EDID> --out <产物>` 保留捕获 blob
  的块 0 其余字节，只把 descriptor 0 改成**按场编码**的 VIC 5（540/2/5/15、2200x562、74.25MHz、
  `misc` 置 interlace 位）。内核 `drm_mode_do_interlace_quirk()`（`drivers/gpu/drm/drm_edid.c:3483`）
  把它翻倍成 1920x1080i vtotal 1125，与 CEA 表逐字段一致，`clk_round_rate()` 命中 74.25 档，
  `video=1920x1080i` 精确匹配到该模式（`drm_connector_pick_cmdline_mode()`）。
  存档：`boardtest/edid-boe105hdr-raw.bin`（md5 `57572385…`）、`edid-boe105hdr-il.bin`
  （md5 `4d989ff5…`）。
- **必须走内核 core 才吃得到 override**：`sii9134_bridge_edid_read()` 原先自己 `kzalloc` +
  手算校验和读 256 字节，绕开 `drm_edid_read*()`，因此 `drm.edid_firmware` 与 debugfs
  `edid_override` 全被跳过（第一次实测 h128i：sysfs 模式表 23 条与逐行寄存器纹丝不动）。
  已改为 `drm_edid_read_custom(connector, sii9134_get_edid_block, sii)`（per-block 读，DDC 段号
  沿用 `drm_do_probe_ddc_edid()` 的 `segment=block>>1` 约定），改完 override 立刻生效，
  sysfs 出现 `1920x1080i`。W=1 编译无警告。
- 配置：`.config` 保留 `CONFIG_DRM_LOAD_EDID_FIRMWARE=y`；测试期用的
  `EXTRA_FIRMWARE="boe-il.bin"` + `EXTRA_FIRMWARE_DIR=/tmp/fw-il` **已回空**（否则下次编译会把
  测试 EDID 静默烘进 zImage）。要再跑隔行：重新 set-str 两个符号，blob 用上面的存档直接拷贝。

### 更正一条旧记录

本文 2026-10-04 第二轮"EDID 通路已闭环：解出 `BOE105HDR`"成立，但当时另处记的"解出 `il`"
不成立：BOE105HDR 的 blob（EDID 1.3，2 blocks，校验和正确）两个 detailed timing 的 `misc`
都是 `0x1e`（interlace 位=0），CEA 扩展只列 VIC 70 与 VIC 16，模式列表 23 条无一含 `i`。
**这台面板不宣告任何隔行模式**，所以隔行只能靠 override 造，人眼/采集卡判屏时不能指望
`sink 自己给出 1080i`。

### 系列状态

`mv100-hdmi` 仍 12 提交；`1f81230 [LOCAL] drm/histb: program the interlaced channel timing`
的提交说明里"none of this has been verified on hardware"与"两处 mode_valid 仍拒绝隔行"
两句已过期（代码里那两处早已删，本轮上板验证通过）。工作树未提交改动：
`drivers/gpu/drm/bridge/sii9134.c`（`bridge.interlace_allowed = true` + 本轮 EDID 读改造）。
