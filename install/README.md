# 安装 / 自动化脚本说明（合并包）

## 一键安装（推荐）

```bash
sudo ./install.sh --android /path/vendor_a.img --pmos /path/firmware-xiaomi-elish-slpi-*.apk --yes
sudo reboot
sudo ./install/check-all.sh
```

`install.sh` = `install/turnkey.sh` 的包装。自动完成：

1. `--deps`：apt 依赖（可用 `--no-deps` 跳过）；
2. 提取：`install/extract-all.sh`（armbian-firmware 数据 / Android vendor 数据 / pMOS `slpi.mbn` /
   指纹 TA，自动在 `./`、`~/`、`~/Downloads`、`/root` 找输入）；
3. 数据母本：`sensors/install/install-data.sh`（含固件路径不变量）；
4. 用户态：`install/build-userspace.sh`（hexrpc 补丁+编译、libssc、iio-sensor-proxy、
   qsee-supplicant、libfprint、fprintd；已装自动跳过，`--force` 重编）；
5. 系统集成：sensors/fingerprint/GNSS/camera 各子包安装脚本；
6. 内核（可选，显式）：`kernel/build-and-install.sh --kernel-src <源码> --write-boot`；
7. 自检：`install/check-all.sh`。

其他参数：`--dry-run`（只演示）、`--yes`、`--skip-extract|--skip-build|--skip-install|--skip-check`、
`--no-fingerprint|--no-gnss|--no-camera`、`--pam`（sudo 指纹，安全敏感）。

## 分阶段脚本

| 脚本 | 作用 |
|---|---|
| `install/turnkey.sh` | 全流程编排（install.sh 调用它） |
| `install/extract-all.sh` | 设备数据/固件提取（本包不附带这些文件） |
| `install/build-userspace.sh` | 打补丁 + 编译 + 安装用户态组件（源码缓存在 `/var/tmp/elish-build`） |
| `install/check-all.sh` | 合并自检（内核/传感器/GNSS/指纹/相机/apt 状态） |
| `sensors/install/install-data.sh` | 组装 `/usr/share/qcom-ssc-elish` + 固件符号链接不变量 |
| `sensors/install/install-system-files.sh` | 传感器 systemd/udev/D-Bus/脚本/调参 |
| `fingerprint/install/install-fingerprint-files.sh` | 指纹 udev/systemd/脚本（`--pam --user` 可选） |
| `gnss/install/install.sh` | GNSS 守护进程 + gpsd/GeoClue 集成 |
| `camera/install.sh` | 相机后端/脚本/udev/虚拟相机/自启/休眠唤醒恢复 |
| `kernel/build-and-install.sh` | 内核补丁/配置/编译/模块/DTB/initramfs/Linux 启动槽（自动检测；自动备份） |

各脚本都支持 `--help` 与 `--dry-run`；除特别说明外需 `sudo`。

## 仍需手动（极少）

1. 提供 Android vendor 镜像与 pMOS SLPI APK（本包不附带；见 `docs/DEVICE-DATA-EXTRACTION.md`）。
2. 重启一次（内核/服务/固件路径生效）。
3. 可选：内核构建（需同版本 Armbian 源码树）、指纹录入、sudo 指纹 PAM。

详细步骤：`../QUICKSTART.md`、`../INSTALL.md`。
