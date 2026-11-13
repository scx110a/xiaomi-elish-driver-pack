# 小米平板 5 Pro (SM8250/elish) 系统笔记

更新于 2026-11-13 上午（相机/屏幕方向/平板模式/唤醒恢复/自启动收尾/传感器启动加速 L2+M1+M2 已验证；
音频栈 PulseAudio/WirePlumber 争用已修复）。待办按难度排序；历史开发/部署记录见 `HISTORY.md`。

> 本文是**整机 Linux 状态摘要**：硬件/驱动覆盖、待办与已知问题。
> 硬件/驱动的权威基线是 `~/xiaomi-elish-linux-v1-final/docs/HARDWARE-STATUS.md`；
> 安装/升级/排障以合并安装包 `~/xiaomi-elish-linux-v1-final/` 及其子包文档为准
> （`sensors/`、`fingerprint/`、`gnss/`、`kernel/`），本文件不重复细节。
> 总自检：`sudo ~/xiaomi-elish-linux-v1-final/install/check-all.sh`。

---

## 整机硬件 / 驱动状态（2026-11-13）

| 类别 | 硬件/功能 | 状态 |
|---|---|---|
| 已驱动 | 显示（CSOT + DPU/DSI）、背光 KTZ8866、触摸/手写笔 NT36523、霍尔/盖子/平板模式 | ✅ |
| 已驱动 | 电池/充电（双 bq27z561、pm8150b、bq2597x、USB-C TCPM/UCSI） | ✅ |
| 已驱动 | 音频（Xiaomi Mi Pad 5 Pro 声卡、8×CS35L41 外放、耳机、录音） | ✅（2026-11-13 修复 PA/WirePlumber 争用，见「已知问题」音频节） |
| 已驱动 | WiFi/BT（QCA6390 / ath11k + btqca） | ✅ |
| 已驱动 | pogo 键盘/鼠标（USB HID）、外接蓝牙鼠标 | ✅ |
| 已驱动 | 传感器：加速度、环境光、旋转矢量方向、指南针 heading（rotv 融合）；原始陀螺/磁力计/接近未暴露 | ⚠️ 部分能力 |
| 已驱动 | 指纹（Goodix GF3626）：fprintd 录入/验证、GDM 解锁、sudo（NOPASSWD 未停用则被绕过） | ✅ |
| 已驱动 | GNSS（Airoha AG3335，QUP5 UART）：gpsd + GeoClue2 地图位置/朝向 | ✅ |
| 已驱动 | 相机闪光灯、Venus 视频编解码、GPU/UFS/PCIe、PMIC/ADC/thermal/RTC | ✅ |
| 已驱动（全分辨率 GPU 管线） | **摄像头**：前 OV8856 3280×2464、后 OV13B10 4208×3120 全分辨率彩色拍照 + HEVC/H.264 硬编录像（~30fps）；`/dev/video2`/`/dev/video20` 虚拟相机；两个原生 PipeWire `media.role=Camera` 节点；portal/Snapshot 实测可用；后摄 AF/AE/AWB；后端登录自启 | ✅ 详见 `xiaomi-elish-linux-v1-final/camera/elish-camera/docs/`；Hi556 景深仍未通 |
| 已驱动（可配置） | **屏幕方向/平板模式**：相机方向按 Mutter transform 自适应；`tablet_mode=on/off/auto`（auto=跟随保护套折叠霍尔）；旋转时相机流按 `rebuild_mode=idle/live/off` 重建 | ✅ `elish-camera-ctl orientation`（配置工具） |
| 已驱动 | **休眠唤醒传感器恢复**：sleep hook + `elish-sensors-resume` 服务，唤醒后等加速计就绪再重启 iio，让 mutter 重新 claim；**M2 后实测唤醒 ~10s 恢复、自动旋转可用**（2026-11-13 实测；此前 ~35–40s） | ✅ 见 `camera/system/` |
| 已驱动 | **相机后端登录自启**：`elish-camera-autostart` + `/etc/xdg/autostart/elish-camera-autoserve.desktop` | ✅ |
| 已知缺陷 | 睡眠唤醒后 pogo 键盘失效 | ⚠️ 内核 USB PHY resume 问题，用户态无解 |
| 已优化（2026-11-13 凌晨，M2 已验证） | 传感器启动加速：服务解耦 multi-user + hexrpc 可写 sensor registry（M2）+ iio 时序；**实测开机 ~22s**（改造前 ~79s），桌面 ~38s 就绪 | ✅ 见文末「传感器启动加速」 |

