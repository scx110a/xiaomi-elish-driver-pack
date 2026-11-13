# 小米平板 5 Pro (SM8250/elish) 系统笔记

更新于 2026-11-13 凌晨（相机/屏幕方向/平板模式/唤醒恢复/自启动收尾/传感器启动加速 L2+M1+M2 已验证）。待办按难度排序。

> 本文是**整机 Linux 状态摘要**：硬件/驱动覆盖、待办与已知问题。
> 硬件/驱动的权威基线是 `~/xiaomi-elish-linux-v1-final/docs/HARDWARE-STATUS.md`；
> 安装/升级/排障以合并安装包 `~/xiaomi-elish-linux-v1-final/` 及其子包文档为准
> （`sensors/`、`fingerprint/`、`gnss/`、`kernel/`），本文件不重复细节。
> 总自检：`sudo ~/xiaomi-elish-linux-v1-final/install/check-all.sh`。

---

## 整机硬件 / 驱动状态（2026-11-12）

| 类别 | 硬件/功能 | 状态 |
|---|---|---|
| 已驱动 | 显示（CSOT + DPU/DSI）、背光 KTZ8866、触摸/手写笔 NT36523、霍尔/盖子/平板模式 | ✅ |
| 已驱动 | 电池/充电（双 bq27z561、pm8150b、bq2597x、USB-C TCPM/UCSI） | ✅ |
| 已驱动 | 音频（Xiaomi Mi Pad 5 Pro 声卡、8×CS35L41 外放、耳机、录音） | ✅ |
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
      预期 PD 初始化 35s→5–10s，开机/唤醒同时受益（详见 `sensor-speedup/IMPLEMENTATION-20261112.md`）；
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
- 只读调研与候选修复见 `keyboard-resume-research-20261112.md`
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
- 只读比较见 `charging-driver-comparison-20261112.md`。

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

## 已完成的系统改动

**启动**
- 曾用 `lpi-pinctrl-rebind` 用户态自愈服务兜底开机 pinctrl/键盘问题；**当前系统已无该服务**
  （2026-11-12 核查，`systemctl`/`/etc/systemd` 均不存在）。**不要用 bind 重试**救急：
  实测会卡死在内核 D 状态。
- 禁用 `serial-getty@ttyS0`、`apt-xapian-index.timer`：开机 95s → 13.8s
- plymouth 启用 `splash quiet`；GDM 登录界面改中文

**输入 / 显示**
- `10-ignore-xiaomi-pad-mouse.rules`：屏蔽键盘附带的隐形鼠标
- QQ / mihomo 改原生 Wayland（修撕裂、ibus-rime 输入法）
- `10-wifi-disable-powermanagement.rules`

**通知 / 钥匙串**
- 装 `gjs`：GNOME 通知服务靠它拉起子进程；`apt autoremove` 会带走，注意
- 装 `libpam-gnome-keyring`：登录自动解锁钥匙串

**提权**
- 免密 sudo + PAM 审批弹窗（zenity 允许/拒绝，30s 超时自动拒绝）

**硬件支持（合并安装包）**
- **SMB5 充电 suspend/resume 隐患已修复（2026-11-12）**：合并包
  `kernel/patches/0004-smb5-typec-legacy-reset-resume.patch` + `kernel/CHARGER-RESUME.md`；
  内核 #7 已写 boot_b，带充电器 3 轮 suspend/resume 验证通过（current_max 保持 1.95A、
  电池 Charging、`resetting stale Type-C legacy cable detection` 正常触发、失败 0）。
  包 SHA256SUMS 已重算（198 条）；`check-all` 当前唯一 FAIL 是测试 suspend 触发的
  已知传感器 PDM 崩溃（数据仍可用），干净重启后应恢复。
- 合并包 `~/xiaomi-elish-linux-v1-final/` 提供统一内核/DTB、传感器（SLPI/SSC）、
  指纹（Goodix QSEE）、GNSS（UART + 用户态守护进程）等支持与自检脚本；
- 传感器服务 `slpi-sensors`/`slpi-fw-guard`/`iio-sensor-proxy`；指纹 `qsee-supplicant`/`fprintd`；
- 地图/位置标准 API 已接入（GeoClue2），Pure Maps 3.5.1（Flathub）已安装并验证地图与定位；
- LPASS pinctrl defer 补丁已部署并重启验证（2026-11-12）：boot_b 已更新，新镜像 kernel+DTB
  与原 candidate2 一致（仅 initramfs 增加补丁模块）；启动后 `dmesg` 无 lpass 报错、pogo 键盘正常、
  `install/check-all.sh` 全部通过；备份/回滚见 `/root/lpass-deploy-backup-20261112-113856/DEPLOY-REPORT.txt`。

