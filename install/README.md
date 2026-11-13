# 安装 / 自动化脚本说明（合并包）

## 一键安装

完整流程见 [`../QUICKSTART.md`](../QUICKSTART.md)（唯一权威）。`install.sh` = `install/turnkey.sh` 的包装，
自动完成：apt 依赖（`--no-deps` 跳过）→ 设备数据/固件提取 → 数据母本（含固件路径不变量）
→ 用户态补丁+编译 → 系统集成 →（可选内核）→ 自检。
其他参数：`--dry-run`、`--yes`、`--skip-*`、`--no-fingerprint|--no-gnss|--no-camera`、`--pam`。

## 分阶段脚本

| 脚本 | 作用 |
|---|---|
| `install/turnkey.sh` | 全流程编排（install.sh 调用它） |
| `install/extract-all.sh` | 设备数据/固件提取（本包不附带这些文件） |
| `install/build-userspace.sh` | 打补丁 + 编译 + 安装用户态组件（源码缓存在 `/var/tmp/elish-build`） |
| `install/check-all.sh` | 合并自检（内核/传感器/GNSS/指纹/相机/apt 状态） |
| `install/backup-boot.sh` | 备份 Linux 启动槽 / boot 镜像 / DTB（`--pam` 加备份 PAM；槽位自动检测） |
| `sensors/install/install-data.sh` | 组装 `/usr/share/qcom-ssc-elish` + 固件符号链接不变量 |
| `sensors/install/install-system-files.sh` | 传感器 systemd/udev/D-Bus/脚本/调参 |
| `fingerprint/install/install-fingerprint-files.sh` | 指纹 udev/systemd/脚本（`--pam --user` 可选） |
| `gnss/install/install.sh` | GNSS 守护进程 + gpsd/GeoClue 集成 |
| `camera/install.sh` | 相机后端/脚本/udev/虚拟相机/自启/休眠唤醒恢复 |
| `kernel/build-and-install.sh` | 内核补丁/配置/编译/模块/DTB/initramfs/Linux 启动槽（自动检测；自动备份） |

各脚本都支持 `--help` 与 `--dry-run`；除特别说明外需 `sudo`。

## 仍需手动

见 [`../QUICKSTART.md`](../QUICKSTART.md)「仍需手动的事」。详细分步：`../INSTALL.md`。
