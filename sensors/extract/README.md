# 设备数据 / 固件提取（本包不附带这些文件）

安装所需的设备相关数据分三类来源。提取脚本在 `extract/`，组装用 `install/install-data.sh`。

## 来源总表

| 数据 | 文件/数量 | 来源 | 脚本 |
|---|---|---|---|
| registry | 155 个 | **armbian-firmware-full** 包：`/lib/firmware/qcom/sm8250/xiaomi/elish/sensors/registry/` | `extract-from-armbian-firmware.sh` |
| config | 40 个 json | 同上（与 Android vendor `/etc/sensors/config` 逐字节一致） | 同上 |
| sensors 根文件（部分） | `sns_reg.conf`、`sns_reg_version`、`sensors_list.txt`、`sensors_settings` | 同上 | 同上 |
| sensors 根文件（部分） | `hals.conf`、`calibratedCheck.json`、`judge_calibrated.json`、`sensor_diag.cfg`、`sns_reg_config` | **自己的 Android vendor 镜像** `/etc/sensors/` | `extract-from-android-vendor-img.sh` |
| proto | 79 个 | 同上 `/etc/sensors/proto/` | 同上 |
| scripts | 空目录（保留即可） | 同上 `/etc/sensors/scripts/` | 同上 |
| socinfo | 6 个 | **armbian-firmware-full** 包 `elish/socinfo/`（运行期也可由 `/sys/devices/soc0/*` 再生成） | `extract-from-armbian-firmware.sh` |
| acdb（可选） | 9 个 | **自己的 Android vendor 镜像** `/etc/acdbdata/` | `extract-from-android-vendor-img.sh` |
| SLPI 固件 | 单文件 `slpi.mbn` 7,633,828B | **pMOS** `firmware-xiaomi-elish-slpi-20240917-r3.apk` | `extract-pmos-slpi-mbn.sh` |

> 重要：`registry` 不在 Android vendor 镜像里（它在 Android persist 分区）；本包验证配置使用的是
> armbian-firmware-full 包内那份（与 pMOS hexagonfs 的 registry 相同，属发行包默认值，非本机工厂校准）。

## 提取流程

### 1) armbian-firmware 数据

```bash
# 系统已安装 armbian-firmware-full 时：
./extract/extract-from-armbian-firmware.sh ./extracted/armbian

# 未安装时：
sudo apt install armbian-firmware-full
# 或 apt-get download armbian-firmware-full && dpkg-deb -x *.deb ./fw
#   ARMBIAN_FW_ROOT=./fw ./extract/extract-from-armbian-firmware.sh ./extracted/armbian
```

### 2) Android vendor 数据（config/proto/hals/… + 可选 acdb）

```bash
# 从你自己的 vendor 镜像（ext4/稀疏均可，脚本用 debugfs 只读）：
./extract/extract-from-android-vendor-img.sh /path/to/android-vendor.img ./extracted/android

# 或从 Android 设备 adb 提取（需 root）：
adb root
adb pull /vendor/etc/sensors  ./extracted/android/sensors
adb pull /vendor/etc/acdbdata ./extracted/android/acdb   # 可选
```

### 3) SLPI 固件（pMOS APK）

```bash
./extract/extract-pmos-slpi-mbn.sh /path/to/firmware-xiaomi-elish-slpi-*.apk ./extracted/pmos
# 期望 md5: 51c3b3bef20a76d386a516dfbcc72b68
```

- pMOS 包名示例：`firmware-xiaomi-elish-slpi-20240917-r3.apk`（device-xiaomi-elish 固件子包；.apk 是 gzip tar）。
- 备选：armbian-firmware-full 自带拆份固件（`slpi.mbn` + `slpi.b00..b20`），同源但**本项目只验证过单文件形式**。
- 安装时 `install-data.sh` 会把 armbian 拆份固件移到 `slpi-disabled/`，避免内核开机 auto_boot。

### 4) 组装

```bash
sudo ./install/install-data.sh \
  --android ./extracted/android \
  --armbian ./extracted/armbian \
  --firmware ./extracted/pmos
```

脚本会校验数量（registry 155 / config 40 / socinfo 6 / slpi.mbn 7,633,828B），
并把 `/lib/firmware/.../slpi.mbn` 设为指向 `/run/sensor-fw/slpi.mbn` 的符号链接。

## 校验清单

| 检查 | 期望 |
|---|---|
| `find /usr/share/qcom-ssc-elish/sensors/registry -type f | wc -l` | 155 |
| `find /usr/share/qcom-ssc-elish/sensors/config -type f | wc -l` | 40 |
| `find /usr/share/qcom-ssc-elish/sensors/proto -type f | wc -l` | 79 |
| `find /usr/share/qcom-ssc-elish/socinfo -type f | wc -l` | 6 |
| `md5sum /usr/share/qcom-ssc-elish/slpi.mbn` | `51c3b3bef20a76d386a516dfbcc72b68` |
| `ls -l /lib/firmware/qcom/sm8250/xiaomi/elish/slpi.mbn` | 符号链接 → `/run/sensor-fw/slpi.mbn` |

## 常见问题

- **找不到 registry**：registry 不来自 Android vendor 镜像，请用 armbian-firmware 脚本。
- **pMOS APK 解包失败**：确认下载的是 `.apk`（Alpine 包，gzip tar）；也可用 `apk fetch` 获取。
- **socinfo 缺失**：可用 Android 侧（root）读取 `/sys/devices/soc0/{hw_platform,platform_subtype,...}`
  生成同名文件；或从 armbian-firmware 包取。
- **vendor 镜像是 sparse**：安装 `android-sdk-libsparse-utils` 提供 `simg2img`（脚本会自动转换）。