驱动栈概要：内核使用合并包提供的联合 DTB 与相应模块/补丁；传感器走 SLPI/SSC，
指纹走 QSEE/TEE，GNSS 走 UART + 用户态守护进程，视频编解码走 Venus V4L2；
各子系统的安装、参数与排障见合并包内对应子包文档。

---

## 待办

### 1. box86/box64 红警 2 极慢（难）

- 目录 `~/Red Alert 2`，dxvk 2.6.1 + cnc-ddraw + box86/64
- 排查方向：dynarec 参数（`BOX64_DYNAREC_BIGBLOCK`/`STRONGMEM`）、wine+dxvk vs cnc-ddraw 的 GDI 路径、
  `DXVK_HUD=1` 确认 GPU 加速、调度器 `conservative`→`schedutil`、
  armhf 库缺失导致 box86 回退解释执行、cnc-ddraw renderer 选择

### 2. Linux 充电栈加固 + 基础快充（第 2 步，中）

目标：**不移植完整 Android SMB5 栈**，保留现有 mainline SMB5 + TCPM +
独立 bq2597x，只补关键状态机，做到 suspend/resume 不丢充电、支持基础快充。

- **范围（第 2 步）**：
  - 修 suspend/resume 后充电卡死：Type-C legacy 状态复位、resume 后重跑 APSD/AICL、
    恢复 current_max、bq2597x 初始 `usb_present` 重同步；
  - 接通 PD/QC 固定档（5V/9V）到 SMB5 的 ICL/FCC；
  - 目标 **15–27W** 基础快充 + 基本 JEITA/温控。
- **不做（第 3 步）**：电荷泵 2:1、双电芯、PPS、33W+ 原厂级快充——目前没有原装充电器，暂不考虑。
- **参考**：Android `qpnp-smb5.c` + `smb5-lib.c`（尤其 `smb5_configure_typec` 的 legacy reset、
  `smblib_rerun_apsd_if_required`、`smblib_set_usb_suspend`）。
- **验收**：反复 suspend/resume、插拔、不同充电器；不再出现 `power_role=source`/100mA 卡死；
  电池温升/限流正常；基础快充电流/电压符合充电器档位。
- **估时**：2–4 周（仅第 2 步；完整电荷泵/第 3 步另算 4–8 周，暂缓）。
- **前置条件（2026-11-12 检查）**：当前充电器 **不支持 PD**（`port0-partner/supports_usb_power_delivery=no`，
  仅 5V DCP，`tcpm-source voltage_max=5V`，`pm8150b current_max≈1.95A`）。要验证 9V/15–27W，
  必须换 **USB-C PD 9V/3A（或 QC 9V）充电器**；否则只能维持 5V 基础充电。
- **DTS 方向**：`sm8250-xiaomi-elish-common.dtsi` 当前 `sink-pdos` 为 5V + `PDO_VAR(5000,12000,5000)`；
  Armbian `0044` 将其改为 `PDO_VAR(5000,9000,3000)`。拿到 PD 充电器后优先加 **9V 固定 PDO**
  并验证 TCPM 请求 9V/3A；SMB5 驱动已有 DCP 3.3A 限流路径，再看 AICL 实际值。


### 3. Hi556 景深摄像头（难）

- 现状：`hi556 5MP` 已确认供电/上电顺序与 OV 相反（3 路供电 → MCLK → 最后释放 reset），
  仍卡在 PM8150A L1/L2 供电方案；
- 证据/接线：`camera/elish-camera/docs/android-camera-facts.md`、`HARDWARE-STATUS.md` §1.1；
- 下一步：解决 PM8150A L2（当前 `vreg_l2a_3p1` 被 USB HS PHY 3.3V 占用）的运行时互斥/迁移，
  再验证 CSIPHY/CCI 与 `hi556` 驱动；
- 验收：`/dev/media*` 出现 Hi556 链路、可抓 raw/彩色帧。

### 4. 原始陀螺 / 磁力计三轴 / 接近/SAR 用户态暴露（中）

