# 安装文档（完整步骤）

目标设备：Xiaomi Mi Pad 5 Pro（SM8250，DT `xiaomi elish`），Armbian noble，kernel `6.18.44-current-sm8250`。
本包只做传感器栈；不含固件/厂商数据/编译产物（按本文件提取与编译）。

> 先读 `README.md` 的"红线"与 `NOTES.md` 的已知问题。所有命令除注明外都需要 root（或 sudo）。

---

## 0. 安全前置与备份

1. **不要写 Android 启动槽**；只允许操作 rootfs 与 **Linux 启动槽**。槽位以
   `/boot/armbianEnv.txt` 的 `abl_boot_partition_label` 为准（本机 `boot_b`；别人的安装可能是 `boot_a`）；
   本包脚本自动跟随，不写死。
2. 备份 Linux 启动槽与你当前的 DTB/boot 镜像（槽位自动检测）：
   ```bash
   sudo ../install/backup-boot.sh
   ```
3. 想临时禁用传感器栈：`sudo touch /etc/disable-sensors`（安装后生效，启动脚本会直接退出）。
4. 本安装**会重启一次**；确保你没有其它未保存工作。

---

## 1. 依赖

```bash
sudo apt update
sudo apt install -y build-essential meson ninja-build pkg-config git e2fsprogs \
  libglib2.0-dev libgudev-1.0-dev libpolkit-gobject-1-dev \
  libqmi-glib-dev libqrtr-glib-dev libprotobuf-c-dev protobuf-c-compiler
# 若用稀疏 vendor 镜像提取： sudo apt install -y android-sdk-libsparse-utils
```

内核源码树：需要与运行内核**同版本**的 `6.18.44-current-sm8250` 源码（本机在 `/var/tmp/kbuild-src`；
也可用 `apt-get source linux` 或 Armbian 源码 + 对应补丁）。版本不一致时模块无法加载。

---

## 2. 提取设备数据与固件（本包不附带）

需要三类数据，全部来自你自己的设备/官方发行包：

| 数据 | 内容 | 来源 | 提取脚本 |
|---|---|---|---|
| armbian-firmware 数据 | `sensors/registry`(155)、`sensors/config`(40)、`sensors/` 根文件、`socinfo`(6) | 已安装的 `armbian-firmware-full` 包（Armbian 从 Xiaomi vendor 镜像抽取） | `extract/extract-from-armbian-firmware.sh` |
| Android vendor 数据 | `sensors/proto`(79)、`hals.conf`、`calibratedCheck.json`、`judge_calibrated.json`、`sensor_diag.cfg`、`sns_reg_config`，以及可选 `acdb/` | 你自己设备的 Android vendor 镜像（`/etc/sensors`、`/etc/acdbdata`） | `extract/extract-from-android-vendor-img.sh` |
| SLPI 固件 | 单文件 `slpi.mbn`（7,633,828 B，md5 `51c3b3bef20a76d386a516dfbcc72b68`） | pMOS `firmware-xiaomi-elish-slpi-20240917-r3.apk` | `extract/extract-pmos-slpi-mbn.sh` |

### 2.1 armbian-firmware 数据

系统通常已安装 `armbian-firmware-full`（`dpkg -S /lib/firmware/qcom/sm8250/xiaomi/elish` 可确认）：

```bash
./extract/extract-from-armbian-firmware.sh ./extracted/armbian
# 若系统没装： sudo apt install armbian-firmware-full  或
#   apt-get download armbian-firmware-full && dpkg-deb -x *.deb ./fw
#   ARMBIAN_FW_ROOT=./fw ./extract/extract-from-armbian-firmware.sh ./extracted/armbian
```

得到：`extracted/armbian/sensors/`（registry/config/根文件）与 `socinfo/`。

### 2.2 Android vendor 数据

**方式 A（推荐，若你有 vendor 镜像）**：

```bash
./extract/extract-from-android-vendor-img.sh /path/to/android-vendor.img ./extracted/android
```

**方式 B（从 Android adb 提取，需 root）**：

```bash
adb root
adb pull /vendor/etc/sensors  ./extracted/android/sensors
adb pull /vendor/etc/acdbdata ./extracted/android/acdb   # 可选
```

得到：`extracted/android/sensors/{config,proto,scripts,hals.conf,calibratedCheck.json,judge_calibrated.json,sensor_diag.cfg,sns_reg_config}`（`registry` 不在这里）。

### 2.3 SLPI 固件（pMOS APK）

```bash
./extract/extract-pmos-slpi-mbn.sh /path/to/firmware-xiaomi-elish-slpi-*.apk ./extracted/pmos
# 校验 md5 应为 51c3b3bef20a76d386a516dfbcc72b68
```

