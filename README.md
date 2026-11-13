# Xiaomi Mi Pad 5 Pro（elish）— Linux 合并驱动包<br>传感器 · 指纹 · GNSS · 相机

[English](README.en.md)

在 **Xiaomi Mi Pad 5 Pro**（Qualcomm **SM8250 / elish**，CSOT 面板，WiFi 版）上，把 Armbian Linux 的
**传感器（SLPI/SSC）**、**指纹（Goodix GF3626）**、**GNSS（Airoha AG3335）**、**相机（OV8856 / OV13B10，
GPU debayer + Venus 硬编）** 四套功能合并为一个**可重复安装的驱动/系统集成包**：
共享同一内核、DTB 与 boot 镜像，提供**一键安装器**、单组件安装脚本、设备数据提取指南与完整文档。

- **版本**：2026.11.13
- **适用设备**：Xiaomi Mi Pad 5 Pro（elish，CSOT 面板，WiFi 版）
- **适用系统**：Armbian 26.8.14（Ubuntu 24.04 noble）+ kernel `6.18.44-current-sm8250`
- **许可**：本包原创部分 **MIT**（见 `LICENSE`）；上游代码/补丁保留各自许可证（见文末）

> **打包约定**：本包只含**代码、补丁、系统配置、脚本与文档**；**不含**固件、设备数据、日志、
> 指纹模板、密钥/序列号、PAM/sudoers 备份，也不含**预编译用户态二进制**。
> 唯一附带的二进制是**模型级联合 DTB**（无本机私有信息，原因见 `kernel/dtb/README.md`）。
> 厂商数据/固件用 `docs/DEVICE-DATA-EXTRACTION.md` 从**你自己的设备/官方发行包**提取。

---

## 目录