- 现状：加速度/环境光/旋转矢量方向/指南针 heading 已可用；原始陀螺、磁力计三轴
  （μT）、接近/SAR 未通过 libssc/udev 暴露；
- 下一步：在 libssc/udev 层补 sensor type/scale/units（天级，非硬件驱动问题）；
- 验收：`gdbus`/iio-sensor-proxy 能读到 gyro、mag 三轴、proximity。

### 5. pogo 键盘 suspend/resume 内核修复（难）

- 现状：s2idle 唤醒后键盘可能失效/USB 反复重枚举；用户态复位已证明风险高
  （曾触发 USB/看门狗死锁），**不要再用 authorized/USBDEVFS_RESET/重绑控制器**；
- 根因：host `a800000.usb` + `88e4000.phy` 的 s2idle fast-resume 只做 PHY runtime resume，
  不重新初始化 PHY/控制器；
- 下一步：只做内核 `qcom_snps_hsphy` resume 路径/上游修复的设计与验证；
- 验收：suspend/resume 后键盘无重连、按键正常。

### 6. 唤醒后自动旋转恢复加速（中，已部分实施；等待合盖实测）

- 现状（2026-11-12 分析确认）：唤醒后 ~35–40s 恢复，主体是 SLPI `sensor_process` PD 在
  resume 时 `frpc_dsp` 崩溃 → SSR → **完整重做 SNS registry 初始化 ~35s**，与冷启动是同一段成本；
  用户态（worker 轮询/iio 重启/libssc 超时）合计只占 ~4–5s；
- 已实施：worker 轮询 0.5s、重启 iio 用 `TimeoutStopSec=3` 限时、删除无效的 slpi-sensors 兜底、
  休眠钩子 post 不再重复拉起 listener（防唤醒 attach 抖动）；hexrpc 补 sns_reg_version 映射；
- 重启验证（2026-11-12/13，见文末）：M1（regver-v1）只修好 version 读取，PD 仍 ~36s；
  **M2（可写 sensor registry，社区 0005/0006/0007 补丁）后 PD 初始化 35s→8.2s**、
  开机传感器 up=21.65s、iio NRestarts=0（`temp.json` 写失败 1570→0，517 次写全部成功）；
  唤醒路径同一段 PD 初始化被消除；**2026-11-13 00:08 合盖实测唤醒 10.2s**（此前 ~35–40s）；
- 现实目标：短期 ≤36s（已实施优化）；**10–20s 必须先加速 PD registry 初始化**，两条路：
  (a) 中等风险：给 hexagonfs 增加可写持久 persist（让 PD 的 `temp.json` 写回成功而不重试），
      预期 PD 初始化 35s→5–10s，开机/唤醒同时受益（详见工作区 `sensor-speedup/IMPLEMENTATION-20261112.md`）；
  (b) 高风险：内核修复 s2idle resume 的 frpc_dsp 崩溃（对照 pMOS 6.12/6.13）；
- 验收：唤醒 → 折叠背面，短期 ≤36s、长期 10–20s 内自动旋转可用。

### 7. GNSS 按需启动 / 懒连接（低收益，暂不做）

- 现状：`gps-nmea-share` 启动即连 gpsd，且 `airoha-gnssd` 开机自启 → GNSS LDO 常开，
  与 Android 的"有客户端才开"不同。
- 实测常开成本：**约 50–70 mW**（屏幕最亮系统 ~6.8 W 的 ~1%；24h 约 1.2–1.7 Wh ≈ 电池 3–5%/天，
  实际休眠/关机更低）。
- 方案（评估过）：bridge 启动只监听 `/run/gps-share.sock`，有客户端才连 gpsd
  （gpsd.socket 按需拉起 gpsd → `Requires` 拉起 airoha）；最后客户端断开后关连接；
  空闲 120s 后 `systemctl stop gpsd`（连带 airoha）；`airoha-gnssd` 改为 disable +
  `StopWhenUnneeded=yes`；`gps-nmea-share` 去掉 `Requires=gpsd.service`。
