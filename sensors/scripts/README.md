# 运行时脚本（安装到 /usr/local/sbin）

| 脚本 | 调用方 | 作用 |
|---|---|---|
| `slpi-sensors-start.sh` | `slpi-sensors.service` ExecStart | 维护固件符号链接不变量；监听器先行→放固件到 /run→启动 SLPI；等设备；iio warm restart 兜底；真实健康检查（gdbus + registry）；写开机报告；检测到本 boot 已被内核 auto_boot 时快速失败 |
| `slpi-sensors-stop.sh` | `slpi-sensors.service` ExecStop | 停监听器/hexagonrpcd；只删 `/run` 固件副本；保持内核路径为悬空符号链接 |
| `hexrpc-sdsp-service.sh` | start 脚本 setsid 启动 | 循环：从母本生成 `/run/sensors-rootsvc` → 等 `/dev/fastrpc-sdsp` → 以 `HEXAGONRPC_WARM_ATTACH=1` 运行 hexagonrpcd；设备消失则重建根并重连 |
| `slpi-fw-guard.sh` | `slpi-fw-guard.service`（multi-user 前） | 若内核固件路径被残留成普通文件→移出并恢复符号链接；若 dmesg 显示本 boot 内核已加载固件→写 `/run/sensor-boot-bad` |
| `sensor-status.sh` | 手动 / 其他脚本 | 只读诊断：固件路径、AUTO_BOOT、PDM、attach registry 计数、iio、gdbus 数值 |
| `sensor-boot-report.sh` | `sensor-boot-report.service`（boot+5min timer） | 写 `/var/log/sensor-boot-report.txt` 与 history（服务/NRestarts/固件不变量/dmesg/采样/OK-FAILED） |

## 环境变量

- `HEXAGONRPC_WARM_ATTACH=1`：监听器脚本设置，启用 hexrpc 的 warm open/attach 重试。
- 配置来自 `/etc/sensor-tuning.conf`：`PRE_DELAY`（默认 0）、`IIO_DELAY`（默认 8）。
- 开关：存在 `/etc/disable-sensors` 时启动脚本直接退出（系统不受影响）。

## 日志

- `/var/log/hexrpc-boot.log`：启动/attach/健康检查主日志（所有 `[start]`/`[guard]`/`[stop]` 行）。
- `/var/log/sensor-last-boot.txt`：最近一次启动的 OK/FAILED 摘要。
- `/var/log/sensor-boot-report.txt`、`/var/log/sensor-boot-report-history.txt`：开机报告。
- `/run/sensor-boot-bad`：本 boot auto_boot 标记（tmpfs，重启消失）。

## 注意

- 脚本中不再包含任何 `/home/<user>` 硬编码（可移植）。
- 修改脚本后：`sudo install -m 0755 scripts/xxx.sh /usr/local/sbin/`，必要时 `systemctl daemon-reload`。
