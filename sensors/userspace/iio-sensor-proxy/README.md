# iio-sensor-proxy（含三处必须的修改）

## 版本

- 上游：https://gitlab.freedesktop.org/hadess/iio-sensor-proxy
- 本机基线 commit：`ca0d5b0`（版本 3.9，master）
- 补丁：`patches/0001-iio-sensor-proxy-claim-stayalive-hotplug.patch`（对 `src/iio-sensor-proxy.c`）

## 三处修改（缺一不可）

### 1) SSC claim 竞态：设备注册后对已有 claim 启动 polling（上游修复）

GNOME/gsd 的 `ClaimLight` / `ClaimAccelerometer` 常常在 SSC 驱动设备注册**之前**到达：
claim 只被记入 clients 表，没有启动 polling；设备随后注册时，因为 clients>1 走了"已活动"分支，
不会再 `driver_set_polling(TRUE)` → 收不到第一帧测量值 → `LightLevel=0`、orientation=undefined。

修复：在**启动枚举**与**udev 热插**两条设备注册路径中，注册完成后检查 clients 表，
若已有 claim 则 `driver_set_polling(sensor_device, TRUE)`。

### 2) 常驻等待热插：启动时没有传感器不再退出

iio 被 GNOME 通过 D-Bus 激活的时机早于 SSC 设备出现。上游在启动时找不到传感器会
`g_message("No sensors...")` 后 `g_main_loop_quit()`，配合 systemd `Restart=always`
会形成"无限重启风暴"（NRestarts 涨到 20+），期间 GNOME 的 claim 反复失败。

修复：启动时没有传感器 → 打印 `No sensors found at startup; staying alive waiting for hotplug (patched)`
并继续运行；设备被 PD 崩溃移除时同样不退出（`all sensors removed; staying alive...`）。

### 3) 无条件连接 udev 热插：uevent 回调必须在 find_sensors 之前注册

上游只有在 `find_sensors()` 成功时才 `g_signal_connect(client, "uevent", ...)`。
配合修改 2（启动时无传感器仍常驻），必须把 uevent 订阅提前到 `find_sensors()` 之前，
并初始化 `sensor_startup_dbus_invocations_delayed[]`，否则设备热插后永远不会被注册
（本次实测：热插失败 → 启动脚本 warm-restart iio 兜底）。

## 编译

依赖（Ubuntu 24.04）：`build-essential meson ninja-build pkg-config libglib2.0-dev
libgudev-1.0-dev libpolkit-gobject-1-dev libssl-dev`，外加本项目自行安装的 libssc（见 `../libssc/README.md`）。

```bash
git clone https://gitlab.freedesktop.org/hadess/iio-sensor-proxy
cd iio-sensor-proxy
git checkout ca0d5b0
git apply /path/to/patches/0001-iio-sensor-proxy-claim-stayalive-hotplug.patch

meson setup build --prefix=/usr -Dssc-support=enabled
ninja -C build
sudo ninja -C build install
```

`ninja install` 会安装：`/usr/libexec/iio-sensor-proxy`、systemd unit、D-Bus policy、
polkit policy 等。

## 本包额外提供的 systemd/D-Bus 配置（必须）

- `systemd/iio-dropin/after-ssc.conf` → `/etc/systemd/system/iio-sensor-proxy.service.d/after-ssc.conf`
  ```
  [Unit]
  After=multi-user.target
  [Service]
  ExecStartPre=/bin/sleep 2
  Restart=on-failure
  RestartSec=5
  ```
  注意：**不能用 `Restart=always`**（上游 clean exit 会触发风暴）；用 `Restart=on-failure`。
- `systemd/dbus/net.hadess.SensorProxy.service` 必须包含
  `SystemdService=iio-sensor-proxy.service`，否则 dbus-daemon 会直接 exec iio
  （进程落在 dbus.service cgroup，systemd 单元永远管不到它，形成重启循环）。

## 验证

```bash
systemctl show iio-sensor-proxy -p ActiveState -p NRestarts -p MainPID
# 期望 active、NRestarts=0、单实例，进程 cgroup 在 /system.slice/iio-sensor-proxy.service
gdbus call --system --dest net.hadess.SensorProxy --object-path /net/hadess/SensorProxy \
  --method org.freedesktop.DBus.Properties.GetAll net.hadess.SensorProxy
# 期望 HasAmbientLight true、LightLevel 有数值、AccelerometerOrientation 非 undefined
```