- 复杂度：~100 行 C + 4 个 unit 文件，约 2–4 小时；回滚简单（`systemctl enable airoha-gnssd`）。
- 关键不确定性：GeoClue/gnome-shell 是否在无 App 时仍保持 `/run/gps-share.sock` 连接；
  若常连则懒连接无收益，需再加 D-Bus 客户端计数（明显更复杂）。
- 结论：省电上限仅 50–70 mW，**收益低，暂不做**；除非以后特别在意续航。

## 硬件限制 / 无法实现（不作为 TODO）

以下由硬件/固件/上游限制决定，当前没有可执行项（详见 `HARDWARE-STATUS.md` §4）：

- **前摄 OV8856 固定焦点**：无 VCM/自动对焦机构，软件无法实现 AF；
- **前后摄不能同时出流**：两 sensor 共用 CAMSS/DTB 资源，需大规模内核/DTB 改造，暂不做；
- **PMIC RTC 无后备电池**：断电后可能回到 1970，依赖 NTP 校准，硬件不可修；
- **额外霍尔**：vendor DTBO 的 `hall_intr3/4`、`gamekey hall_left/right`、键盘 MCU gpio155
  在 mainline 无驱动/未启用，不计划支持（自动旋转/软键盘只需 `SW_TABLET_MODE`）；
- **本机不存在**：5G 版本（本机 WiFi 版；其余杂项硬件所有型号均无）；
- **dmesg 噪音**：DPU frame event overflow、GPU cooling 注册告警，不影响使用；
- **传感器 PD 偶发静默卡死（约 1/14 开机）**：SLPI/PD 固件侧问题，用户态只能重启恢复，
  不再作为 TODO（2026-11-12 深夜加速后实测开机 ~55s 可用）。

---

## 已知问题

### 音频无输出 / PulseAudio“设备或资源忙”（2026-11-13 已修复）

- **症状**：PA 启动后唯一 sink 长期 `SUSPENDED`；`paplay` 能连上流但永不开始播放
  （延迟一直 2s 不动），外放无声；`journalctl --user-unit pulseaudio` 反复
  `Error opening PCM device _ucm0001.hw:Pro,0: 设备或资源忙`（EBUSY）。
- **根因**：相机后端需要 PipeWire，而 PipeWire 的 **WirePlumber 0.4.17 ALSA monitor** 与
  PulseAudio 16.1 同时管理唯一 ALSA PCM `hw:Pro,0`（WirePlumber 也建了
  `alsa_output.platform-sound.HiFi__hw_Pro_0__sink` 并抢占 PCM）→ PA 打不开设备，
  sink 永久 suspended，无音频输出。**不是 PulseAudio 本身故障，也不是功放损坏。**
- **修复（用户级，无需 root，重启后仍生效）**：
  `~/.config/wireplumber/main.lua.d/51-disable-alsa.lua` → `alsa_monitor.enabled = false`
  （只禁用 WirePlumber 的 ALSA 音频监视，保留 v4l2/相机监视；PipeWire 继续服务相机）。
  生效：`systemctl --user restart wireplumber`。
- **验证**：`pw-cli ls Node` 无 ALSA 节点、相机节点 2；`pactl` sink `IDLE`、`paplay` rc=0、
  PCM `RUNNING`；连续 3 轮 PA 重启、WP 重启、冷启动顺序模拟均无 EBUSY；重启后配置持久。
- **回滚**：删除该 lua 文件 + `systemctl --user restart wireplumber`。
- **详情/证据/验证命令**：工作区 `audio-fix-20261113.md`。
- **修正旧结论**：此前“CS35L41 `PUP_DONE` 超时（`Enable(1) failed: -110`）导致外放无声”的判断有误——
  本次 boot 的 `Enable(1)` -110 只出现在 18–72s 早期启动（声卡/固件未就绪时 PA 探测），
  72s 后播放时功放上电成功、无 `PUP-done` 超时。**遗留（与无声无关）**：PCM 关闭时仍有
  `Enable(0) failed: -110` / `POST_PMD Main AMP event failed`（功放下电 hibernate 超时，
  不影响的播放；可能略增待机功耗）；固件系数 29.53 vs 驱动期望 29.78 告警仍在。
  后续可查 CS35L41 固件/DTB 供电以消除日志。
- 用户此前的 `audio-card-retry.service`（无 alsa sink 就重启 PA）保留为兜底，未改动。