**资源**
- 固件精简 `/etc/dpkg/dpkg.cfg.d/10-firmware-trim`：46 条 `path-exclude`，
  `/lib/firmware` 2.2G→350M。`dpkg -V` 会报 ~3100 条 missing，属正常噪音
- 卸载 Epiphany flatpak、清 apt/chromium 缓存、卸 Ubuntu 6.8 内核 / snapd / rc 包

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

## 历史：libcamera + PipeWire/portal 调研（已被自研 PipeWire 相机节点方案取代）

> **已过时（2026-11-12 晚）**：最终实现未走 libcamera，而是自研原生 PipeWire
> `media.role=Camera` 节点（带完整 EnumFormat + `api.libcamera.location`），
> 详见 `HARDWARE-STATUS.md` §2.4 与 `camera/elish-camera/docs/measurements-integrated.md`。
> 以下内容仅作历史调研记录。

- **定位**：前/后摄驱动和 raw 链路稳定后，再统一解决“标准 App/视频通话”的相机接入；
  这是摄像头适配的最后一块，不阻塞当前后摄 OV13B10 基础驱动。
- **目标**：
  - OV8856/OV13B10 以**原生 libcamera 相机**出现，不依赖 v4l2loopback；
  - PipeWire/WirePlumber 暴露相机节点，`xdg-desktop-portal`（GNOME）Camera portal 可用；
  - Cheese、浏览器、Flatpak 视频通话 App 能直接看到前后摄；
  - 保留 `elish-camera-vcam` 作为直连 V4L2 App 的兜底。
- **当前障碍**：
  - Flatpak 的 libcamera 0.8 **没有 OV8856 sensor properties/helper**；运行时不可覆盖，
    需要重建/替换 Flatpak 内 libcamera，或统一用系统 libcamera；
  - GNOME 的 `org.freedesktop.portal.Camera` 当前没有可用相机节点；
  - v4l2loopback 不是 libcamera 相机，PipeWire/portal 默认不会枚举它；
  - 当前 `ov8856.ko` 的 BGGR+VFLIP 是让 libcamera media pipeline 先跑起来的补丁，
    原生 libcamera 修好后应评估回退，避免长期依赖驱动侧 hack。
- **任务清单**：
  1. 给 libcamera 补 OV8856/OV13B10 的 `CameraSensorProperties`（Bayer/曝光/AWB/方向）
     和 `CameraSensorHelper`（黑电平/增益）；优先上游化；
  2. 解决 Flatpak libcamera 版本问题：重建 Flatpak、换系统 libcamera，或提供 portal 侧统一后端；
  3. 检查/启用 PipeWire 的 libcamera/V4L2 camera 节点和 WirePlumber 会话管理；
     确认 `pw-cli ls Node` 出现相机，`/org/freedesktop/portal/desktop` 的 Camera API 可用；
  4. 实测 Cheese、Chromium/Firefox（视频通话页面）、Flatpak 相机；保留 Guvcview 直连验证；
  5. 如果 portal 路线仍受限，再评估 PipeWire V4L2 桥接（把 vcam 暴露为 PipeWire 相机）作为过渡。
- **验收**：
  - 不启动 vcam 也能在 Cheese/浏览器中看到前摄（后摄同理）；
  - 视频通话 App 可选择相机并取到彩色画面；
  - 曝光/AWB/方向正常，无驱动侧 Bayer hack；
  - Guvcview/vcam 仍可作为 V4L2 fallback。
- **依赖/顺序**：前摄稳定 → 后摄 OV13B10/Hi556 → 本项收官。


---

## 摄像头（已完成，2026-11-12 晚）

- **状态（2026-11-12 晚）：已完成**。前摄 3280×2464、后摄 4208×3120 全彩拍照/录像，
  GPU debayer（FD650）+ Venus HEVC/H.264 硬编，实测约 30fps；`/dev/video2`（前）/
  `/dev/video20`（后）标准 V4L2 NV12；后端同时发布两个原生 PipeWire 相机节点
  （`elish-camera-front/rear`，带 `api.libcamera.location=front/back`，portal fd 可见），
  GNOME Snapshot 前后摄预览/切换/拍照/录像实测可用；后摄软件 AF/AE/AWB。
