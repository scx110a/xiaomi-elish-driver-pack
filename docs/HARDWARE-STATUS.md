# 硬件驱动状态（本机权威基线）

- **设备**：Xiaomi Mi Pad 5 Pro（SM8250 / elish，**WiFi 版**，CSOT 面板）
- **基线**：Armbian noble，kernel `6.18.44-current-sm8250`，已部署本安装包的传感器 + 指纹 + LPASS pinctrl 修复
- **更新日期**：2026-11-13 上午（**音频 PA/WirePlumber 争用已修复**；此前：传感器启动加速 L2+M1+M2、GNSS、相机 portal/Snapshot、屏幕方向/平板模式、休眠唤醒恢复、相机自启）
- **用途**：后续会话/安装/排查**以本文件为准**；它与旧调研报告（见 §6）冲突时，以本文件为准。
- **判定标准**：✅ 已驱动且当前可用；⚠️ 部分驱动/有已知功能缺陷；❌ 有硬件但 Linux 未驱动；— 本机不存在。

---

## 0. 结论速览

| 类别 | 硬件 | 状态 |
|---|---|---|
| **部分驱动** | 前摄 OV8856 8MP（raw/彩色 vcam/扫码）；后摄 OV13B10 13MP（raw/彩色 + CN3927V AF + AE/AWB + EEPROM 24c64 + 4208 全彩）；景深 Hi556 未通 | ⚠️ 后摄 AF/AE/AWB/EEPROM/4208 全彩打通；Hi556 卡在 PM8150A L1/L2 供电 |
| 已驱动 | **GNSS（Airoha AG3335，QUP5 UART + PAIR 协议）** | ✅ 2026-11-12 打通（见 §3、`gnss/README.md`） |
| 已驱动 | 指南针 heading（旋转矢量 rotv 融合 → `CompassHeading`） | ✅（原始磁力计三轴仍未暴露） |
| 部分驱动 | 原始陀螺、原始磁力计三轴、接近/SAR | ⚠️ 用户态未暴露 |
| 传感器启动/可靠性 | L2+M1+M2（2026-11-13）：开机约 **22 秒**可用（改造前 79s）；干净启动约 **13/14** 成功（偶发 PD 静默卡死，重启恢复） | ✅ 已大幅改善 |
| 已驱动（可配置） | 屏幕方向/平板模式：相机方向跟随 Mutter transform；`tablet_mode=on/off/auto`；`rebuild_mode=idle/live/off`；`elish-camera-ctl orientation` 配置工具 | ✅ |
| 已驱动 | 休眠唤醒传感器恢复：sleep hook + `elish-sensors-resume` 服务（等加速计就绪后再重启 iio 让 mutter re-claim）；**连续睡眠加固**：pre 取消 worker + flock + sleep delay inhibitor（2026-11-13） | ✅ M2 后实测唤醒 **~10s、自动旋转可用**；连续睡眠不再叠加旧 worker（此前快速连续合盖可让下一次唤醒拖到 25s+） |
| 已知缺陷 | 连续/快速睡眠后**第二次唤醒屏幕点亮慢**（按电源后黑屏数秒）；日志显示第二次快速唤醒的 DRM unblank 比 PM exit 晚 ~2.5s（正常 0.1s），伴随 gnome-shell JS error，属 GNOME/mutter/DRM resume 路径，非传感器 worker | ⚠️ 传感器加固仅消除“恢复中再睡”的触发；屏幕点亮需单独定位（mutter/DPU/DSI resume） |
| 已驱动 | 相机后端登录自启：`elish-camera-autostart` + `/etc/xdg/autostart/elish-camera-autoserve.desktop` | ✅ |
| 功能缺陷 | 睡眠唤醒后 pogo 键盘可能失效/反复重枚举（host USB PHY resume 问题；重启/重插恢复） | ⚠️ 内核问题，用户态不改（历史危险复位已禁用） |
| 已驱动 | 传感器（加速度/环境光/旋转矢量方向/指南针）、指纹、触摸/笔/霍尔、显示/背光、电池/充电、音频、WiFi/BT、键盘/鼠标、Venus 编解码、GPU/UFS/PCIe、相机闪光灯 | ✅ |
| 本机不存在 | 5G 版本（本机 WiFi 版；5G 变体不在本包测试范围） | — |

---

## 1. 未驱动（真正没起来的硬件）

### 1.1 摄像头（2026-11-12 晚已完整打通，保留条目作历史）

