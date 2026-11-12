# Xiaomi Mi Pad 5 Pro (elish) — Qualcomm SLPI 传感器栈（v1-final 安装包）

在 Armbian Linux（kernel `6.18.44-current-sm8250`）上启用 SM8250 的 SLPI 传感器 DSP（SSC），
让环境光、加速度计、旋转矢量方向、指南针 heading 等传感器在 Linux 桌面可用（自动亮度/自动旋转/朝向）。

本归档是**安装包**：只含代码、补丁、系统集成文件与文档；**不含固件、厂商数据、编译产物、日志**。
缺失的设备相关数据/固件在 `INSTALL.md` / `extract/` 中给出确定来源与提取方法。

## 已验证可用

- ✅ 环境光（`HasAmbientLight` / `LightLevel`）
- ✅ 旋转矢量方向（`AccelerometerOrientation`）
- ✅ 指南针 heading（`/net/hadess/SensorProxy/Compass` → `CompassHeading`，来自 rotv 融合；原始磁力计三轴仍未暴露）
  （权威状态见 `../docs/HARDWARE-STATUS.md`）
- ✅ 加速度计 `lsm6dso`（需要 `99-ssc-accel.rules` 启用；`81-libssc-xiaomi-elish.rules` 提供挂载矩阵）
- ℹ️ 固件 mount matrix 全 0 → iio 回退 identity，旋转方向由 81 规则矩阵修正（如方向不对可调该矩阵）
- ⚠️ 陀螺原始数据未提供（仅融合后的 rotv）；proximity 默认未启用

## 开机链路（当前实现）

```
kernel（开机不 auto_boot SLPI）→ slpi-sensors.service（boot+~37s 启动）
  → warm 监听器（hexrpc-sdsp-service.sh，HEXAGONRPC_WARM_ATTACH=1）
  → 把固件放进 /run/sensor-fw 并启动 SLPI（/lib/firmware 路径是悬空符号链接）
  → /dev/fastrpc-sdsp 出现后 hexagonrpcd attach（5–20ms 窗口）
  → iio-sensor-proxy 被 GNOME 经 D-Bus 激活（常驻），等 SSC 设备热插/或 warm restart 兜底
  → libssc 枚举 sensor core → D-Bus SensorProxy → GNOME
```

- 开机到传感器可用：**约 22 秒**（M2 可写 registry 后；改造前 79s；PRE_DELAY=0、IIO_DELAY=8 实测）。
- 每 boot 会在 `/var/log/hexrpc-boot.log` 看到 `iio has no SSC sensors after device appeared (hotplug missed); doing one warm restart`
  ——这是**已知且有意保留的兜底路径**，不影响使用。
- 开机报告：`/var/log/sensor-boot-report.txt`、`/var/log/sensor-last-boot.txt`。

## 已知限制 / 残留风险（详见 `NOTES.md`）

1. **偶发 PD 静默卡死**：约 1/14 次干净开机会出现"attach 只读 hw_platform 就停住、无崩溃、无 assign -22"，
   传感器起不来；干净重启即可恢复。与 PRE_DELAY 无关（0/10/45s 都出现过/都通过过）。
2. **suspend/resume**：合盖唤醒时 PD 可能报 `frpc_dsp` 崩溃；现有 sleep hook + iio 补丁通常能自动恢复，
   少数情况需要重启。
3. **固件不变量**：`/lib/firmware/.../slpi.mbn` 必须始终保持"指向 /run 的悬空符号链接"；
   任何普通固件文件出现在该路径都会导致内核开机抢先 auto_boot → 本次开机传感器不可用。
   升级 `armbian-firmware-full` 后务必检查（`slpi-fw-guard` 会在开机时修正并记录告警）。
4. 本包**不包含**指纹、音频、显示等其它项目内容；也**不包含**任何 Android 分区操作。

## 目录结构

```
README.md / INSTALL.md / NOTES.md / PATCHES.md / VERSIONS.md / MANIFEST.md / SHA256SUMS
../kernel/    统一内核资产（fastrpc 补丁、指纹、LPASS、联合 DTB、initramfs hook）
userspace/    hexrpc 0.3.2 warm-attach 补丁、iio-sensor-proxy 补丁、libssc 说明
systemd/      systemd 单元 / drop-in / udev 规则 / 休眠钩子 / 调参文件 / D-Bus 配置
scripts/      运行时脚本（启动/停止/守卫/诊断/开机报告）
install/      安装/验证/卸载脚本 + 文件落位对照表
extract/      设备数据与固件提取脚本（armbian-firmware / Android vendor 镜像 / pMOS APK）
docs/         安装历史、iio 修复说明、故障排查、验证证据
```

## 快速开始

```bash
# 1) 提取数据（三条来源，见 INSTALL.md 第 2 步）
# 2) 编译安装内核模块与用户态组件（第 3 步）
# 3) 安装系统文件与数据
sudo ./install/install-system-files.sh
sudo ./install/install-data.sh --android ... --armbian ... --firmware ...
# 4) 重启并自检
sudo reboot
./install/verify.sh
```

完整步骤、依赖、验证判据、升级/回滚、故障排查：见 **`INSTALL.md`**、**`NOTES.md`**、**`docs/TROUBLESHOOTING.md`**。

**升级前必读**：`docs/UPGRADE-IMPACT.md`（内核/DTB/boot 镜像、armbian-firmware、iio/hexrpc/libssc
在升级后分别会受什么影响、哪些必须保持原样、哪些可以修补）。

## 红线（必须遵守）

- **不要写 Android 启动槽**；不要改 Android 侧任何分区。（本机 Android 在 `boot_a`、Linux 在 `boot_b`；
  若你的 Linux 在别的槽，以 `/boot/armbianEnv.txt` 的 `abl_boot_partition_label` 为准，本包脚本自动跟随）
- 不要在传感器栈测试中途 `echo stop > /sys/class/remoteproc/*/state`（历史会 RCU stall → panic）。
- 不要用 `dtc` 往返修改 DTB（会破坏 `regulators-1/ldo10` 的 4 字节电压属性）——DTB 修改见 `../kernel/dtb/README.md`。

## 第三方组件

hexagonrpc（linux-msm）、libssc（mmcallister）、iio-sensor-proxy（hadess）、postmarketOS 固件包、
Armbian 内核/固件包。补丁与集成脚本见 `PATCHES.md`。