> 备选：若没有 pMOS 包，`armbian-firmware-full` 自带拆份固件（`slpi.mbn` + `slpi.b00..b20`）。
> 它与单文件固件同源但**本项目只验证过单文件形式**；如果坚持用拆份形式，需要自行验证（不推荐）。

### 2.4 组装数据母本 + 固件路径不变量

```bash
sudo ./install/install-data.sh \
  --android ./extracted/android \
  --armbian ./extracted/armbian \
  --firmware ./extracted/pmos
```

该脚本会：
- 生成只读母本 `/usr/share/qcom-ssc-elish/{sensors,socinfo,acdb?,slpi.mbn}`；
- 把 `/lib/firmware/qcom/sm8250/xiaomi/elish/` 里可能存在的拆份固件移到 `slpi-disabled/`、`slpi.mbn.disabled`；
- 创建符号链接 `/lib/firmware/qcom/sm8250/xiaomi/elish/slpi.mbn -> /run/sensor-fw/slpi.mbn`
  （**这是防内核开机 auto_boot 的关键不变量**）。

校验：registry 155、config 40、proto 79、socinfo 6、slpi.mbn 7,633,828 B。

---

## 3. 编译与安装组件（顺序：内核 → hexrpc → libssc → iio）

> **合并包用户注意**：统一内核（fastrpc + 指纹 QSEECOM/goodix + LPASS + 联合 DTB + initramfs + boot 镜像）
> 请先按 `../kernel/README.md` 一次完成。本节仅说明 fastrpc 模块的补丁来源与单独验证方式。
>
> **一键安装器已自动完成 3.2–3.4 的补丁+编译+安装**（`../install/build-userspace.sh`），
> 以及数据提取与系统集成；下面的命令是手动等价步骤，用于排障/自定义。

### 3.1 内核 fastrpc.ko（两处补丁）

```bash
cd <内核源码树>
patch -p1 < ../kernel/patches/0001-fastrpc-sdsp-vmid-and-sensors-pd.patch
make ARCH=arm64 -j$(nproc) drivers/misc/fastrpc.ko

KVER=$(uname -r)
sudo cp -a /lib/modules/$KVER/kernel/drivers/misc/fastrpc.ko /root/fastrpc.ko.bak-$(date +%s)
sudo cp drivers/misc/fastrpc.ko /lib/modules/$KVER/kernel/drivers/misc/fastrpc.ko
sudo depmod -a
```

- 补丁基于 pristine v6.18.44，包含：SDSP VMID 候选（`{3,5}` 优先）+ 官方 SMMU sensors-PD 修复。
- 细节与验证见 `../kernel/README.md`。
- **注意**：如果你使用本机 `/var/tmp/kbuild-src` 这棵源码树，它里面 `qcom_q6v5_pas.c` 曾被实验性改成
  `auto_boot=false`（已安装模块是原厂）。只编译 `fastrpc.ko` 不受影响；但如果你要 `make modules_install`
  全量安装，请先把 `qcom_q6v5_pas.c` 恢复为 upstream（`auto_boot=true`），否则 SLPI 不会开机自启，
  传感器栈会等不到系统服务启动 SLPI（或需要手动 start）。
- DTB 的联合修改见 `../kernel/dtb/README.md`；先只装模块，若 dmesg 出现
  `SDSP assign try [0]: 0` 即成功，无需改 DTB。

### 3.2 hexagonrpcd（hexagonrpc v0.3.2 + warm-attach 补丁）

```bash
git clone https://github.com/linux-msm/hexagonrpc
cd hexagonrpc && git checkout v0.3.2
git apply /path/to/userspace/hexrpc/patches/0001-hexrpc-0.3.2-warm-attach.patch
meson setup build --prefix=/opt/hexrpc-032b
ninja -C build
sudo ninja -C build install
```

详见 `userspace/hexrpc/README.md`。安装后二进制在 `/opt/hexrpc-032b/bin/hexagonrpcd`，
库在 `/opt/hexrpc-032b/lib/aarch64-linux-gnu/`。

### 3.3 libssc 0.4.4

```bash
git clone https://codeberg.org/mmcallister/libssc
cd libssc && git checkout <0.4.4 对应 tag/commit>   # 本机验证: v0.4.4-9-g4e4c6c8
meson setup build --prefix=/usr/local -Dintrospection=false
ninja -C build
sudo ninja -C build install
sudo ldconfig
pkg-config --modversion libssc   # 0.4.4
```

详见 `userspace/libssc/README.md`。

### 3.4 iio-sensor-proxy（含三处补丁）

```bash
git clone https://gitlab.freedesktop.org/hadess/iio-sensor-proxy
cd iio-sensor-proxy && git checkout ca0d5b0
git apply /path/to/userspace/iio-sensor-proxy/patches/0001-iio-sensor-proxy-hotplug-stayalive.patch
meson setup build --prefix=/usr -Dssc-support=enabled
ninja -C build
sudo ninja -C build install
```

