# iio-sensor-proxy 修复说明（三处补丁 + 配置）

## 背景

SSC 传感器（libssc + iio-sensor-proxy）在 GNOME 下有两个典型故障：

1. **数据不上报**：`LightLevel=0`、`AccelerometerOrientation=undefined`，但 SSC 侧传感器其实已创建。
   根因：GNOME/gsd 的 `ClaimLight`/`ClaimAccelerometer` 在 SSC 设备**注册之前**到达；
   iio 只把 claim 记入 clients 表，没有启动 polling；设备注册后因 clients>1 走"已活动"分支，
   不再 `driver_set_polling(TRUE)` → 收不到第一帧测量。
2. **iio 重启风暴 / 热插不注册**：iio 被 GNOME 通过 D-Bus 提前激活时 SSC 设备还没出现。
   上游在找不到传感器时直接 `g_main_loop_quit()` 退出，配合 `Restart=always` 形成无限重启；
   且上游只有 `find_sensors()` 成功时才 `g_signal_connect(client,"uevent",...)`，
   于是"启动时无设备、之后热插"的路径永远不会注册传感器。

## 修复（`patches/0001-iio-sensor-proxy-hotplug-stayalive.patch`）

基于上游 `ca0d5b0`（3.9-1），修改 `src/iio-sensor-proxy.c`：

1. **claim→polling**：`name_acquired_handler()` 启动枚举、`sensor_changes()` 热插 add
   两条设备注册路径，在 `DEVICE_FOR_TYPE(i)=sensor_device` 后检查 `clients[i]`，
   非空则 `driver_set_polling(sensor_device, TRUE)`。
2. **常驻**：
   - `name_acquired_handler()` 的 `bail:` 分支去掉 `g_main_loop_quit()`，改为
     `g_message("No sensors found at startup; staying alive waiting for hotplug (patched)")` + return；
   - `sensor_changes()` 的 remove 分支在 `!any_sensors_left()` 时不再退出，
     打印 `all sensors removed; staying alive waiting for hotplug (patched)`。
3. **无条件 uevent**：把 `sensor_startup_dbus_invocations_delayed[]` 初始化与
   `g_signal_connect(...,"uevent",sensor_changes,...)` 提前到 `find_sensors()` 之前，
   保证启动时无传感器也能处理后续热插（并避免 delayed 数组未初始化的空指针风险）。

## 必须配套的系统配置（不是源码补丁，已随包提供）

1. `/usr/share/dbus-1/system-services/net.hadess.SensorProxy.service` 必须包含
   `SystemdService=iio-sensor-proxy.service`。否则 dbus-daemon 直接 exec iio，
   进程落在 `dbus.service` cgroup，systemd 单元永远管不到 → 重启循环、claim 反复失败。
2. drop-in `/etc/systemd/system/iio-sensor-proxy.service.d/after-ssc.conf`：

   ```ini
   [Unit]
   After=multi-user.target

   [Service]
   ExecStartPre=/bin/sleep 2
   Restart=on-failure
   RestartSec=5
   ```

   **不能用 `Restart=always`**：iio 的"无传感器"路径是 clean exit(0)，always 会导致风暴。

## 验证

```bash
systemctl show iio-sensor-proxy -p ActiveState -p NRestarts -p MainPID
# active / 0 / 单实例，cgroup 在 /system.slice/iio-sensor-proxy.service
journalctl -b -u iio-sensor-proxy | grep -E 'staying alive|Mount matrix|registry'
journalctl -b | grep -E 'Claim.*(fail|AccessDenied|NoReply)'   # 应为空
gdbus call --system --dest net.hadess.SensorProxy --object-path /net/hadess/SensorProxy \
  --method org.freedesktop.DBus.Properties.GetAll net.hadess.SensorProxy
# HasAccelerometer/HasAmbientLight true；LightLevel 有数值；AccelerometerOrientation 非 undefined
```

实测：LightLevel 随遮光变化（如 59.9→26.8 lux），orientation 随转动变化（right-up 等），
`iio NRestarts=0`、无 claim 失败日志。启动脚本的 warm-restart 兜底日志
（`iio has no SSC sensors after device appeared (hotplug missed); doing one warm restart`）
属于已知的正常兜底路径，不影响功能。