| 项 | 内容 |
|---|---|
| 硬件 | 前摄 **OV8856 8MP**（2-lane，固定焦点）+ 后摄 **OV13B10 13MP**（4-lane，CN3927V VCM）+ 景深 **Hi556 5MP**（未通） |
| Linux 现状 | **前后摄全分辨率彩色拍照/录像已可用**：前 3280×2464、后 4208×3120；GPU debayer（FD650 EGL/GLES）+ Venus 硬件 HEVC/H.264，实测约 30fps；虚拟相机 `/dev/video2`（前）/`/dev/video20`（后），WirePlumber/PipeWire + portal 已验证；后摄软件自动对焦可用 |
| 关键事实 | Bayer：**前摄 BGGR、后摄 GRBG**；前摄必须 `vertical_flip=0`（vflip=1 时 3280×2464 无帧）；Venus encoder/decoder 的 video 节点号不跨启动稳定，需按 sysfs 名发现 |
| 实现/文档 | `camera/elish-camera/`（源码 + install.sh + verify.sh），文档见 `camera/elish-camera/docs/`：architecture、interface、measurements/（历史测量） |
| 未完成 | Hi556 景深（新证据：上电顺序 **3 路供电 → MCLK → 最后释放 reset**，与 OV 相反；仍需 PM8150A L1/L2 供电方案）；前后摄不能同时出流（camss/DTB 限制，需内核工作） |
| 备注 | 相机闪光灯 `/sys/class/leds/white:flash` 已驱动；VCM sysfs 需 video 组可写（`camera/99-elish-vcm.rules`） |

### 1.2 GNSS（Airoha AG3335）—— 已打通（历史条目）

2026-11-12 已完成 Linux 侧驱动并实机验证：芯片从 flash 正常启动（bootloader/firmware
在 **921600**），主机需在上电 ~1 秒内发 `$PAIR002`；`airoha-gnssd` 把 NMEA 转给 gpsd。
**不再属于未驱动硬件**，当前实现见根目录 `gnss/README.md` 与 §3。

> 早期判断“芯片停在 BROM、需要 HDL_START_CMD 运行时加载”是误判：那些日志字符串
> 其实是 BROM 下载握手 `A0/0A/50/05`，且旧测试在错误的 115200 波特率监听。
---

## 2. 部分驱动 / 已知功能缺陷

### 2.1 传感器：加速度/环境光/方向/指南针已可用；原始陀螺/磁力计/接近未暴露

| 项 | 状态 |
|---|---|
| 已工作 | 加速度计、环境光、旋转矢量方向；**指南针 heading 已可用**（来自 rotv 融合，D-Bus `/net/hadess/SensorProxy/Compass` 属性 `CompassHeading`，实测约 176°） |
| 未暴露 | 原始陀螺、原始磁力计三轴（μT）、接近/SAR（`HasProximity=false`） |
| 硬件 | LSM6DSO（加速度+陀螺）、AK991x（磁力计）、TCS3701/BU27030（环境光）、SX932x（SAR/接近） |
| 启动时间 | `PRE_DELAY=0`、`IIO_DELAY=8`：开机到传感器可用 **约 22 秒**（boot+12s 服务 → boot+13s SLPI → boot+22s OK；M2 可写 registry 后 PD 初始化 35s→8s） |
| 可靠性 | 修复固件泄漏/auto_boot、iio 重启风暴与热插后，干净启动成功率从约一半提升到 **约 13/14**；残留为 PD 初始化偶发静默卡死（attach 只读 `hw_platform` 后停住、无崩溃/无 assign -22、registry timeout），与延迟无关，干净重启恢复 |
| 已知兜底 | iio 每次 boot 可能走一次 warm restart（正常）；suspend/resume 后 PD `frpc_dsp` 可能崩，多数能自动恢复，少数需重启 |
| 剩余工作量 | 原始陀螺/磁力计/接近仍需在 libssc/udev 层补类型（天级，不是硬件驱动问题） |

### 2.2 睡眠唤醒后 pogo 键盘失效

- **现象**：s2idle 唤醒后键盘无响应，需物理拔插。
- **根因（2026-11-12 只读复查修正）**：日志里的 `dwc3-qcom-legacy a6f8800.usb: port-1 HS-PHY not in L2`
  是 **Type-C/外设控制器（device 模式）的误报**，不是键盘控制器；键盘实际在
  `a8f8800.usb/a800000.usb`（host）。问题在 host+wakeup 的 s2idle fast-resume 只做 PHY runtime
  resume、不重新初始化 PHY/控制器。调研与候选修复见工作区 `keyboard-resume-research-20261112.md`。
