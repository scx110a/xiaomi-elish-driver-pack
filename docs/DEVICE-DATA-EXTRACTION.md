# 设备数据 / 固件提取指南（本包不附带）

本包遵循统一打包约定：

> **只含代码、补丁、系统配置、脚本与文档**；不含固件、编译产物、日志、指纹模板、
> 密钥/序列号、PAM/sudoers 备份，也不含任何**单机私有数据**（如工厂校准 dump）。
> 唯一附带的可执行/二进制是**已验证联合 DTB**（模型级、无本机私有信息，原因见
> `kernel/dtb/README.md`）。

下面把「安装 / 调试 / 分析」可能需要的设备相关文件集中列出，全部从**你自己的设备或官方发行包**提取，
并在提取后按各子包 `install/` 脚本或 README 安装。

> **大部分已自动化**：`install/extract-all.sh` 会自动提取第 1–3 节（armbian/Android/pMOS/指纹 TA，
> 自动在常见位置找输入文件）；`install/build-userspace.sh` 自动编译第 5 节的用户态组件。
> 下面的手工命令用于排障、自定义来源或不想用一键安装器时。

## 1. 传感器数据与 SLPI 固件（安装前，必须）

| 数据 | 来源 | 方法 |
|---|---|---|
| registry（155）、config（40）、socinfo（6）、部分 sensors 根文件 | 已安装的 `armbian-firmware-full` 包 | `sensors/extract/extract-from-armbian-firmware.sh` |
| proto（79）、hals/calibratedCheck/judge_calibrated/sensor_diag/sns_reg_config、可选 acdb | 你自己的 Android vendor 分区镜像（`/etc/sensors`、`/etc/acdbdata`） | `sensors/extract/extract-from-android-vendor-img.sh` |
| SLPI 单文件固件 `slpi.mbn`（7,633,828 B） | pMOS `firmware-xiaomi-elish-slpi-*.apk` | `sensors/extract/extract-pmos-slpi-mbn.sh` |

完整命令与校验：`sensors/extract/README.md`、`sensors/INSTALL.md`。

## 2. 指纹 TA（指纹功能，必须）

`gfenu.mdt` + `gfenu.b00..b07`（9 个文件，约 4.8 MB，Goodix 签名 ELF）——本包不附带：

- 从自己设备 adb：`fingerprint/extract/extract-gfenu-ta.sh --adb ./out`
- 从 Android firmware 分区镜像：`fingerprint/extract/extract-gfenu-ta.sh <firmware_mnt.img> ./out`

详见 `fingerprint/extract/README.md`。

## 3. GNSS flash 镜像（仅 flash 损坏需重刷；正常使用不需要）

`ag3335_bootloader.bin`、`gnss_demo.bin`、`gnss_config.bin`、`slave_da_UART.bin`、
`partition_table.bin` 共 5 个，来自 Android vendor 镜像 `/etc/gnss/`。
提取步骤与风险说明见 `gnss/extract/README.md`（整片擦写，非必要不做）。

## 4. 后摄 EEPROM 校准 dump（可选，仅做校准分析）

后摄 EEPROM 为 **24c64，8 KB @0x51**（at24 驱动自动绑定）。本包**不附带**该 dump
（单机工厂校准数据）。在设备上读取：

```bash
# 1) 确认 at24 已绑定（0x51）
ls /sys/bus/i2c/devices/*-0051/eeprom
# 2) 直接读 8KB（把 <bus> 换成上一步路径里的编号）
sudo dd if=/sys/bus/i2c/devices/<bus>-0051/eeprom of=rear-eeprom-24c64.bin bs=8192 count=1
# 3) 解析（LSC 4ch×17×13 等布局）
python3 camera/elish-camera/docs/evidence/eeprom-24c64-parse.py rear-eeprom-24c64.bin
```

也可用 `i2c-tools`（`i2cdetect -y <bus>` / `i2cdump -y <bus> 0x51`）。
解析结论见 `camera/elish-camera/docs/android-camera-facts.md` §4。

## 5. 用户态 / 内核二进制（本包不附带预编译，按源码编译）

| 组件 | 说明 |
|---|---|
| hexagonrpcd（warm-attach + regver + M2 可写 registry） | `sensors/userspace/hexrpc/README.md`：v0.3.2 + 补丁 0001/0003/0004/0005（`install/build-userspace.sh` 自动完成） |
| libssc / iio-sensor-proxy | `sensors/userspace/libssc/README.md`、`iio-sensor-proxy/README.md` |
| libfprint / qsee-supplicant | `fingerprint/` 子包 README |
| `fastrpc.ko` / `goodix_fp_spi.ko` / `airoha_gps.ko` 等 | 按 `kernel/README.md`、`gnss/` 的编译说明用本包补丁构建 |

部署后的参考 sha256 见 `VERSIONS.md` / 各子包 `VERSIONS.md`（用于核对自行编译的产物）。

## 6. 联合 DTB（唯一随包的二进制，例外）

`kernel/dtb/sm8250-xiaomi-elish-csot.dtb`：模型级、无本机私有信息；当前内核树的 DTS 不完整，
无法安全地从头重编，故随包提供。校验：`kernel/dtb/verify-combined-dtb.sh`。详见 `kernel/dtb/README.md`。

## 7. 校验

- 一键提取：`sudo ./install/extract-all.sh --android <img> --pmos <apk>`；分阶段/手动见各 `extract/` README；
- 各 `extract/` 脚本自带大小/sha256 检查；
- 打包完整性：`cd ~/xiaomi-elish-linux-v1-final && sha256sum -c SHA256SUMS`；
- 安装后：`sudo ./install/check-all.sh`。
