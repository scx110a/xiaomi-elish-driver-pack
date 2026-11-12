# 版本与校验值（VERSIONS / BOM）

> 本包不包含任何编译产物、固件、厂商数据。下列 sha256/md5 用于安装后交叉核对。

## 内核与模块

| 项 | 值 |
|---|---|
| 运行内核 | `6.18.44-current-sm8250`，#6 SMP Tue Nov 10 05:54:26 CST 2026，aarch64 |
| 编译器 | gcc 13.3.0（Ubuntu 13.3.0-6ubuntu2~24.04.1） |
| fastrpc 补丁基准 | Linux **v6.18.44** pristine（`/var/tmp/linux-6.18.44.tar.xz`） |
| 安装后 fastrpc.ko sha256 | `fe2cd7ba1899f516b7858e914af21132122505457da425557d8647c64b640134` |
| 模块路径 | `/lib/modules/6.18.44-current-sm8250/kernel/drivers/misc/fastrpc.ko` |
| 补丁 | `../kernel/patches/0001-fastrpc-sdsp-vmid-and-sensors-pd.patch`（2 hunk，55 行） |

## hexagonrpcd

| 项 | 值 |
|---|---|
| 上游 | https://github.com/linux-msm/hexagonrpc |
| 版本 | tag **v0.3.2**（commit `b7eff23db1b11541d11175bd3f42bd4c85214baf`） |
| 补丁 | `userspace/hexrpc/patches/0001-hexrpc-0.3.2-warm-attach.patch`（+30/−3） |
| M2 补丁 | `patches/0003-hexagonrpcd-add-writable-hexagonfs.patch`、`0004-hexagonrpcd-support-writable-sensor-registry.patch`、`0005-hexagonrpcd-support-large-and-extended-callbacks.patch`（社区 Armada/SteamOS-ARM，适配本机 v0.3.2；writable 构建 = 0001+0003+0004+0005，不叠 0002） |
| 安装二进制 | `/opt/hexrpc-032b/bin/hexagonrpcd`（aarch64，sha256 参考 `ec9c370e4ddb95196e4bf56a5001a93aad67381d56e1eccc935b90fe54ea6312`） |
| M2 构建参考 | `/opt/hexrpc-032b/bin/hexagonrpcd.writable-v1`（自行编译；sha256 参考 `68809903…`，默认优先；本包不附带二进制） |
| 回退构建参考 | `/opt/hexrpc-032b/bin/hexagonrpcd.regver-v1`（自行编译；sha256 参考 `c52ecf31…`；本包不附带二进制） |
| 配套库 | `/opt/hexrpc-032b/lib/aarch64-linux-gnu/libhexagonrpc.so`（sha256 参考 `c88c92c31a0cf237d959c4750437df07c1f6704575457bf1c5bd63a2e4203a1b`） |
| 说明 | 必须 0.3.x；0.5.0 会 `Unsupported handle: 0xffffffff` |

## libssc

| 项 | 值 |
|---|---|
| 上游 | https://codeberg.org/mmcallister/libssc |
| 版本 | **0.4.4**；本机验证快照 `v0.4.4-9-g4e4c6c8`（HEAD `4e4c6c8cc702a754bd700506c01961aaf971a23b`） |
| 构建 | `meson setup build --prefix=/usr/local -Dintrospection=false` + `ninja install` |
| 安装库 | `/usr/local/lib/aarch64-linux-gnu/libssc.so.2`（sha256 参考 `a46d9ca7878ea7d58f4de382b4f58f323d97cb3f4c5ec7fef5e74f91e27fe944`） |

> 若严格复现本机状态用 `4e4c6c8`；追求“原始 0.4.4”可用 tag `v0.4.4`，但未逐项回归验证。

## iio-sensor-proxy

| 项 | 值 |
|---|---|
| 上游 | https://gitlab.freedesktop.org/hadess/iio-sensor-proxy |
| 版本 | commit **`ca0d5b08bd647afc5f8ead3ce00a44018fbc633f`**（describe `3.9-1-gca0d5b0`） |
| 补丁 | `userspace/iio-sensor-proxy/patches/0001-iio-sensor-proxy-hotplug-stayalive.patch`（+33/−9） |
| 构建 | `meson setup build --prefix=/usr -Dssc-support=enabled` + `ninja install` |
| 安装二进制 | `/usr/libexec/iio-sensor-proxy`（sha256 参考 `c6e707c4e01d2461ce182e5bc5b69c17a1449b77def5939eb2a21e5cb306d49d`） |

## 固件（不随包提供）

| 项 | 值 |
|---|---|
| 推荐形式 | pMOS 单文件 `slpi.mbn`，7,633,828 字节 |
| 来源包 | `firmware-xiaomi-elish-slpi-20240917-r3.apk`（postmarketOS） |
| md5 | **`51c3b3bef20a76d386a516dfbcc72b68`** |
| 备选 | `armbian-firmware-full` 自带拆份固件（`slpi.mbn` 8,380B + `slpi.b00..b20`），同源但本项目只验证过单文件形式 |

## 数据（不随包提供）

| 数据 | 期望数量/校验 | 来源 |
|---|---|---|
| `sensors/registry/` | **155** 个文件（含 31 个 `*.fac_cal*`，为发行包默认值） | armbian-firmware-full 包（Armbian 从 Xiaomi vendor 抽取） |
| `sensors/config/` | **40** 个 json（sm8250_*.json） | 同上（与 Android vendor `/etc/sensors/config` 逐字节一致） |
| `sensors/proto/` | **79** 个（`*.proto` 等） | 自己的 Android vendor 镜像 `/vendor/etc/sensors/proto` |
| `sensors/` 根文件 | `sns_reg.conf`、`sns_reg_version`、`sensors_list.txt`、`sensors_settings` | armbian-firmware-full 包 |
|  | `hals.conf`、`calibratedCheck.json`、`judge_calibrated.json`、`sensor_diag.cfg`、`sns_reg_config` | 自己的 Android vendor 镜像 `/vendor/etc/sensors` |
| `socinfo/` | **6** 个：`hw_platform=ELISH`、`platform_subtype=strange`、`platform_subtype_id=2`、`platform_version=589824`、`revision=2.1`、`soc_id=356` | armbian-firmware-full 包 |
| `acdb/`（可选） | `adsp_avs_config.acdb` + `Forte/*.acdb`（9 个） | 自己的 Android vendor 镜像 `/vendor/etc/acdbdata` |
| `slpi.mbn` | 7,633,828 B / md5 见上 | pMOS APK |

## 系统集成文件

| 项 | 值 |
|---|---|
| systemd | slpi-sensors.service、slpi-fw-guard.service、sensor-boot-report.service/.timer、iio drop-in |
| udev | 80-iio-sensor-proxy.rules、81-libssc-xiaomi-elish.rules、99-ssc-accel.rules |
| sleep hooks | 60-sensors-suspend.sh、40-cancel-scheduled-shutdown.sh |
| 调参 | `/etc/sensor-tuning.conf`：`PRE_DELAY=0`、`IIO_DELAY=8`（0s 经 8/8 验证） |
| D-Bus | `net.hadess.SensorProxy.service`（含 `SystemdService=`）、`.conf`、`.policy` |
