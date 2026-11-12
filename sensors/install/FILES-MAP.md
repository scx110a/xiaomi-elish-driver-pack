# 文件落位对照表（archive → 系统）

`install/install-system-files.sh` 会按此表安装。安装前可对照检查。

| 归档内文件 | 系统目标路径 | 权限 | 作用 |
|---|---|---|---|
| `scripts/slpi-sensors-start.sh` | `/usr/local/sbin/slpi-sensors-start.sh` | 0755 | 启动主逻辑（`slpi-sensors.service` ExecStart） |
| `scripts/slpi-sensors-stop.sh` | `/usr/local/sbin/slpi-sensors-stop.sh` | 0755 | ExecStop |
| `scripts/hexrpc-sdsp-service.sh` | `/usr/local/sbin/hexrpc-sdsp-service.sh` | 0755 | warm 监听器循环（优先 writable-v1 → regver-v1 → 原厂） |
| `scripts/sensor-timing-diag.sh` | `/usr/local/sbin/sensor-timing-diag.sh` | 0755 | 一次性启动时序诊断 |
| `scripts/slpi-fw-guard.sh` | `/usr/local/sbin/slpi-fw-guard.sh` | 0755 | 开机固件路径守卫 |
| `scripts/sensor-status.sh` | `/usr/local/sbin/sensor-status.sh`（+ `/usr/local/bin/sensor-status` 软链） | 0755 | 诊断 |
| `scripts/sensor-boot-report.sh` | `/usr/local/sbin/sensor-boot-report.sh` | 0755 | 开机报告 |
| `systemd/slpi-sensors.service` | `/etc/systemd/system/slpi-sensors.service` | 0644 | 主服务（enabled） |
| `systemd/slpi-fw-guard.service` | `/etc/systemd/system/slpi-fw-guard.service` | 0644 | 守卫（enabled，Before=multi-user） |
| `systemd/sensor-boot-report.service` | `/etc/systemd/system/sensor-boot-report.service` | 0644 | 开机报告（由 timer 触发） |
| `systemd/sensor-boot-report.timer` | `/etc/systemd/system/sensor-boot-report.timer` | 0644 | `OnBootSec=5min`（enabled→timers.target） |
| `systemd/iio-dropin/after-ssc.conf` | `/etc/systemd/system/iio-sensor-proxy.service.d/after-ssc.conf` | 0644 | iio drop-in（`Restart=on-failure`） |
| `systemd/udev/80-iio-sensor-proxy.rules` | `/etc/udev/rules.d/80-iio-sensor-proxy.rules` | 0644 | 上游 SSC 标签规则 |
| `systemd/udev/81-libssc-xiaomi-elish.rules` | `/etc/udev/rules.d/81-libssc-xiaomi-elish.rules` | 0644 | 加速度计挂载矩阵 |
| `systemd/udev/99-ssc-accel.rules` | `/etc/udev/rules.d/99-ssc-accel.rules` | 0644 | 启用 ssc-accel |
| `systemd/system-sleep/60-sensors-suspend.sh` | `/usr/lib/systemd/system-sleep/60-sensors-suspend.sh` | 0755 | 休眠钩子 |
| `systemd/system-sleep/40-cancel-scheduled-shutdown.sh` | `/usr/lib/systemd/system-sleep/40-cancel-scheduled-shutdown.sh` | 0755 | 防 shutdown/suspend 撞车 |
| `systemd/sensor-tuning.conf` | `/etc/sensor-tuning.conf` | 0644 | 调参（PRE_DELAY/IIO_DELAY） |
| `systemd/dbus/net.hadess.SensorProxy.service` | `/usr/share/dbus-1/system-services/net.hadess.SensorProxy.service` | 0644 | **必须含 SystemdService=** |
| `systemd/dbus/net.hadess.SensorProxy.conf` | `/usr/share/dbus-1/system.d/net.hadess.SensorProxy.conf` | 0644 | D-Bus policy |
| `systemd/dbus/net.hadess.SensorProxy.policy` | `/usr/share/polkit-1/actions/net.hadess.SensorProxy.policy` | 0644 | polkit |

## 数据与固件路径（`install/install-data.sh`）

| 目标 | 说明 |
|---|---|
| `/usr/share/qcom-ssc-elish/sensors/` | registry/config/proto/根文件（只读母本） |
| `/usr/share/qcom-ssc-elish/socinfo/` | 6 个 socinfo 文件 |
| `/usr/share/qcom-ssc-elish/acdb/` | 可选（存在时 hexagonfs 根会带上） |
| `/usr/share/qcom-ssc-elish/slpi.mbn` | 单文件固件（7,633,828 B，md5 校验） |
| `/lib/firmware/qcom/sm8250/xiaomi/elish/slpi.mbn` | **符号链接** → `/run/sensor-fw/slpi.mbn`（不变量） |
| `/lib/firmware/qcom/sm8250/xiaomi/elish/slpi-disabled/`、`slpi.mbn.disabled` | 原拆份固件被移到这里，防止内核开机 auto_boot |
| `/run/sensor-fw/` | tmpfs，运行时由启动脚本写入 `slpi.mbn` |

## 不由本包脚本安装（需按 INSTALL.md 编译安装）

| 组件 | 目标路径 |
|---|---|
| fastrpc.ko | `/lib/modules/<kver>/kernel/drivers/misc/fastrpc.ko`（+ `depmod`） |
| hexagonrpcd | `/opt/hexrpc-032b/bin/hexagonrpcd` + `.../lib/aarch64-linux-gnu/libhexagonrpc.so` |
| libssc | `/usr/local/lib/aarch64-linux-gnu/libssc.so.2` 等 |
| iio-sensor-proxy | `/usr/libexec/iio-sensor-proxy`（及上游 meson 安装的 unit/policy） |
