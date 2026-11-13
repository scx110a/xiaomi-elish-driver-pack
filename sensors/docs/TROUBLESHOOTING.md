# 故障排查（按现象定位）

先跑：

```bash
sensor-status                       # 或 /usr/local/sbin/sensor-status.sh
sudo dmesg | grep -E 'PDM|SDSP assign|Direct firmware load|Booting fw image.*slpi'
sudo tail -80 /var/log/hexrpc-boot.log
sudo systemctl status slpi-sensors iio-sensor-proxy --no-pager
sudo journalctl -b -u iio-sensor-proxy --no-pager | tail -30
sudo cat /var/log/sensor-boot-report.txt   # 若已生成
```

## 1. 开机后传感器一直不出来（最常见）

按下表判断属于哪类：

| 现象（dmesg / 日志） | 含义 | 处理 |
|---|---|---|
| 开机早期（<30s）出现 `Booting fw image .../slpi.mbn`；随后 `PDM: service 'sensor_process' crash: ... dog_virtual_user`；`SDSP assign try [*]: -22`；hexrpc attach 只读 `hw_platform` | **固件泄漏导致内核 auto_boot**（监听器来不及就绪） | 确认 `/lib/firmware/qcom/sm8250/xiaomi/elish/slpi.mbn` 是悬空符号链接；`sudo rm -f` 掉普通文件后 **干净重启**；检查是谁覆盖了它（armbian-firmware 升级/手工 cp） |
| 无 PDM 崩溃、无 assign -22；attach 只读 `hw_platform` 后停；iio `'registry' sensor timed out`；start 日志 `sensors FAILED (timeout)` | **偶发 PD 静默卡死**（约 1/14，与延迟无关） | `sudo reboot`；同 boot 内重启服务无效 |
| `all SDSP assign attempts failed (-22)` 且 SLPI 之前已跑过（单次 boot 内 stop/start 过） | SCM assign 同一 boot 不可重复 | 干净重启；不要在 boot 内 stop/start SLPI 栈 |
| `SDSP assign try [0]: 0` 但 attach 段只有 `hw_platform`、registry 无 | hexagonrpcd attach 窗口未命中/PD 未就绪 | 确认 hexrpc 是 0.3.2 + warm-attach 补丁；`HEXAGONRPC_WARM_ATTACH=1`；看 `/var/log/hexrpc-boot.log` 是否有 `SLPI start issued; device=yes` |
| `Direct firmware load ... failed` 之后服务阶段仍无 `Booting fw image` | SLPI 没被启动（服务未运行/失败） | `systemctl status slpi-sensors`；检查 `/etc/disable-sensors` 是否存在；`journalctl -b -u slpi-sensors` |

## 2. iio-sensor-proxy 异常

| 现象 | 原因 | 处理 |
|---|---|---|
| `NRestarts` 持续增长、`systemctl status` 一直 activating、`dbus.service` cgroup 里有 iio | D-Bus service 文件缺 `SystemdService=`，或 drop-in 用了 `Restart=always` | 确认 `/usr/share/dbus-1/system-services/net.hadess.SensorProxy.service` 含 `SystemdService=iio-sensor-proxy.service`；drop-in 为 `Restart=on-failure`；`systemctl daemon-reload && systemctl reload dbus`；`systemctl restart iio-sensor-proxy` |
| iio active 且单实例，但 `LightLevel=0` / `orientation=undefined` | GNOME claim 早于设备注册（claim→polling 竞态） | 确认 iio 二进制带补丁（`strings /usr/libexec/iio-sensor-proxy | grep patched` 或看启动日志 "staying alive"）；`sudo systemctl restart iio-sensor-proxy`；`journalctl -b | grep Claim` 看是否有 AccessDenied/NoReply |
| `journalctl -u iio-sensor-proxy` 有 `No sensors found at startup; staying alive waiting for hotplug (patched)`，随后设备出现但无传感器 | 热插注册失败（PD 未就绪） | 正常会由启动脚本 warm restart 兜底；若没有，手动 `systemctl restart iio-sensor-proxy`；若仍无，看 `dmesg` PD 状态 |

## 3. 传感器数据出现但方向/亮度不对

- 自动旋转方向：修改 `/etc/udev/rules.d/81-libssc-xiaomi-elish.rules` 的 `ACCEL_MOUNT_MATRIX`，
  然后 `sudo udevadm control --reload-rules && sudo udevadm trigger --subsystem-match=misc` 并重启 iio。
- 自动亮度：检查 `HasAmbientLight` 与 `LightLevel` 是否变化（遮光测试）；`monitor-sensor`。
- 陀螺/proximity：本包未启用（见 NOTES.md）。

## 4. 休眠/唤醒后异常

- 唤醒后 PD 可能报 `frpc_dsp` 崩溃；通常监听器重连 + iio 重启能恢复。
- 若唤醒后锁屏无法解锁：先确认不是"定时关机与 suspend 撞车"（检查 `/var/lib/suspend-watch.log`
  是否 `pending scheduled shutdown canceled`）。已有 `40-cancel-scheduled-shutdown.sh` 防御；
  如已发生，长按电源强制重启（或 TTY 登录 `sudo systemctl restart gdm`）。
- 唤醒后传感器没了：`sudo reboot`（同 boot 不可重试）。

## 5. 升级后失效

| 升级 | 影响 | 修复 |
|---|---|---|
| 内核版本变化 | fastrpc.ko/iio ABI 不匹配 | 对同版本源码重打补丁、重编 fastrpc.ko；必要时重编 iio |
| armbian-firmware-full | 固件路径可能变普通文件/拆份固件回来 | 检查路径并恢复悬空符号链接；重启（守卫会兜底但该 boot 可能已坏） |
| iio-sensor-proxy 包 | 覆盖自定义二进制/配置 | 重新 `ninja -C build install`，恢复 D-Bus service 与 drop-in |
| libssc 包 | iio 链接失败/版本不匹配 | 重装 0.4.4，`ldconfig`，重启 iio |

## 6. 一键禁用/恢复

```bash
sudo touch /etc/disable-sensors && sudo reboot     # 保系统正常
sudo rm /etc/disable-sensors && sudo reboot        # 恢复
# 立即停止（无需重启）
sudo systemctl stop slpi-sensors iio-sensor-proxy 2>/dev/null
sudo rm -f /lib/firmware/qcom/sm8250/xiaomi/elish/slpi.mbn
```

## 7. 采集证据

```bash
sudo dmesg > /tmp/dmesg.txt
sudo journalctl -b -u slpi-sensors -u iio-sensor-proxy --no-pager > /tmp/services.txt
sudo cp /var/log/hexrpc-boot.log /tmp/
sensor-status > /tmp/status.txt
```

（附：历史完整调试记录已移出包，归档于工作区。）