详见 `userspace/iio-sensor-proxy/README.md`。

---

## 4. 安装系统集成文件

```bash
sudo ./install/install-system-files.sh
```

会安装：运行时脚本（`/usr/local/sbin`）、systemd 单元（`slpi-sensors`、`slpi-fw-guard`、`sensor-boot-report`）、
iio drop-in（`Restart=on-failure`）、udev 规则、休眠钩子、`/etc/sensor-tuning.conf`（`PRE_DELAY=0`/`IIO_DELAY=8`）、
D-Bus/polkit 配置；并 enable `slpi-fw-guard`、`slpi-sensors`、`sensor-boot-report.timer`。
文件落位见 `install/FILES-MAP.md`。

> **不要 enable `iio-sensor-proxy`**：它由 GNOME 通过 D-Bus 激活；且它的 D-Bus service 文件必须包含
> `SystemdService=iio-sensor-proxy.service`（本包已提供），否则会出现重启风暴。

---

## 5. 第一次启动与验证

```bash
sudo reboot
```

开机后约 22 秒（M2 可写 registry 后；此前 ~79s）：

```bash
# 1) 内核层
sudo dmesg | grep -E 'Direct firmware load.*slpi|Booting fw image.*slpi|SDSP assign|PDM'
#   期望：开机早期只有 "Direct firmware load ... failed"（=内核没抢先启动 SLPI）；
#         服务启动时出现 "Booting fw image" + "SDSP assign try [0]: 0"；无 PDM 崩溃。
# 2) 服务/数据
sudo systemctl status slpi-sensors iio-sensor-proxy --no-pager
sudo tail -30 /var/log/hexrpc-boot.log        # attach 段应有大量 sensors/registry 访问
# 3) 传感器
./install/verify.sh      # 或 sudo /usr/local/sbin/sensor-status.sh
gdbus call --system --dest net.hadess.SensorProxy --object-path /net/hadess/SensorProxy \
  --method org.freedesktop.DBus.Properties.GetAll net.hadess.SensorProxy
monitor-sensor
```

标准成功判据：`HasAccelerometer=true`、`HasAmbientLight=true`、`LightLevel` 有数值、
`AccelerometerOrientation` 非 `undefined`、iio `NRestarts=0` 且单实例。

---

## 6. 日常使用

- 禁用：`sudo touch /etc/disable-sensors && sudo reboot`（整条链路不启动，系统完全正常）。
- 重新启用：`sudo rm /etc/disable-sensors && sudo reboot`。
- 调参：`/etc/sensor-tuning.conf` 中 `PRE_DELAY`、`IIO_DELAY`（秒）。`PRE_DELAY=0` 已通过 8/8 验证；
  若遇到启动不稳定可改回 `10` 或 `45`（见 `NOTES.md`）。
- 诊断：`sensor-status`（或 `/usr/local/sbin/sensor-status.sh`）；
  开机报告：`/var/log/sensor-boot-report.txt`、`/var/log/sensor-last-boot.txt`。
- 升级注意（**详细影响评估见 `docs/UPGRADE-IMPACT.md`**）：
  - **升级 `armbian-firmware-full` 后**：固件路径可能被恢复成普通文件/拆件 → 立即检查
    `ls -l /lib/firmware/qcom/sm8250/xiaomi/elish/slpi.mbn` 是否为符号链接并指向 `/run/sensor-fw/slpi.mbn`；
    不是就手动恢复（或重启让 `slpi-fw-guard` 修正，但该次开机可能已 auto_boot，需再重启一次）。
  - **升级内核（版本变化）**：必须重编 `fastrpc.ko` 与 iio-sensor-proxy（如 ABI 变化），并更新 unit 依赖。
  - 重新安装 `armbian-firmware-full`/内核后，建议直接重启并运行 `verify.sh`。

---

## 7. 卸载 / 回滚

```bash
# 停用并移除系统集成（保留数据与已编译组件）
sudo ./install/uninstall.sh
# 连数据一起删
sudo ./install/uninstall.sh --purge-data
```

彻底回滚还需要（按需）：
- 恢复原厂 `fastrpc.ko`（安装时已备份到 `/root/fastrpc.ko.bak-*`）；
- 卸载/覆盖 iio-sensor-proxy、libssc、/opt/hexrpc-032b；
- 如改过 DTB/boot 镜像，使用 `/root` 或 `/boot` 的备份恢复（Linux 启动槽，按 armbianEnv 实际槽位）。

详见 `docs/TROUBLESHOOTING.md` 与 `NOTES.md`。