- **现状**：键盘本身已正常驱动（`Xiaomi Pad Keyboard` / USB HID）；问题只在 suspend→resume。
- **不要尝试**：`authorized` 0→1、`USBDEVFS_RESET`、重绑控制器（曾导致 `qcom_snps_hsphy_exit` 死锁、USB 瘫痪）。
- **修复**：需改内核 resume 路径或报上游；用户态无解。
- **2026-11-12 晚实测补充**：一次 suspend 后键盘 USB 进入 **每 ~7 秒断开重连**循环
  （`usb 1-1` device 2→…→100+，每次断开丢键；期间无 USB 错误日志），**重启后恢复为只枚举 1 次**。
  用户态的 `elish-sensors-resume` 只处理 iio/传感器，**刻意不碰 pogo USB**（历史上有 USB 死锁/看门狗教训）。
  遇此现象：重启或物理重插键盘；不要用 `authorized`/`USBDEVFS_RESET`/重绑控制器。

### 2.3 其他小毛病（非驱动缺口）

- PMIC RTC 无有效后备电池时间，断电后可能回到 1970，系统靠 NTP 校准（不影响硬件驱动判定）。
- GPU cooling device 注册告警、DPU frame event overflow 偶发——已知 dmesg 噪音，不影响使用。
- GStreamer 的 `libgstva.so` 在 Ubuntu arm64 是空插件（软件生态问题）；Firefox 走 libva/VA-API 硬解正常。

---

### 2.4 屏幕方向 / 平板模式 / 自动旋转（2026-11-12 晚）

| 项 | 说明 |
|---|---|
| 相机方向 | 后端读 Mutter `DisplayConfig` transform 自动选旋转角：横屏 180°、竖屏 90°；`elish-camera-ctl rotate front|rear <角度>` 可覆盖 |
| 平板模式 | 由保护套折叠霍尔 `SW_TABLET_MODE`（gpiochip3 line121）决定：折到背面=1→GNOME 平板模式（自动旋转+软键盘）；键盘在前/取下=0→笔记本模式 |
| 配置 | `/etc/elish-orientation.conf`：`tablet_mode=on|off|auto`、`rebuild_mode=idle|live|off`；`elish-camera-ctl orientation`（zenai 面板/CLI） |
| 相机流重建 | `idle`：使用中不打断、空闲后按新方向重建；`live`：立即重建并自动重开 Snapshot；`off`：不重建 |
| 霍尔数量 | Android vendor DTBO 另有 `hall_intr3`(gpio136)、`hall_intr4`(gpio138)、`gamekey hall_left`(gpio121)/`hall_right`(gpio64)、键盘 MCU gpio155；mainline 只接 2 个开关，额外霍尔读数恒定、不可用于键盘插拔判断 |
| 已知限制 | Snapshot/aperture 不支持预览中途换分辨率，故默认 idle；唤醒后传感器恢复此前 ~35–40s，M2 后实测 **~10s** |

## 3. 已驱动（当前可用）

| 子系统 | 硬件/证据 |
|---|---|
| 显示 | CSOT 面板 + DPU/DSI；背光 KTZ8866（i2c 11-0011） |
| 触摸/笔 | NT36523：`NVTCapacitiveTouchScreen`、`NVTCapacitivePen`（基础笔输入已驱动） |
| 霍尔/按键 | `gpio-keys`：`switch-lid`/`switch-tablet-mode`（SW=3），电源键/resin |
| 电池/充电 | 双 `bq27z561`、`pm8150b`、`bq2597x` master/slave、USB-C TCPM/UCSI；**2026-11-12：suspend/resume 后 Type-C legacy/100mA 卡死已修复（`kernel/patches/0004`，带充电器 3 轮 suspend/resume 验证；`bq2597x` 快充泵仍可能 present=0，当前约 1.95A 基础充电）** |
| 音频 | `Xiaomi Mi Pad 5 Pro` 声卡；8×CS35L41 外放、耳机、录音。**2026-11-13 已修复无输出**：根因是 PipeWire/WirePlumber ALSA monitor 与 PulseAudio 争用唯一 PCM（`hw:Pro,0`），PA 报“设备或资源忙”、sink 永久 SUSPENDED；用户级 `~/.config/wireplumber/main.lua.d/51-disable-alsa.lua`（`alsa_monitor.enabled=false`）解决，相机 PipeWire 不受影响。遗留 `Enable(0) failed: -110` 为功放下电 hibernate 超时（播放上电正常、无 `PUP-done` 超时，不影响外放）。详见 `docs/TODO.md`「已知问题」音频节 |
| WiFi/BT | QCA6390（ath11k + btqca/hci_uart）；`wlp1s0`/`hci0` 正常 |
| 键盘/鼠标 | USB HID：`Xiaomi Pad Keyboard`、`Xiaomi Pad Mouse`、`Xiaomi Pad`，外接蓝牙鼠标 |
| 传感器 | 加速度/环境光/方向/指南针 heading（见 §2.1） |
| **GNSS** | **Airoha AG3335**：`/dev/ttyHS1`（QUP5 UART，921600）+ `airoha_gps.ko`（供电/时钟/pinctrl）+ `airoha-gnssd`（PAIR002 + NMEA→pty）+ `gpsd`（`/dev/ttyGNSS0`，NMEA0183）+ `gps-nmea-share` → GeoClue2（`/run/gps-share.sock`，GNOME Maps 等取位置）；实机 NMEA 1 Hz，与传感器/罗盘可同时工作；**2026-11-13 室外真实 fix 验证：mode=3、12 星、GGA quality=1（此前仅验证 NMEA/合成 fix）**。地图应用：GNOME Maps 仅位置（不读传感器）；**Pure Maps 3.5.1（Flathub）已安装并验证**（地图渲染 + 合成定位下点击定位跳到目标蓝点；QtSensors iio 插件随 KDE 运行时提供）。朝向：本包 polkit 规则放行 geoclue 的 `ClaimCompass`，实测 GeoClue Location 同时返回位置与 heading≈177°；Pure Maps 的罗盘在跟随/导航且速度<10km/h 时激活（普通浏览模式点定位仅居中）。 |
| 指纹 | Goodix GF3626：fprintd 录入/验证、GDM 解锁、sudo（NOPASSWD 未停用则被绕过） |
| 相机闪光灯 | `/sys/class/leds/white:flash`（v4l2 flash LED） |
| 视频编解码 | Venus V4L2 stateful：`vainfo` → H.264/HEVC 解码/编码；Firefox VA-API |
| GPU/存储/总线 | Adreno/freedreno、UFS、PCIe、USB 主机 |
| PMIC/杂项 | regulator、ADC、thermal、RTC、flash LED、UCSI 等 |