- **方向/平板模式**：相机方向按 Mutter 显示 transform 自适应（横屏 180°、竖屏 90°）；
  `elish-camera-ctl orientation tablet on|off|auto` 控制平板模式（auto 跟随保护套折叠霍尔）；
  `rebuild_mode=idle|live|off` 控制旋转时相机流重建（idle=空闲重建，live=立即重建并自动重开 Snapshot）。
- **自动启动**：登录桌面自动拉起后端（`elish-camera-autostart`）。
- **剩余工作**：Hi556 景深见待办 §3；Snapshot 不支持预览中途换分辨率，
  故默认 `rebuild_mode=idle`，`live` 会重启 Snapshot。
  （前摄固定焦点、前后摄同时出流属于硬件/DTB 限制，见“硬件限制”一节，不作为 TODO。）
- **文档**：`camera/elish-camera/docs/{architecture,measurements-integrated,measurements-gpu,measurements-encode,interface,android-camera-facts}.md`；
  历史开发交接文档 `camera/elish-camera/docs/` 已删除，内容已并入上述文档与本节。


## 摄像头收官：全分辨率 GPU 管线（2026-11-12 晚完成）

- 项目：`elish-camera/`（已并入合并包 `camera/elish-camera/`，系统安装 `/usr/local/bin/elish-camera`）
- 能力：前摄 3280×2464 / 后摄 4208×3120 全分辨率彩色拍照 + HEVC/H.264 硬件编码录像（实测 ~30fps）；
  GPU debayer（FD650 EGL/GLES，前 11–12ms、后 19–20ms 每帧）；Venus 编码吞吐 >90fps。
- 通用接口：`/dev/video2`（前）/`/dev/video20`（后）标准 V4L2 NV12 30fps；
  WirePlumber 自动发布 `media.role=Camera` 节点，portal Camera `IsCameraPresent=true`；
  ffmpeg/GStreamer/Guvcview/pipewiresrc 均实测可用。
- 已知限制：前后摄**不能同时出流**（一次一路）；后摄重负载下 25–29fps；Hi556 景深仍未通
  （新证据：上电顺序为 3 rail→MCLK→最后释放 reset，见 `elish-camera/docs/android-camera-facts.md`）。
- 注意：前摄必须 `vertical_flip=0`（vflip=1 时 3280×2464 无帧）；Venus encoder/decoder
  video 节点号不跨启动稳定，按 sysfs 名发现。
- 详情：`elish-camera/docs/{architecture,measurements-integrated,measurements-gpu,measurements-encode,interface,android-camera-facts}.md`

---

## 2026-11-12 晚：屏幕方向 / 平板模式 / 唤醒恢复 / 相机自启（已全部并入安装包）

### 1. 屏幕方向与平板模式

- 相机后端根据 Mutter `DisplayConfig` 的 transform 自动选旋转角（横屏 180°/竖屏 90°），
  显式覆盖用 `--rotate-front/--rotate-rear` 或 `elish-camera-ctl rotate front|rear <角度>`。
- 平板模式来自保护套霍尔 `SW_TABLET_MODE`（gpiochip3 line121，active-low）：
  - 保护套折到背面 → 1 → GNOME 平板模式（自动旋转 + 软键盘）；
  - 键盘在前/保护套取下 → 0 → 笔记本模式（不自动旋转/不弹软键盘）。
- 实测主板只把 2 颗霍尔接进 input：`SW_LID`(gpio110) + `SW_TABLET_MODE`(gpio121)。
  Android vendor DTBO 里另有 `hall_intr3`(gpio136)、`hall_intr4`(gpio138)、
  `gamekey hall_left`(gpio121)/`hall_right`(gpio64)、键盘 MCU gpio155，但 mainline 未启用
  （读取恒为 1/1/0/0），无法用于判断键盘插拔。
- 配置工具：`elish-camera-ctl orientation`（zenity 面板）或
  `elish-camera-ctl orientation tablet on|off|auto`、`... rebuild idle|live|off`；
  配置文件 `/etc/elish-orientation.conf`。
- 已知限制：Snapdragon 上 pogo 键盘 USB 在 suspend 后会重连循环/失效（内核 PHY resume 问题，
  用户态无解；重启或重插恢复）；本次会话已观察到并记录，未修改任何危险复位路径。

