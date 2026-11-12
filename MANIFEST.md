# 合并包清单（MANIFEST）

合并包 = 传感器安装包（已清理）+ 指纹安装包（已清理）+ GNSS 子包 + 共享内核/DTB 资产 + 合并文档。
**不含**固件、编译产物、日志、指纹模板、密钥/序列号、PAM/sudoers 备份。

## 顶层

```
README.md / README.en.md / QUICKSTART.md / INSTALL.md / NOTES.md / PATCHES.md / VERSIONS.md / MANIFEST.md / LICENSE / SHA256SUMS
install.sh     一键安装入口（= install/turnkey.sh 的包装）
kernel/        统一内核（详见下）
sensors/       传感器子包（自包含，58 文件）
fingerprint/   指纹子包（自包含）
gnss/          GNSS 子包：airoha_gps 驱动源码、airoha-gnssd 用户态、systemd/gpsd 集成、
               协议与逆向结论（docs/PROTOCOL.md）、flash 修复说明（extract/）
install/       一键安装器（turnkey/extract-all/build-userspace + lib/）与合并自检（check-all.sh）
docs/          合并升级影响评估（UPGRADE-IMPACT-ALL.md）+ 硬件状态权威基线（HARDWARE-STATUS.md）
               + 整机 TODO（TODO.md）+ 设备数据/固件提取指南（DEVICE-DATA-EXTRACTION.md）
               + 安全加固说明（HARDENING.md）
               + 内核升级风险评估（KERNEL-UPGRADE-RISK.md）
camera/        相机子包：elish-camera/（前后摄全分辨率 3280×2464/4208×3120，GPU debayer + Venus 硬编，~30fps 拍照/录像，/dev/video2+/dev/video20 双虚拟相机，autoserve 自动路由，PipeWire/portal，后摄自动对焦）+ 旧 OV8856 helper + install.sh（一键安装）+ elish-camera/verify.sh（自检）
               + Android/Linux 充电驱动对比（CHARGING-DRIVER-COMPARISON.md）
```

## kernel/

| 内容 | 说明 |
|---|---|
| `README.md` | 统一内核构建步骤、配置/ABI 校验、联合 DTB/boot 镜像 |
| `build-and-install.sh` | 一键补丁/配置/编译/模块/DTB/initramfs/Linux 启动槽（按 armbianEnv 自动检测；自动备份；不碰 Android 槽） |
| `compat-check.sh` | 升级前只读兼容性检查：补丁 dry-run + 整文件副本 diff + 关键符号/DTS 检查（支持 --src / --fetch-ref） |
| `qseecom/` | 指纹 QSEECOM TEE 驱动 |
| `mdt_loader.c` / `qcom_scm.c` | 指纹内核文件 |
| `goodix_fp_spi/` | 指纹传感器驱动源码（无编译产物） |
| `config-used-6.18.44` / `kernel.release` | 内核配置/版本 |
| `ARMBIAN-BASELINE.md` | Armbian sm8250 基线获取说明（不随包补丁集） |
| `patches/0001-fastrpc-...patch`、`fastrpc.c.modified` | 传感器 fastrpc 补丁/完整文件 |
| `patches/fingerprint/` | 指纹内核补丁：**qseecom-tee-series.patch（14 文件权威系列）** + fp-spi ×4 + scm-qseecom 分片 |
| `boot/` | `zz-update-abl-kernel` 系统 hook 副本（参考；统一用 update-boot-image.sh） |
| `integration/` | DTS vmids 片段（完整基线重建参考） |
| `patches/0002-pinctrl-lpass-lpi-defer-on-clock-timeout.patch`、`PINCTRL-LPASS.md` | LPASS pinctrl defer（pogo 键盘/USB，独立问题） |
| `patches/0003-qcom-mdt-loader-user-selectable.patch` | QCOM_MDT_LOADER 用户可选（TEE_QSEECOM 依赖） |
| `patches/0004-smb5-typec-legacy-reset-resume.patch`、`CHARGER-RESUME.md` | SMB5 suspend/resume 后 Type-C legacy/100mA 卡死修复 + 验证记录 |
| `update-boot-image.sh` | 预检 DTB/initramfs + 备份目标 Linux 启动槽 + 重建 + 回读校验（槽位自动检测；`--slot` 断言） |
| `install-new-kernel.sh` | 安装内核并写 Linux 启动槽的脚本（自动检测；历史参考） |
| `include/` | 指纹驱动必需的 3 个内核头文件 |
| `INTEGRATION.md` | 头文件 + drivers/tee Kconfig/Makefile 集成步骤 |
| `initramfs/` | 修复版 initramfs hook + 安装脚本（防止 SLPI 固件进 initramfs） |
| `dtb/` | 已验证联合 DTB（二进制，模型级）+ 说明/参考/校验脚本 |