---

## 4. 本机不存在（不要当驱动缺口）

| 硬件 | 依据 |
|---|---|
| 5G 版本 | 本机是 WiFi 版（用户确认）；5G 变体不在本包测试范围 |

> 其余所有 elish 型号均不存在的杂项硬件不再列出；DTB 中也未包含对应节点。

---

## 5. 如何复核（当前系统）

```bash
# 整体自检（含 DTB/传感器/GNSS/指纹/initramfs）
sudo ./install/check-all.sh

# 相机 / 虚拟相机 / PipeWire 节点
elish-camera-ctl status                       # /dev/video2,20 + consumers + PipeWire 节点数
pw-cli ls Node | grep 'elish-camera-'          # 两个原生 PipeWire Camera 节点
v4l2-ctl -d /dev/video2 --get-fmt-video        # NV12 3280x2464（方向随屏幕变化）
elish-camera photo --camera front -o /tmp/front.png   # 前摄 3280x2464 全分辨率

# 屏幕方向 / 平板模式配置
elish-camera-ctl orientation                   # tablet_mode / rebuild_mode
gdbus call --session --dest org.gnome.Mutter.DisplayConfig --object-path /org/gnome/Mutter/DisplayConfig \
  --method org.freedesktop.DBus.Properties.Get org.gnome.Mutter.DisplayConfig PanelOrientationManaged
journalctl -t elish-sensors-resume -b --no-pager | tail   # 唤醒恢复日志
elish-check-usb                                # USB 枚举次数/键盘/平板模式

# GNSS 抽查
gnss/install/verify.sh                        # DTB 节点/设备/服务/NMEA/gpsd
ls -l /dev/ttyHS1 /dev/airoha_gps /dev/ttyGNSS0 /run/gps-share.sock
gpspipe -w -n 5                               # gpsd TPV（室内 mode=1 正常）
systemctl status gps-nmea-share geoclue       # GeoClue 标准地图 API 桥
gdbus call --system --dest net.hadess.SensorProxy --object-path /net/hadess/SensorProxy/Compass \
  --method org.freedesktop.DBus.Properties.Get net.hadess.SensorProxy.Compass CompassHeading

# 已驱动抽查
cat /proc/asound/cards                        # Xiaomi Mi Pad 5 Pro
vainfo | head -5                              # Qualcomm Venus V4L2 VA-API
rfkill list; hciconfig -a | head              # WiFi/BT
grep -A4 'gpio-keys' /proc/bus/input/devices  # SW=3 lid/tablet
grep -A3 'NVTCapacitivePen' /proc/bus/input/devices
gdbus call --system --dest net.hadess.SensorProxy --object-path /net/hadess/SensorProxy \
  --method org.freedesktop.DBus.Properties.GetAll net.hadess.SensorProxy
```

---

## 6. 来源与更新约定

- 本文件基于 2026-11-12/13 的会话实测与核实；历史调研/开发记录已移出包（工作区历史归档）。
- **后续更新硬件状态直接改本文件**：同步 §0 速览、对应小节与“更新日期”，并重新生成包内 `SHA256SUMS`。
- 与旧报告/旧结论冲突时以本文件为准（例如音频：早期判为 CS35L41 故障，实际是 PulseAudio/WirePlumber 争用）。