### 2. 休眠唤醒传感器恢复

- v1 hook 的后台进程会被 systemd-sleep 的 cgroup 杀掉；且唤醒后 SLPI 传感器栈约 35s 才回来，
  过早重启 iio 会导致 mutter 无法 claim 加速计。
- v2：`/usr/lib/systemd/system-sleep/elish-sensors-resume`（post）→
  `systemctl --no-block start elish-sensors-resume.service`（独立 cgroup）→
  worker：等加速计（最多 60s，必要时拉起 SLPI）→ 传感器就绪后再重启 iio →
  mutter 重新 claim → 再确认 30s。实测唤醒后 ~40s 自动旋转恢复（M2 后 10.2s，见文末）。
- 诊断：`journalctl -t elish-sensors-resume -b --no-pager`。

### 3. 相机后端自启

- `/usr/local/bin/elish-camera-autostart`（等 PipeWire socket）+
  `/etc/xdg/autostart/elish-camera-autoserve.desktop`（登录执行）；实测后端启动、
  `/dev/video2`/`/dev/video20` 就绪、两个 PipeWire 相机节点发布。

### 4. 合并包更新

- `camera/system/`：resume hook/service/worker；
- `camera/elish-camera-autostart`、`camera/elish-camera-autoserve.desktop`、`camera/elish-check-usb`；
- `camera/elish-camera-ctl`（orientation/rebuild 配置）、`camera/elish-camera/tools/hall_monitor.c`；
- `docs/HARDWARE-STATUS.md` 与 `docs/TODO.md` 已同步；`SHA256SUMS` 重新生成。


---

## 2026-11-12/13 深夜：传感器启动加速（L2+M1+M2 已重启验证：开机 ~22s）

> 三份只读分析 + 实施记录：`sensor-speedup/`（`sensor-boot-slow-analysis.md`、
> `sensor-resume-slow-analysis.md`、`android-sensor-init-notes.md`、`IMPLEMENTATION-20261112.md`）。
> 救援/回滚：`~/000-急救说明-传感器加速-20261112.md`；
> 备份：`/root/sensor-speedup-backup-20261112-230727/rollback.sh`。

**根因（已确认的 3 层）**
1. `slpi-sensors.service` 原 `After=multi-user.target`，被 GDM/plymouth 链拖到 boot+37s 才启动；
2. SLPI 起后 PD registry 初始化 ≈35s，其中 hexrpc 0.3.2 虚拟 persist 目录：
   - `.../registry/registry/../sns_reg_version` 不可达（虚拟目录没暴露该文件）→ 每次 attach 重跑 registry 生成；
   - `temp.json` 写被 `AEE_EUNSUPPORTED` 拒绝，固件 10× 退避重试（每次 attach 约 1570 次）→ 20–25s；
   - Android 侧证据：`init.vendor.sensors.rc` 在 `early-boot` 就启动 SLPI、persist 有 registry 书签；
     vendor 数据与我们逐字节一致；vendor 无 dog/超时参数可调（详见 android 笔记）；
3. iio 在 +8s 盲 warm restart，systemd 等旧进程 graceful stop（阻塞在 libssc 30s 等待）≈27s，
   与 PD 初始化并行，只拖长服务时长。

**已实施（rootfs only，未写 boot_a/boot_b/Android）**
- `slpi-sensors.service`：去 `After=multi-user.target`，改 `After=slpi-fw-guard.service`；
  `Type=exec`（避免 multi-user 等待 oneshot 完成而拖慢桌面）；
- `slpi-sensors-start.sh`：早期启动先等内核 slpi remoteproc；iio 轮询数据 + 健康检查 150s
  （原 ≥45s 无数据 SIGKILL 硬重启兜底已于 2026-11-13 删除，见文末）；
- `iio-sensor-proxy` drop-in：去 `After=multi-user.target`，加 `TimeoutStopSec=3`；
- `/opt/hexrpc-032b/bin/hexagonrpcd.regver-v1`（补 sns_reg_version 映射）+ 监听器优先使用；
- 唤醒 worker 轮询 0.5s、删除无效 slpi-sensors 兜底；休眠钩子 post 去重（防多 listener 争抢）；
- 一次性诊断 `sensor-timing-diag.sh` → `/var/log/sensor-timing.log`（420s 后自退）。