### 睡眠唤醒后键盘失效（用户态无解）

- **现象**：s2idle 唤醒后键盘无响应，只能物理拔插
- **根因（2026-11-12 只读复查修正）**：日志里的 `dwc3-qcom-legacy a6f8800.usb: port-1 HS-PHY not in L2`
  是 **Type-C/外设控制器（device 模式）的误报**；键盘实际在 `a8f8800.usb/a800000.usb`（host），
  问题在 host+wakeup 的 s2idle fast-resume 只做 PHY runtime resume、不重新初始化 PHY/控制器。
- 键盘 `3206:3ffc` 在 `a800000.usb / xhci-hcd.1.auto / usb1-1`，依赖 `88e4000.phy`
- **A 实验结果（2026-11-12 14:41，无效）**：关闭 `a800000.usb` wakeup + 带充电器 suspend；
  resume 后 USB 设备仍在，但键盘无反应，用户重新拔插键盘才恢复。dmesg 里没有
  USB disconnect/重枚举记录，说明 host/PHY 重初始化不足，**pogo 键盘 MCU/reset/vdd GPIO 更关键**。
  下一步：查 GPIO 141/127/83 与 Android `xiaomi-keyboard` 驱动时序，再决定用户态/内核复位方案。
- 原 A 方案（关 host wakeup）暂不采用；B 方向从“DWC3 PHY reinit”改为“键盘 reset/vdd 复位”。
- **手动复位已验证有效（2026-11-12）**：TLMM 141（sysfs 全局 687）低 100ms → 高，
  USB 键盘会重新枚举（Device 002→003/004），按键恢复；充电保持 1.95A。
- **自动 hook 导致唤醒卡死重启（2026-11-12 14:52，已禁用）**：
  - 安装了 `/etc/systemd/system-sleep/99-keyboard-reset`，在 `post` 里 sleep 1s 后脉冲 687；
  - 一次 suspend 唤醒时系统直接卡死并重启；journal 14:50-14:52 无记录，pstore 是旧记录，
    无调用栈；
  - 高度怀疑在 **xHCI/USB host 尚未完成 resume 时复位键盘**，触发 USB 死锁/看门狗；
  - hook 已移到 `/root/99-keyboard-reset.disabled-20261112-1453`，**不要重新启用**。
- **结论（2026-11-12）**：桌面延迟复位没有实际价值（不如直接拔插），不采用；
  自动复位必须在 **USB/DRM resume 正确时序由内核完成**，否则不做。
- **键盘目标暂停**：保留“拔插键盘”作为现 workaround；不再自动动 GPIO，避免再次看门狗重启。
  若以后继续，只做只读的内核时序设计，先不实机测试。
- 只读调研与候选修复见工作区 `keyboard-resume-research-20261112.md`
- **不要重试**：`authorized` 0→1、`USBDEVFS_RESET`、重绑控制器
  （第三个曾在 `qcom_snps_hsphy_exit` 死锁，搞瘫 USB，热重启都没恢复，第二次重启才好）
- 要修只能改内核 `qcom_snps_hsphy` 的 resume 路径（内建驱动），或报上游
- 旧用户态兜底服务 `lpi-pinctrl-rebind` 已不存在；LPASS pinctrl defer 补丁已部署并重启验证（见「已完成的系统改动」）。

### 充电状态卡死（2026-11-12 事故，已修复）

- **触发**：A 实验在充电器插着时 s2idle suspend/resume；Linux SMB5/TCPM 缺 Android
  `smb5_configure_typec` 的 Type-C legacy 复位，resume 后卡在 `power_role=source`/100mA。
- **为什么热重启没用**：旧 Linux probe 不做 legacy 复位，坏状态跨热重启保留；Android
  SMB5 probe 会清 legacy 状态。
- **修复**：`kernel/patches/0004-smb5-typec-legacy-reset-resume.patch`：
  `qcom_pm8150b_charger.c` 增加 legacy 复位 + `.resume` 重跑初始化/APSD/AICL；
  内核 #7 已写 boot_b（sha256 `dc3ee5...`）。
- **验证**：带充电器连续 3 轮 suspend/resume：`current_max=1950000` 保持、
  电池 Charging、`suspend_stats.success=3 fail=0`、每轮 logs
  `resetting stale Type-C legacy cable detection`。
