# systemd / udev / D-Bus 配置说明

安装脚本 `install/install-system-files.sh` 会把本目录文件按 `install/FILES-MAP.md` 落位。

## 服务依赖关系

```
slpi-fw-guard.service        （开机早期，After=local-fs.target，Before=multi-user.target）
        ↓
multi-user.target
        ↓
slpi-sensors.service         （oneshot，After=multi-user.target，RemainAfterExit）
        ↓ ExecStart: slpi-sensors-start.sh
        ↓ ExecStop:  slpi-sensors-stop.sh
iio-sensor-proxy.service     （Type=dbus，由 GNOME 通过 D-Bus 激活；静态，不要 enable）
        ↑ drop-in: after-ssc.conf（After=multi-user.target、ExecStartPre=sleep 2、Restart=on-failure）
sensor-boot-report.timer     （OnBootSec=5min，触发 sensor-boot-report.service）
```

## 关键点

- `slpi-sensors.service` 的 ExecStart 约需 1.5–2 分钟（healcheck 在内），是 oneshot；
  不要给它加 `TimeoutStartSec` 限制（默认 oneshot 无限）。
- `slpi-fw-guard.service` 必须在 `multi-user.target` 之前运行（它守护内核固件路径）；
  不要禁用/移动它。
- `iio-sensor-proxy.service` **不要 enable/start**（由 D-Bus 激活），也不要加 `Restart=always`。
- `systemd/dbus/net.hadess.SensorProxy.service` 必须含：
  ```
  SystemdService=iio-sensor-proxy.service
  ```
  否则 dbus-daemon 会直接 exec iio，进程落在 `dbus.service` cgroup，systemd 单元永远管不到，
  形成"无限重启 + claim 失败"的故障。
- udev：
  - `80-iio-sensor-proxy.rules`：上游规则，给 `fastrpc-sdsp` 打 `ssc-light ssc-compass` 标签；
  - `99-ssc-accel.rules`：额外启用 `ssc-accel`（上游默认 disabled）；
  - `81-libssc-xiaomi-elish.rules`：`ACCEL_MOUNT_MATRIX`，修正自动旋转方向（按机型，可按需调整）。
- 休眠钩子（`/usr/lib/systemd/system-sleep/`）：
  - `60-sensors-suspend.sh`：pre 停 iio，post 重拉监听器 + iio；
  - `40-cancel-scheduled-shutdown.sh`：suspend 前取消 pending shutdown（防"半关机+休眠"导致锁屏无法解锁），
    并记录 pre/post 诊断到 `/var/lib/suspend-watch.log`。
- 调参文件 `/etc/sensor-tuning.conf`：`PRE_DELAY=0`、`IIO_DELAY=8`。

## 启用/禁用

```bash
# 正常启用（安装脚本已做）
sudo systemctl enable slpi-fw-guard.service slpi-sensors.service sensor-boot-report.timer
# 临时禁用整条链
sudo touch /etc/disable-sensors && sudo reboot
# 恢复
sudo rm /etc/disable-sensors && sudo reboot
```