**重启验证结果（2026-11-12 23:35 重启，23:39–23:42 只读验证）**
- ✅ 传感器可用 **54.8s**（改造前 ~79s，−24.2s / −31%）：boot=23:35:31，
  `hexrpc-boot.log [start] up=54.83 sensors OK`（23:36:26），iio `HasAccelerometer=yes up=54.10`；
  SLPI 起于 up=13.07（改造前 boot+37.7s 才起服务）→ 加速收益来自启动时序提前。
- ✅ 桌面/multi-user **23:36:09（boot+38.1s）**，`Startup finished 38.401s`；改造前同口径 79.5s
  （boot -1/-2/-3 均 ~79.3–79.5s），未被传感器栈拖慢；systemd PID1 **无 ordering cycle**（0 条）。
- ✅ 无 PDM 崩溃（0）、`SDSP assign try[0]: 0`、`systemctl --failed` 0 个；**无内核 auto_boot**
  （3.743s 内核自启尝试因固件路径悬空失败 `-2`；12.878s 的成功加载是服务触发；`/run/sensor-boot-bad` 不存在）。
  注：`sensor-status` 的 `AUTO_BOOT` 旧启发式已于 2026-11-13 修正（改用 bad-marker + 固件加载/服务启动单调时刻对比），见文末。
- ✅ 新二进制生效：监听器进程为 `hexagonrpcd.regver-v1`，最后 attach 头 `(hexagonrpcd.regver-v1)`；
  `sns_reg_version` openat 成功，**version 读失败 0**。
- ⚠️ `temp.json` 写失败仍 **1570 次/attach**（与改造前相同）→ M1 未缩短 PD registry 初始化；
  M2（hexagonfs 可写 persist）仍是最大收益点 → **已于 2026-11-13 实施并验证，见下**。
- ⚠️ iio `NRestarts=1`（验收期望 0）：脚本 45s 兜底因**开机 NTP 步进 +21.887s**（chronyd 23:35:57）
  用 wall-clock（`date +%s%3N`）计时而提前触发——iio 实际只跑 ~26s 就被 SIGKILL，重启后 54s 才拿到数据
  （估计多花 ~5s）。→ 该兜底已于 2026-11-13 整体删除（M2 后不再需要），见文末。
- 未回滚；唤醒路径**未验证**（需用户合盖实测），日志：`journalctl -t elish-sensors-resume -b`。
  → M2 后已于 2026-11-13 00:08 实测 **10.2s**，见下。
- 报告文件：`/var/log/sensor-boot-report.txt`（timer 自动生成，本 boot VERDICT: OK（NRestarts=1 仍判 OK））。

**M2 验证（2026-11-13 00:01 重启：hexrpc 可写 sensor registry）**
- ✅ **传感器可用 up=21.65s**（`sensor-timing`：HasAmbientLight=yes up=20.78、HasAccelerometer=yes up=21.42；
  `hexrpc-boot.log [start] sensors OK up=21.65`）——改造前 79s，−57.3s / −73%；比桌面（~38s）还早 ~16s。
- ✅ **PD registry 初始化 8.2s**（SLPI start issued up=13.49 → 传感器 OK up=21.65；改造前 ~36–42s）。
- ✅ 写路径：195 次写模式 open（155 registry + 40 config，全部 `.../registry/../temp.json`）成功、
  517 次 fwrite 全部成功（~199KB，0 错误）；`Tried to open ... for writing` 1570 → **0**；
  registry 155+40 个文件成功写回 `/run/sensors-rootsvc` 副本（temp.json 被 rename 掉，无残留）。
- ✅ **iio NRestarts=0**、单实例；`slpi-sensors` NRestarts=0（M1 时的 NRestarts=1 副作用消失——
  传感器 21s 就绪；该 45s 兜底已于 2026-11-13 删除）。
- ✅ **合盖唤醒实测 ~10.2s**（2026-11-13 00:08：s2idle resolve 00:08:22.8 → iio 重新 claim 完成
  00:08:33.1；resume 后 attach 同样 195 次写/517 次成功/0 错误；此前 ~35–40s）；
  PDM 崩溃 1 次（`EX:sensor_process ... frpc_dsp`，已知休眠后现象，传感器数据正常）。
- ✅ 桌面 `Startup finished 38.285s`（未被拖慢）；PDM 崩溃 0、`SDSP assign try[0]: 0`、无 auto_boot
  （bad-marker none）、ordering cycle 0、failed units 0；attach 段 2800 → 2136 行。