- **遗留**：`bq2597x present=0`，当前约 1.95A 基础充电；完整快充见待办 2。
- 只读比较见 `docs/CHARGING-DRIVER-COMPARISON.md`。

### 睡眠/唤醒副作用（2026-11-12 观察）

- **Wi‑Fi 短暂断连**：NetworkManager 在 suspend 前主动 `DEAUTH_LEAVING`，resume 后
  ath11k PCI 重新初始化并自动重连（约 1–2s）。日志里未见 ath11k firmware/PCI 失败，
  多数是重连窗口；若某次长时间无网，再查 `journalctl -u NetworkManager -u wpa_supplicant`
  和 `dmesg | grep ath11k`。
- **唤醒后亮屏/触摸慢**：内核 resume 本身约 1.2–1.8s；主要延迟来自：
  - NVT 触摸控制器每次 resume 都重新下载固件（`update_firmware_request` →
    `nvt_update_firmware success <330ms>`），期间触摸可能不可用；
  - SLPI/PDM 每次 resume 的 `sensor_process` crash + `qcom_scm Assign memory protection -22`，
    SLPI 自动恢复、iio 重启（已知传感器残余）；
  - NetworkManager/iio/fprintd/GNOME 用户态恢复叠加。
- **PDM 崩溃**：每次 suspend/resume 后都会出现；传感器数据通常仍可用，但 `check-all`
  的“无 PDM 崩溃”会 FAIL，干净重启后消失。
- **后续可选优化**：触摸 resume 跳过无变化固件重下载；SLPI PDM crash 根因；Wi‑Fi WoWLAN
  或 NetworkManager sleep 行为（暂不优先）。

### 传感器残余风险

- 开机到可用：改造前 ~79s；L2+M1 后 ~55–57s；**L2+M1+M2 后实测 ~22s**（M2 可写 registry，`PRE_DELAY=0`、`IIO_DELAY=8`，详见文末）；
- 约 1/14 次干净启动遇 PD 静默卡死（无崩溃、attach 只读 `hw_platform` 后停住），干净重启恢复；
- suspend/resume 后 PD `frpc_dsp` 可能崩，多数能自动恢复，少数需重启。

---

## 硬件视频编解码（已打通）

- venus 在本内核是 **V4L2 stateful M2M**
- 驱动：`/usr/lib/aarch64-linux-gnu/dri/qcom_drv_video.so`（源码 `~/qcom-va-driver-venus`）
- **Firefox 可用**：`media.ffmpeg.vaapi.enabled=true`，直接走 libva（RDD 进程加载 libva）
- **GStreamer 不可用**：Ubuntu arm64 的 `gst-plugins-bad` 里 `libgstva.so` 是 0 元素的空插件
  → Totem / Epiphany 这类拿不到硬解
- **ffmpeg 的 v4l2m2m 与 venus 不兼容**，别用

---

## 关键细节

- **启动镜像**：`/boot/armbian-kernel-csot.img`，由 `/etc/kernel/postinst.d/zz-update-abl-kernel` 生成；
  cmdline 在 `/boot/armbianEnv.txt` 的 `extraargs`。**该文件被 shell `source`，含空格的值必须加引号**
- **别升 edge 内核**：会与 current 并存，ABL 脚本的 glob 会生成坏镜像
- **sudo 逃生通道**：PAM 钩子坏掉时用 `pkexec`（走 polkit，不经过 `/etc/pam.d/sudo`）；
  回退先删 `/etc/pam.d/sudo` 里的 `sudo-notify` 行，再删脚本和 sudoers 规则
- **`/tmp` 是 tmpfs**：大文件重启即清，不占磁盘
- **硬件状态/升级影响**：统一以 `~/xiaomi-elish-linux-v1-final/docs/HARDWARE-STATUS.md`
  与 `docs/UPGRADE-IMPACT-ALL.md` 为准；安装包/子包 README 里有各硬件的注意事项。

---

## 历史记录

> 2026-11-12/13 的开发、部署与验证流水（已完成的系统改动、摄像头收官、传感器启动加速、卫生清理/安全加固等）
> 已移至 [`HISTORY.md`](HISTORY.md)；本文件只保留当前状态/待办/已知问题。