- [支持状态一览](#支持状态一览)
- [适用系统版本](#适用系统版本)
- [实测结果](#实测结果2026-11-13本机同一台设备)
- [包内组件](#包内组件)
- [安装](#安装)
- [构建](#构建)
- [验证](#验证)
- [已知限制](#已知限制)
- [Credit / 上游代码](#credit--上游代码)
- [开源协议](#开源协议mit)

---

## 支持状态一览

> ✅ 完美运行（当前可用、已实测） · ⚠️ 基本运行（可用但有已知限制） · 🚫 受限/未完成 · — 本机不存在

| 子系统 | 组件 | 状态 | 说明 |
|---|---|---|---|
| **传感器** | SLPI/SSC 传感器栈（加速度/环境光/旋转矢量方向/指南针 heading） | ✅ | 自动亮度/自动旋转可用；开机 ~22s 可用（M2 后） |
| 传感器 | 启动时序加速（L2/M1/M2） | ✅ | 服务早期启动 + hexrpc 可写 registry：PD 初始化 35s→8s |
| 传感器 | 休眠唤醒恢复 | ✅ | sleep hook + worker，唤醒 ~10s 后 iio 重新 claim（M2 后） |
| 传感器 | 原始陀螺 / 磁力计三轴 / 接近（SAR） | 🚫 | 用户态 libssc/udev 未暴露；属软件缺口，非硬件驱动 |
| **指纹** | Goodix GF3626（fprintd 录入/验证、GDM 锁屏/登录、sudo 指纹） | ✅ | TA 需自提；TA 拒服务看门狗自动恢复 |
| **GNSS** | Airoha AG3335（QUP5 UART + PAIR + gpsd + GeoClue2） | ✅ | 室外真实定位已验证（mode=3、12 星）；GeoClue 位置与指南针 heading；Pure Maps 已验证 |
| **相机** | 前摄 OV8856 3280×2464 / 后摄 OV13B10 4208×3120 全分辨率拍照/录像 | ✅ | GPU debayer + Venus HEVC/H.264，~30fps；两个 V4L2 虚拟相机 |
| 相机 | 后摄 AF/AE/AWB、CN3927V VCM、EEPROM 24c64 | ✅ | 软件 AF 扫焦写 focus sysfs |
| 相机 | PipeWire / xdg-desktop-portal / Snapshot | ✅ | 两个 `media.role=Camera` 节点，portal 取流已实测 |
| 相机 | Hi556 景深 | 🚫 | 卡在 PM8150A L1/L2 供电方案（进行中） |
| 相机 | 前后摄同时出流 | 🚫 | CAMSS/DTB 资源限制，一次只能一路 |
| **内核/系统** | 联合 DTB（传感器 vmids + 指纹节点 + GNSS QUP5 UART + 相机节点） | ✅ | 模型级、无本机私有信息；构建/校验见 `kernel/dtb/README.md` |
| 内核 | fastrpc SDSP VMID/sensors-PD、QSEECOM/TEE、mdt_loader、qcom_scm、goodix_fp_spi | ✅ | 指纹/传感器内核层 |
| 内核 | LPASS pinctrl clock-timeout defer（pogo 键盘/USB 随机失效） | ✅ | 独立补丁 `kernel/patches/0002-…` |
| 内核 | SMB5 Type-C legacy/100mA resume 卡死 | ✅ | 带充电器 3 轮 suspend/resume 验证 |
| 系统 | 显示/触摸/笔/霍尔/电池/音频/WiFi/BT/USB 键鼠/GPU/UFS/PCIe/Venus | ⚠️ | 基础平台；**外放当前 CS35L41 PUP 超时无声（待查）**，见 `docs/HARDWARE-STATUS.md` |
| 系统 | 屏幕方向/平板模式/自动旋转配置 | ⚠️ | `tablet_mode=on/off/auto`、`rebuild_mode`；依赖保护套霍尔 |
| 系统 | 睡眠唤醒后 pogo 键盘 | ⚠️ | 已知内核 USB PHY resume 问题；重启/重插恢复，用户态不改（防死锁） |
| 系统 | 充电 | ⚠️ | 基础 ~1.95A；快充泵/33W+ 无原装充电器未验证 |
| — | 5G 版本 | — | 本机为 WiFi 版（5G 变体不在本包测试范围；其余杂项硬件所有型号均无） |

完整硬件基线见 [`docs/HARDWARE-STATUS.md`](docs/HARDWARE-STATUS.md)。

---

## 适用系统版本

| 项目 | 值 |
|---|---|
| 设备 | Xiaomi Mi Pad 5 Pro（代号 **elish**，CSOT 面板，WiFi 版） |
| SoC | Qualcomm Snapdragon 870（**SM8250**） |
| 发行版 | **Armbian 26.8.14 noble**（Ubuntu 24.04 LTS 用户态） |
| 内核 | **`6.18.44-current-sm8250`**（Armbian sm8250 基线 + 本包补丁；BTF/ABI 三件套见 `kernel/README.md`） |
| 启动 | Linux 启动槽由 `/boot/armbianEnv.txt` 的 `abl_boot_partition_label` 决定（本机 `boot_b`；脚本自动跟随，**不写死**）；只写该槽，**不碰 Android 槽**（本机 Android 在 `boot_a`，你的布局以实际为准） |
| 面板 | CSOT（本包联合 DTB 为 `sm8250-xiaomi-elish-csot.dtb`；BOE 变体未实测） |
| 内核源码（构建用） | 同版本 Armbian sm8250 源码树（如 `/var/tmp/kbuild-src`）；不随包附带 |
| 依赖 | Ubuntu 24.04 包；一键安装器 `--deps` 自动安装 |

> 传感器数据/固件、指纹 TA、GNSS flash 镜像、相机校准 dump **不在包内**，需从自己的设备或
> 官方发行包提取，见 [`docs/DEVICE-DATA-EXTRACTION.md`](docs/DEVICE-DATA-EXTRACTION.md)。

---

## 实测结果（2026-11-13，本机同一台设备）

同一台 elish（CSOT），Armbian noble + `6.18.44-current-sm8250`，干净启动 + 一次合盖唤醒：

### 启动 / 传感器 / 唤醒

| 指标 | 实测 |
|---|---|
| 桌面就绪（systemd `Startup finished`） | **38.3 s**（kernel 3.2s + userspace 35.0s；graphical 34.8s userspace） |
| 开机 → 传感器可用 | **21.65 s**（`hexrpc-boot.log [start] up=21.65 sensors OK`；改造前 ~79s，−73%） |
| PD registry 初始化（SLPI 起到 sensors OK） | **8.2 s**（M2 可写 registry 前 ~36–42s） |
| iio 状态 | active、单实例、**NRestarts=0** |
| 合盖 → 唤醒 → 传感器恢复/重新 claim | **≈10.2 s**（s2idle；此前 ~35–40s） |
| 自动旋转 | 唤醒后可用（实测） |
| 传感器自检 | `13 项 OK`；1 项 PDM crash 是**休眠唤醒后的已知现象**（干净重启后为 0） |

### 指纹

| 指标 | 实测 |
|---|---|
| `fprintd-list` | Goodix QSEE match-on-chip，已录入 `left-index-finger` |
| 录入 / 验证 | ✅（`fprintd-enroll` / `fprintd-verify`） |
| GDM 锁屏 / 登录 | ✅（`enable-fingerprint-authentication=true`） |
| sudo 指纹 | ✅（`pam_fprintd` 8s 超时→密码回退；2026-11-13 实测按指纹提权成功） |
| TA 拒服务恢复 | ✅ `fp-ta-watchdog` 自动重载 loader |
| 指纹自检 | **OK=20 / WARN=0 / FAIL=0** |

### GNSS

| 指标 | 实测 |
|---|---|
| 链路 | `/dev/ttyHS1`(921600) → `airoha_gps.ko` → `airoha-gnssd` → `/dev/ttyGNSS0` → gpsd |
| NMEA | 1 Hz，已转发句子数 10491+；`gpsd` TPV 正常（室内 `mode=1` 属正常） |
| GeoClue2 | `gps-nmea-share` → `/run/gps-share.sock`；GeoClue Location 可用 |
| 指南针 | `CompassHeading` 实测 ≈176–177°；GeoClue heading 可用 |
| 地图应用 | GNOME Maps（位置）；Pure Maps 3.5.1（位置+罗盘）已验证 |

### 相机

| 指标 | 实测 |
|---|---|
| 前摄 | 3280×2464 PNG；HEVC 20 Mbps 录制 **30.05 fps** |
| 后摄 | 3120×4208 PNG；HEVC 20 Mbps 录制 **29.96 fps** |
| GPU debayer | 前摄 11.0–12.1 ms/帧；后摄 18.7–19.8 ms/帧（p95 ≤14.7 / ≤22.1） |
| Venus 编码吞吐（不限速） | 3280×2464 HEVC **135 fps**；4208×3120 HEVC **90.6 fps** |
| 虚拟相机 | `/dev/video2`（前，NV12 3280×2464）、`/dev/video20`（后，NV12 3120×4208） |
| PipeWire / portal | 两个 `media.role=Camera` 节点；portal `OpenPipeWireRemote` 取流 + Snapshot 已验证 |
| 后摄软件 AF | 实测室内目标最佳 focus=640（0–1023） |
| 相机自检 | **OK=23 / WARN=0 / FAIL=0** |

### 合并自检（`install/check-all.sh`）

| 项 | 结果 |
|---|---|
| 联合 DTB（传感器 vmids + 指纹 + GNSS） | ✅ 通过 |
| 传感器栈 | 13 OK / 1 FAIL（PDM crash 为休眠后已知现象） |
| 指纹栈 | 20 OK / 0 FAIL |
| GNSS 栈 | 正常（非 root 运行时 polkit 规则只能 WARN） |
| initramfs 固件不变量 | ✅ 不含 SLPI 固件 |
| 相机栈 | 23 OK / 0 FAIL |
| apt 状态 | 建议 hold `libfprint-2-2`（否则升级覆盖自编译库） |

> 完整自检建议 `sudo ./install/check-all.sh`（以 root 可读更多系统文件）。

---

## 包内组件

```
README.md / README.en.md / QUICKSTART.md / INSTALL.md / LICENSE
kernel/        统一内核：驱动源码/补丁、内核配置、initramfs hook、模型级联合 DTB、
               一键内核构建安装脚本（build-and-install.sh）、Linux 启动槽重建脚本（自动检测）
sensors/       传感器子包：systemd/udev/D-Bus 集成、启动/唤醒/诊断脚本、
               hexrpc/libssc/iio-sensor-proxy 补丁与编译说明、数据提取脚本、文档
fingerprint/   指纹子包：qsee-supplicant/libfprint 源码与补丁、udev/systemd/PAM 配置、
               TA 提取、安装/验证/卸载脚本、文档
gnss/          GNSS 子包：airoha_gps 驱动源码、airoha-gnssd/gps-nmea-share 用户态、
               systemd/gpsd/GeoClue 集成、协议与排障文档、flash 修复提取说明
camera/        相机子包：elish-camera GPU 管线（前后摄全分辨率、GPU debayer、Venus 硬编、
               虚拟相机、autoserve、自动对焦）、辅助脚本、udev/自启/休眠恢复、
               install.sh + verify.sh、docs/（架构/测量/逆向资料）
install/       安装器：一键安装 turnkey.sh、数据提取 extract-all.sh、用户态构建
               build-userspace.sh、合并自检 check-all.sh、文件落位表
docs/          硬件状态基线、整机 TODO、升级影响、备份/救援、设备数据提取指南
```

---

## 安装

> 详细步骤见 [`QUICKSTART.md`](QUICKSTART.md)（一键）与 [`INSTALL.md`](INSTALL.md)（手动分步）。
> 所有安装脚本支持 `--help` 与 `--dry-run`；已安装组件会自动跳过（`--force` 重编）。

### 0. 前置

- 设备为 Xiaomi Pad 5 Pro（elish，CSOT），已刷入 Armbian noble；
- root（sudo；若已按安全加固删除免密，用**指纹或密码**授权）；
- 两个设备文件（不随包）：
  - Android vendor 分区镜像（如 `vendor_a.img`，只读 dump）
  - pMOS SLPI 固件包 `firmware-xiaomi-elish-slpi-*.apk`
- 指纹 TA（可选；若系统已有 `/lib/firmware/gfenu.*` 会复用）；
- 网络（用户态组件需要 `git clone` 上游源码；内核需要自备同版本源码树）。

### 1. 全组件一键安装（推荐）

```bash
cd ~/xiaomi-elish-linux-v1-final

# 先预览（不写系统）
./install.sh --dry-run --android /path/vendor_a.img --pmos /path/firmware-xiaomi-elish-slpi-*.apk

# 正式安装：依赖 → 提取 → 数据母本/固件不变量 → 用户态补丁+编译 → 系统集成 → 自检
sudo ./install.sh --android /path/vendor_a.img --pmos /path/firmware-xiaomi-elish-slpi-*.apk --yes

# 可选：把内核也一次构建安装（写 Linux boot 槽，自动检测，显式 opt-in，自动备份）
sudo ./install.sh --with-kernel --kernel-src /var/tmp/kbuild-src --write-boot --yes

sudo reboot
sudo ./install/check-all.sh
```

### 2. 单组件安装

#### 2.1 内核 / DTB / initramfs（可选，需要内核源码）

```bash
sudo ./kernel/build-and-install.sh --kernel-src /var/tmp/kbuild-src --write-boot
# 只预检/不写盘： --dry-run
# 复用已编译产物： --skip-build
```
**启动槽位不写死**：脚本读取 `/boot/armbianEnv.txt` 的 `abl_boot_partition_label`
（本机 `boot_b`；若你的 Linux 在 `boot_a` 等其他槽也会自动跟随），`--slot <label>` 只做断言。

脚本自动：应用内核补丁、集成 QSEECOM/mdt_loader/qcom_scm、确保 `q6v5_pas auto_boot=true`、
编译 Image/modules、安装模块（fastrpc/goodix_fp_spi/airoha_gps）、安装联合 DTB、
修 initramfs hook、备份并重建 Linux 启动槽（按 armbianEnv 自动检测；**不碰 Android 槽**）。手动步骤见 `kernel/README.md`。

#### 2.2 传感器

```bash
# 一键（提取 + 数据母本 + 系统集成 + 自检）
sudo ./install.sh --android <vendor.img> --pmos <slpi.apk> --skip-build --yes

# 或分步：
sudo ./install/extract-all.sh --android <vendor.img> --pmos <slpi.apk>
sudo ./sensors/install/install-data.sh --android /var/tmp/elish-build/extracted/android \
     --armbian /var/tmp/elish-build/extracted/armbian --firmware /var/tmp/elish-build/extracted/pmos
sudo ./sensors/install/install-system-files.sh   # systemd/udev/D-Bus/脚本/调参
sudo ./sensors/install/verify.sh
```
用户态二进制（hexagonrpcd + M1/M2 补丁、libssc、iio-sensor-proxy）由
`install/build-userspace.sh` 自动打补丁并编译；手动命令见 `sensors/userspace/*/README.md`。

#### 2.3 指纹

```bash
# TA（从自己设备/adb/固件镜像）
sudo ./install/extract-all.sh --fingerprint-adb --skip-sensors
sudo install -m0644 /var/tmp/elish-build/extracted/fingerprint-ta/gfenu.* /lib/firmware/

# 用户态（qsee-supplicant、libfprint、fprintd、系统集成）
sudo ./install/build-userspace.sh --skip-hexrpc --skip-libssc --skip-iio
sudo ./fingerprint/install/install-fingerprint-files.sh
sudo ./fingerprint/install/verify-fingerprint.sh "$USER"
```
详细（PAM/sudo 指纹、看门狗、回退）见 `fingerprint/INSTALL.md`。

#### 2.4 GNSS

```bash
sudo ./gnss/install/install.sh     # 编译并安装 airoha-gnssd + gpsd/GeoClue 集成
sudo ./gnss/install/verify.sh
```
`airoha_gps.ko` 由内核步骤（或 `--with-kernel`）一并编译安装。

#### 2.5 相机

```bash
sudo ./camera/install.sh           # 编译 elish-camera + 脚本/udev/自启/休眠恢复 + orientation 权限
bash ./camera/elish-camera/verify.sh
```

#### 2.6 安全加固（可选，推荐）

关闭 GDM 自动登录、删除 sudo 免密，保留指纹授权（sudo/登录均可用指纹）：
见 `docs/HARDENING.md`（本机实测：`sudo -n` 需密码、指纹 sudo 可用、唤醒/自动旋转正常）。

### 3. 数据 / 固件提取（本包不附带）

```bash
sudo ./install/extract-all.sh --android <vendor.img> --pmos <slpi.apk> [--fingerprint-adb] [--out DIR]
```
自动提取并校验（registry=155 / config=40 / proto=79 / slpi.mbn md5）。完整清单与手动方法：
[`docs/DEVICE-DATA-EXTRACTION.md`](docs/DEVICE-DATA-EXTRACTION.md)。

---

## 构建

- **一键安装器**会自动 `git clone` 上游源码、应用本包补丁并编译安装（源码缓存在 `/var/tmp/elish-build`）；
- 手动构建步骤：
  - 内核：`kernel/README.md`（步骤 A/B、配置检查、ABI 三件套）
  - hexrpc：`sensors/userspace/hexrpc/README.md`（v0.3.2 + 0001/0003/0004/0005）
  - libssc / iio：`sensors/userspace/*/README.md`
  - 指纹：`fingerprint/INSTALL.md`（qsee-supplicant、libfprint goodix-qsee）
  - 相机：`camera/elish-camera/Makefile`（`make -C camera/elish-camera`）
- 已验证的源码版本/commit 与安装后 sha256 参考见 `VERSIONS.md` 与各子包版本文件。

---

## 验证

```bash
sudo ./install/check-all.sh         # 合并自检（DTB/传感器/指纹/GNSS/相机/initramfs）
sensor-status                       # 传感器总览（服务/固件路径/PDM/iio/数值）
bash camera/elish-camera/verify.sh  # 相机栈
./kernel/compat-check.sh --src <新内核源码树>   # 升级前：补丁/整文件兼容性 dry-run（只读）
sudo ./fingerprint/install/verify-fingerprint.sh "$USER"
sudo ./gnss/install/verify.sh
sudo ./kernel/dtb/verify-combined-dtb.sh
```

---

## 已知限制

- 传感器 PD 偶发静默卡死（约 **1/14** 次干净启动）：固件侧问题，干净重启恢复，非本包可修；
- 休眠唤醒后 PDM crash 1 次属已知现象（传感器数据仍可用；干净重启后自检为 0）；
- 原始陀螺/磁力计三轴/接近未暴露（libssc/udev 层待补）；
- 相机：景深 Hi556 未通；前后摄不能同时出流；前摄固定焦点；Flatpak/COSMIC 的 portal backend 限制；
- pogo 键盘唤醒后可能失效（内核 USB PHY resume；**不要**用 `authorized`/`USBDEVFS_RESET`/重绑控制器）；
- 指纹 TA 需自提；`libfprint-2-2` 升级会覆盖自编译库（建议 hold）；内核/固件升级后需按 `docs/UPGRADE-IMPACT-ALL.md` 重做；
- sudo 指纹 PAM 属安全敏感配置，安装前确保能用密码回退。

---

## Credit / 上游代码

本包是"多上游整合 + 大量逆向/适配/验证"的成果，感谢以下项目与个人：

### 上游代码库

| 项目 | 用途 | 许可 |
|---|---|---|
| [Linux kernel](https://git.kernel.org/) / [mainline](https://github.com/torvalds/linux) | 内核基础与驱动框架 | GPL-2.0 |
| [Armbian build](https://github.com/armbian/build) / armbian-firmware | sm8250 基线、配置、传感器固件数据 | GPL-2.0 / 各自 |
| [linux-msm/hexagonrpc](https://github.com/linux-msm/hexagonrpc) | SLPI/SSC FastRPC 文件服务（hexagonrpcd 0.3.2） | GPL-2.0 |
| [mmcallister/libssc](https://codeberg.org/mmcallister/libssc) | SSC 传感器发现/通信库（0.4.4） | LGPL-2.1 |
| [hadess/iio-sensor-proxy](https://gitlab.freedesktop.org/hadess/iio-sensor-proxy) | IIO→D-Bus SensorProxy | GPL-2.0-or-later |
| [wrobelda/libfprint](https://github.com/wrobelda/libfprint)（goodix-qsee） | Goodix QSEE 指纹驱动 | LGPL-2.1 |
| [wrobelda/qsee-supplicant](https://github.com/wrobelda/qsee-supplicant) | QSEE supplicant / app-loader | GPL-2.0 |
| [postmarketOS](https://gitlab.com/postmarketOS)（device-xiaomi-elish / firmware 包） | SLPI 单文件固件来源 | 各自（pMOS 打包） |
| PipeWire / WirePlumber | 相机/音频媒体管线 | MIT / MIT |
| [v4l2loopback](https://github.com/v4l2loopback/v4l2loopback) | 虚拟相机 | GPL-2.0 |
| Mesa / EGL / GLES / libdrm / GBM | GPU debayer 渲染 | MIT / 各自 |
| GeoClue / gpsd / Pure Maps | 定位栈与地图应用 | LGPL / BSD / GPL |
| quectel-open-source/GNSSTool 等 GNSS 参考 | AG3335 协议参考 | 各自 |
| 社区 hexrpc 可写 registry 补丁（suckbluefrog / gennro / Armada OS 打包） | M2 可写 sensor registry | 随上游补丁 |

### 个人 / AI 贡献

- **caitming** — 项目发起、设备实验、内核/驱动集成、测试与维护；
- **DeepSeek Harness AI Agent（deepseek-flash）** — 承担三份只读逆向分析、传感器启动加速
  L2/M1/M2 与唤醒 R1/R2 的实现、hexrpc 可写 registry 补丁适配、相机/指纹/GNSS 集成验证、
  安装器与文档（含本 README）编写。

---

## 开源协议（MIT）

本包**原创部分**（安装器、系统配置、脚本、文档）以 **MIT License** 发布，见 [`LICENSE`](LICENSE)。

> 上游代码/补丁（内核与驱动补丁、hexagonrpc、libssc、iio-sensor-proxy、libfprint、
> qsee-supplicant、PipeWire、v4l2loopback 等）**保留各自原许可证**（主要为 GPL-2.0 /
> LGPL-2.1 / MIT 等），以各上游仓库的 LICENSE 为准；再分发/修改时请同时遵守相应许可证。

---

## 维护

- 硬件基线：`docs/HARDWARE-STATUS.md`（权威）
- 整机待办/已知问题：`docs/TODO.md`
- 升级影响：`docs/UPGRADE-IMPACT-ALL.md`、`sensors/docs/UPGRADE-IMPACT.md`
- 内核升级风险评估与操作清单：`docs/KERNEL-UPGRADE-RISK.md`
- 回滚/救援：`docs/` 与 `kernel/README.md`、备份脚本（安装内核/系统前会自动备份 Linux 启动槽与关键配置）