- 二进制：`/opt/hexrpc-032b/bin/hexagonrpcd.writable-v1`（sha256 `68809903…`；监听器优先使用，
  回退链 writable-v1 → regver-v1 → 原厂）；源码/补丁/离线测试 `~/hexrpc-m2/`；
  回滚：`sudo rm /opt/hexrpc-032b/bin/hexagonrpcd.writable-v1` + 重启。
- 唯一 open 失败是历史一直缺的 `oemconfig.so`（无害）。

**2026-11-13 凌晨：卫生清理 + 合并包体检**
- ✅ 修正 `sensor-status` 的 `AUTO_BOOT` 旧启发式（写死"服务 boot+60s 才起、<30s 即内核 auto_boot"，
  加速后服务 up=12s 起必然误报）：改为 fw-guard 的 `/run/sensor-boot-bad` 标记 +
  "固件加载时刻 vs slpi-sensors 单调启动时刻"对比；现在输出
  `AUTO_BOOT=no (fw loaded by service at 13.3s; kernel early attempt blocked by dangling symlink)`。
- ✅ 删除 `slpi-sensors-start.sh` 的 iio "≥45s 无数据 → SIGKILL 硬重启一次" 兜底（系统 + 合并包同步）：
  M2 后传感器 21s 就绪、iio 热插拔已验证可靠，兜底永不触发；且它用 wall-clock 计时会被开机 NTP 步进
  误触发（M1 阶段 NRestarts=1 的根因）。健康检查（150s）与轮询逻辑保留；下次重启即用新脚本
  （正常路径行为不变）。
- ✅ 合并包体检（`~/xiaomi-elish-linux-v1-final/`）：按"只含代码/补丁/配置/脚本/文档，唯一二进制是 DTB"
  的打包约定，删除 3 个设备/环境相关项：
  1) `camera/rear-eeprom-24c64-20261112.bin`（8KB 后摄工厂校准 dump，单机私有）；
  2) `sensors/userspace/hexrpc/prebuilt/` 两个预编译 ELF（编译产物；补丁 + 编译说明保留）；
  3) `camera/elish-camera/src/capture.c` 里硬编码的本机家目录锁路径 → 运行时路径
     （`$ELISH_CAMERA_LOCK_DIR` → `$XDG_RUNTIME_DIR/elish-camera-locks` → `/tmp/elish-camera-locks`）。
  新增 `docs/DEVICE-DATA-EXTRACTION.md`（传感器/指纹/GNSS/相机 EEPROM/二进制编译的统一提取指南）；
  更新 README/INSTALL/MANIFEST/VERSIONS/FILES-MAP/install 脚本/SHA256SUMS（269 文件、3.0MB、无 ELF）。

**2026-11-13 凌晨：合并包自动化（傻瓜包）+ 安装审计**
- 备份（改动前）：`~/xiaomi-elish-linux-v1-final.bak-20261113-002050`（SHA256SUMS 校验通过；保留可整体回退包内容）。
- 新增一键安装入口 `install.sh`（= `install/turnkey.sh`）：
  `sudo ./install.sh --android <vendor.img> --pmos <slpi.apk> --yes` 自动完成 apt 依赖 → 数据/固件提取 →
  数据母本 + 固件路径不变量 → 补丁应用 + 用户态编译安装 → 系统集成 → 自检。
- 新增自动化脚本：`install/extract-all.sh`（自动找输入并校验 registry=155/config=40/proto=79）、
  `install/build-userspace.sh`（hexrpc 0001/0003/0004/0005 + libssc + iio + qsee-supplicant + libfprint +
  fprintd，已装自动跳过、`--force` 重编）、`kernel/build-and-install.sh`（内核补丁/配置/编译/模块/DTB/
  initramfs/boot_b，自动备份，`--write-boot` 显式才写盘）、`camera/install.sh`（修复 README 里悬空的
  install.sh 引用，含相机全部安装步骤）。
- 手动步骤缩减为：①提供 Android vendor 镜像 + pMOS SLPI APK（本包不含）；②重启一次；
  ③可选：内核构建（需同版本 Armbian 源码树）、指纹录入、sudo 指纹 PAM。
- 验证：所有脚本 `bash -n` + `--help` + `--dry-run` 通过；未在本机执行真实安装（本机已装好），
  安装器按"已装即跳过"保证可重复执行。包清洁度复查：276 文件、3.1MB、无 ELF、无单机私有数据。
