# 用户态组件（编译顺序）

本包只含补丁与说明，不含编译产物。按以下顺序编译安装：

| 顺序 | 组件 | 作用 | 说明 |
|---|---|---|---|
| 1 | 内核 `fastrpc.ko`（`../../kernel/`） | 让 `/dev/fastrpc-sdsp` 出现并正确分配 VMID/remote heap | 必须先做，否则用户态无从 attach |
| 2 | hexagonrpcd 0.3.2（`hexrpc/`） | 在 /dev/fastrpc-sdsp 上提供 hexagonfs 文件服务（PD 读 registry/配置，M2 起也允许写回 sensor persist） | 必须带 warm-attach（+ M2 可写 registry）补丁，装到 `/opt/hexrpc-032b` |
| 3 | libssc 0.4.4（`libssc/`） | 通过 QRTR/QMI 连接 SSC 并枚举传感器 | iio 的 `-Dssc-support=enabled` 依赖它 |
| 4 | iio-sensor-proxy（`iio-sensor-proxy/`） | 把 SSC 传感器暴露为 D-Bus SensorProxy | 必须带三处补丁 + 正确的 systemd/D-Bus 配置 |

## 运行时链路

```
/dev/fastrpc-sdsp
  ← hexagonrpcd 0.3.2（-f /dev/fastrpc-sdsp -d sdsp -s -R /run/sensors-rootsvc）
  ← libssc（QRTR/QMI service 0x190，node 9）
  ← iio-sensor-proxy（D-Bus net.hadess.SensorProxy）
  ← GNOME（gsd-power / gnome-shell claim）
```

## 版本与路径

见 `../VERSIONS.md`。运行路径：

- `/opt/hexrpc-032b/bin/hexagonrpcd`（默认优先 `hexagonrpcd.writable-v1` → `hexagonrpcd.regver-v1` → 原厂；`LD_LIBRARY_PATH=/opt/hexrpc-032b/lib/aarch64-linux-gnu`）
- `/usr/local/lib/aarch64-linux-gnu/libssc.so.2`
- `/usr/libexec/iio-sensor-proxy`
- `/run/sensors-rootsvc`：每次启动从 `/usr/share/qcom-ssc-elish` 复制的干净 hexagonfs 根（PD 可写回，不污染母本）
- `/run/sensor-fw/slpi.mbn`：运行时固件副本（tmpfs）