## sensors/

原传感器安装包的用户态部分：`README/INSTALL/NOTES/PATCHES/VERSIONS/MANIFEST`、`userspace/`（hexrpc/iio/libssc）、
`systemd/`、`scripts/`、`install/`、`extract/`、`docs/`（TROUBLESHOOTING、IIO-FIX、HANDOFF-SENSORS、
UPGRADE-IMPACT、evidence）。内核资产统一在顶层 `kernel/`（不再在 sensors/ 内重复）。

## gnss/

Airoha AG3335 GNSS 子包（2026-11-12 打通）：

| 内容 | 说明 |
|---|---|
| `README.md` / `INSTALL.md` / `NOTES.md` | 子包说明、安装、限制与排障 |
| `docs/PROTOCOL.md` | 主机协议（PAIR ASCII）、启动链/波特率、anld 下载流程与镜像表（仅修复 flash 用） |
| `kernel/` | `airoha_gps_driver.c` + Makefile：`/dev/airoha_gps` 的 OPEN/CLOSE/DF/DI（电源/时钟/pinctrl），无内核源码补丁 |
| `userspace/` | `airoha-gnssd.c`：上电 + `$PAIR002` + NMEA→pty + 自愈；`gps-nmea-share.c`：gpsd NMEA → GeoClue socket（均为纯 C，无第三方依赖） |
| `systemd/` | `airoha-gnssd.service`（Type=notify）、`gps-nmea-share.service`、gpsd/geoclue drop-in |
| `polkit/` | `49-iio-sensor-proxy-geoclue.rules`：放行 geoclue 用户 claim iio 传感器（GeoClue compass heading 必需） |
| `install/` | `install.sh`（含 gpsd 配置）、`verify.sh`、`uninstall.sh` |
| `extract/` | 仅当 GNSS flash 损坏需重刷时的厂商文件提取说明（正常使用不需要） |

## fingerprint/

| 内容 | 说明 |
|---|---|
| `README/INSTALL/NOTES/MANIFEST` | 指纹子包文档（安装指向 `../kernel/`） |
| `src/qsee-supplicant/` | supplicant + app-loader 源码（0.1.1，无二进制/.git） |
| `src/libfprint/` | goodixqsee 驱动 + 日志补丁（相对 430e24e5） |
| `system/` | pam.d、systemd、udev、bin、user-bin 配置/脚本 |
| `extract/` | `extract-gfenu-ta.sh` + TA 来源与 sha256 |
| `install/` | 安装/自检/卸载脚本 |
| `docs/` | HARDWARE-STATUS（硬件权威基线）、LOCAL-LIBFPRINT（本地同名 libfprint 包）、HANDOFF-FINGERPRINT、DT-COMPARE、REPORT、PORTING-PLAN、ANDROID-ANSWER |

## 明确不包含 / 需现场获取

| 内容 | 原因 | 获取方式 |
|---|---|---|
| SLPI 单文件固件 `slpi.mbn` | 厂商固件二进制 | `sensors/extract/extract-pmos-slpi-mbn.sh`（pMOS APK） |
| sensors registry/config/socinfo、proto/hals、acdb | 设备型号数据 | `sensors/extract/`（armbian-firmware + Android vendor 镜像） |
| 指纹 TA `gfenu.*`（4.8MB） | 厂商签名固件 | `fingerprint/extract/extract-gfenu-ta.sh`（自己设备） |
| GNSS 厂商镜像（`ag3335_bootloader.bin` 等 5 个） | 已在设备 GNSS flash 内且能正常启动；**正常使用不需要** | 仅 flash 损坏时：`gnss/extract/README.md`（Android vendor 镜像提取） |
| `fastrpc.ko`、`goodix_fp_spi.ko` | 与内核 ABI 绑定 | 按 `kernel/README.md` 编译 |
| 后摄 EEPROM 校准 dump（24c64，8KB） | 单机工厂校准数据（本机私有） | 从自己设备 at24 sysfs 读取：`camera/README.md` §EEPROM + `docs/DEVICE-DATA-EXTRACTION.md` §4 |
| hexagonrpcd/libssc/iio/libfprint/qsee 二进制 | 编译产物（本包不附带任何预编译） | 按各子包 README 编译 |
| 日志、指纹模板、PAM/sudoers 备份、Android 抓包/工具 | 运行记录/私有/非安装必需 | 不随包 |

## 统计

**283 个文件；体积约 3.2MB**（不含 SHA256SUMS）。**无 ELF/固件/日志/TA/指纹模板/单机校准 dump**；唯一二进制是模型级联合 DTB；`SHA256SUMS` 已按最终树生成。