- 合并包仍不含设备数据/固件/编译产物，唯一二进制为模型级联合 DTB（联合 DTB 的提取/构建说明见
  `kernel/dtb/README.md`）。

**2026-11-13 凌晨：安全加固（关闭自动登录 + 删除 sudo 免密，保留指纹授权）**
- GDM 自动登录：`/etc/gdm3/custom.conf` → `AutomaticLoginEnable=false`（保留 `AutomaticLogin=caitming` 便于恢复）；
  **下次开机/重启 GDM 后生效**（当前已登录会话不受影响）。
- sudo 免密删除（2 处）：
  1. `/etc/sudoers.d/010-caitming-nopasswd`（全量 `NOPASSWD: ALL`）→ 已移到备份；
  2. `/etc/sudoers` 的 `caitming ALL=(ALL) NOPASSWD: /usr/bin/psd-overlay-helper` → 已删除
     （psd 当前 symlink 模式、不用 overlayfs，无需该 helper；`020-nopasswd.disabled` 文件名含 `.` 被 sudo 忽略，未动）。
  `visudo -c` 通过；`sudo -n true` 现在返回“需要密码”。
- 指纹授权保留（未改动）：sudo 的 `/etc/pam.d/sudo`（`pam_fprintd` 8s 超时→密码回退）、
  GDM 指纹（`enable-fingerprint-authentication=true` + `pam_fprintd`）、fprintd 已录入 left-index-finger。
- 备份/恢复：`~/security-hardening-backup-latest/`（含 `restore.sh`、原 sudoers/gdm 配置、README）；
  root 镜像在 `/root/security-hardening-backup-20261113-005009/`。
  恢复：`sudo bash ~/security-hardening-backup-latest/restore.sh`。
- ✅ 补充（已执行，2026-11-13 00:5x）：相机两个辅助脚本原本依赖 `sudo -n`（删免密后会静默失败）：
  `/usr/local/bin/elish-camera-ctl`（写 `/etc/elish-orientation.conf`）与
  `/usr/local/bin/elish-camera-rear-autofocus`（写 VCM focus sysfs）。已提供一次性修复脚本：
  `sudo bash ~/security-hardening-backup-latest/fix-camera-sudo-deps.sh`
  （改为组权限 + 去掉脚本里的 sudo，不再需要任何免密）。基础相机/开机自启本来不依赖它。
- ⚠️ 影响：DSH Agent 以 caitming 运行且无交互终端，**以后不能再执行 sudo**；需要 root 的操作请在桌面终端
  手动执行（指纹/密码），或先临时恢复免密。日常（传感器/相机/DSH）不受影响。
- 验证建议：终端 `sudo -k; sudo true`（按指纹或输密码）；重启后 GDM 应要求登录（指纹/密码）。
- ✅ 相机 sudo 依赖修复已执行（用户按指纹授权）：`/etc/elish-orientation.conf` → `root:caitming 0664`；
  `elish-camera-ctl` / `elish-camera-rear-autofocus` 已去掉 `sudo -n`；实测
  `elish-camera-ctl orientation set tablet_mode auto` 无 sudo 写入成功；同时**实测指纹 sudo 授权链路可用**
  （执行时输出“请把您的左手食指放在指纹读取器上”并成功提权）。原脚本备份
  `~/security-hardening-backup-latest/pre-fix/`。

**2026-11-13 凌晨：驱动包文档与脚本收尾**
- 包内脚本同款 sudo 依赖问题已修：`camera/elish-camera-ctl`、`camera/elish-camera-rear-autofocus`
  去掉 `sudo -n`（与系统已修版本一致）；`camera/install.sh` 安装时把 `/etc/elish-orientation.conf`
  设为安装用户组可写（0664），focus sysfs 由 udev 规则保证 video 组可写 → 无 sudo 免密也可用。
- 新增 `camera/elish-camera/verify.sh`（相机只读自检，实测 23 项 OK）；`gnss/install/verify.sh` 的
  polkit 检查改为 root-aware（非 root 显示 WARN 而非 FAIL）；`install/check-all.sh` 相机项现在能自检。
- 驱动包 README 重写：`README.md`（中文详细版）+ `README.en.md`（英文）+ `LICENSE`（MIT）+
  `docs/HARDENING.md`（安全加固）；内容含包内组件、完美/基本/受限运行状态、适用系统版本、
  实测结果（开机 ~38s/传感器 21.65s/PD 8.2s/唤醒 10.2s/相机 30fps 等）、单组件与一键安装步骤、
  上游 credit（含 DeepSeek Harness Agent）。
- 包统计：280 文件、3.2MB、无 ELF（唯一二进制为联合 DTB）；`SHA256SUMS` 重新生成并校验。

**2026-11-13 凌晨：boot 槽位不再写死（适配别人的安装槽位）**
- 审查发现关键路径原已读 `/boot/armbianEnv.txt` 的 `abl_boot_partition_label`，但仍有硬编码：
  `build-and-install.sh` 备份固定 `boot_b`、`install-new-kernel.sh` 固定写 `boot_b`/`slot_suffix=_b`、
  多处文档与回滚命令固定 `boot_b`。
- 已改为统一自动检测：`update-boot-image.sh` / `build-and-install.sh` / `install-new-kernel.sh`
  都从 `abl_boot_partition_label` 读取 Linux 启动槽（本机 boot_b；别人可能是 boot_a），
  未设置时拒绝盲写；`--slot <label>` 仅作断言（与 armbianEnv 不一致直接报错）。
- 备份文件名改为 `<label>-backup-*.img`；README（中/英）/INSTALL/QUICKSTART/kernel 与 sensors 文档
  全部改为"Linux 启动槽（自动检测）"，历史记录保留"本机 boot_b"说明。
- 验证：全包无 `by-partlabel/boot_b` 硬编码；槽位检测在本机得到 boot_b 且分区存在；脚本 `bash -n` + `--help` 通过。
- 包统计不变（283 个受控文件）；SHA256SUMS 重新生成并校验；已 commit + push。

**2026-11-13 凌晨：内核升级兼容性工具 + 不存在硬件清理**
- 新增 `kernel/compat-check.sh`：升级前只读检查补丁 dry-run + 整文件副本 diff + 关键符号/DTS；
  实测：Armbian `linux-6.18.y` HEAD 7/8 补丁 clean（0004 目标在补丁集里，分支镜像无）；
  mainline `v6.19` 下 0001 fastrpc、0005 ov8856 有 hunk 冲突，其余 clean。
- 新增 `docs/KERNEL-UPGRADE-RISK.md`：churn 数据、dry-run 结果、升级清单、移植 vs 从零评估。
- 按用户要求**不加 GitHub Actions**（本地/手动跑 compat-check 即可）。
- 文档清理：删除"本机不存在"里所有型号均无的杂项硬件条目，只保留 5G 版本一项
  （这些硬件所有 elish 型号都没有；DTB 中也无对应节点）。
- 包统计：283 个包文件；SHA256SUMS 重新生成并校验；commit + push。

**2026-11-13 上午：GNSS 真实定位修复（PAIR 运行时初始化）**
- 症状：室外 5 分钟无 fix；有 16 星/SNR 20–40，但 GSV 仰角/方位为空、gpsd SKY=0。
- 根因：`airoha-gnssd` 只发 `$PAIR002`（打开 DSP），未发星座/NMEA 配置与 hot start。
- 实测：向芯片补发 `$PAIR066,1,1,1,1,0,0` + `$PAIR062` 系列 + `$PAIR086,0` + `$PAIR004`
  后，室内即可看到 GSV talker 切换（命令被接受）；室外 2 分钟内 `mode=3`、12 星、GGA quality=1。
- 已改源码 `gnss/userspace/airoha-gnssd.c`，启动后自动发送该序列（runtime-only，不写 flash）；
  文档/verify 已更新。
- ✅ 2026-11-13 08:13 已安装到系统（指纹 sudo）：`/usr/local/sbin/airoha-gnssd` 更新，
  `systemctl restart airoha-gnssd/gpsd/gps-nmea-share`；日志确认
  `runtime init sent: PAIR021/066/062/086/004`，服务 enabled → 重启后仍生效。
- 包内已提交推送 commit `92cb942`；tag `26.8.14` 已移到 `92cb942`；`git status` clean。
- 办公室内 `mode=1` 属正常（无星）；下次室外 30–60 秒内应恢复 `mode=3`（此前实测 12 星、quality=1）。

**磁盘清理（同日）**：删除 16 个重复 boot_b 全分区备份、相机调试一次性备份、重复内核镜像、
GNSS candidate boot img、apt 缓存等，释放 ~4.1GB（46G→42G used）；保留
`/root/boot_b-known-good-20261112-2305.img`（sha256 f204631e…）与文档引用的 rollback 点。
