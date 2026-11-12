# 部署记录：传感器稳定化修复（2026-11-11 17:34）

状态：**已安装，等待一次干净重启验证**。

## 时间线

| 时间 | 动作 |
|---|---|
| 17:32 | 首次手动备份 `/root/sensor-fix-backup-20261111-173254`（原始状态） |
| 17:34 | `install.sh` 安装；再次备份 `/root/sensor-fix-backup-20261111-173439`（含 rollback.sh） |
| 17:34:45 | 守卫首次运行：把开机泄漏的普通文件（7,633,828B）移到 `/run/sensor-fw/leaked-slpi-*.mbn`，内核路径改为悬空符号链接，写 auto_boot 标记 |
| 17:34:55 | 测试：脏 boot 下启动脚本 2.4s 快速失败，写 `/home/<user>/sensor-last-boot.txt` |
| 17:35:04 | 测试：模拟"固件被手动放成普通文件"→ 守卫自愈（移走 + 恢复符号链接）；测试 stop 脚本清理卡死监听器 |
| 17:35 | 清理 `/run` 留证文件；`/lib/firmware/.../slpi.mbn` = 悬空符号链接 |
| 17:35 | 交接文档追加第四十节（两份同步）；刷新归档 SHA256SUMS |
| 17:36 | **回滚实测通过**：执行 rollback.sh 后 3 个被改文件 sha256 与原始状态完全一致、固件恢复为普通文件、guard 单元移除；随后重新执行 install.sh（新备份 `...-173609`）恢复修复态 |

## 变更文件

```
/usr/local/sbin/slpi-sensors-start.sh       重写
/usr/local/sbin/slpi-sensors-stop.sh        重写
/usr/local/sbin/slpi-fw-guard.sh            新增
/usr/local/sbin/sensor-status.sh            新增（/usr/local/bin/sensor-status 软链）
/etc/systemd/system/slpi-fw-guard.service   新增（enabled）
/lib/firmware/qcom/sm8250/xiaomi/elish/slpi.mbn   改为符号链接 -> /run/sensor-fw/slpi.mbn
```

未改动：`slpi-sensors.service`、`/etc/sensor-tuning.conf`（PRE_DELAY=45, IIO_DELAY=8）、
`hexrpc-sdsp-service.sh`、内核模块、DTB、boot 分区、Android 侧任何内容。

## 已验证

- `bash -n` 全部通过；`systemd-analyze verify` 通过；unit 排序确认
  `slpi-fw-guard` → `multi-user.target` → `slpi-sensors`。
- 守卫实际运行：移走泄漏固件、恢复符号链接、写 auto_boot 标记、日志正确。
- 守卫自愈：手动把内核路径替换成普通文件后运行守卫 → 移走并恢复符号链接。
- 启动脚本 fail-fast：当前脏 boot 下 2.4s 退出，日志/开机报告正确，未影响运行中的进程。
- 停止脚本：清理卡死监听器，内核路径保持悬空符号链接。
- 当前 `sensor-status`：`kernel-fw-path=symlink ... (dangling: boot-safe)`。

## 未验证（需要重启）

- 干净重启后的完整成功路径：内核开机不 auto_boot → 监听器先行 → SLPI 启动 → attach 读到 registry → 传感器出数据。
- 重启后检查清单：
  ```bash
  cat /home/<user>/sensor-last-boot.txt
  sensor-status
  sudo dmesg | grep -E 'Booting fw image .*slpi|Direct firmware load .*slpi|PDM|SDSP assign'
  grep -c 'sensors/registry' /var/log/hexrpc-boot.log
  ```

## 回滚

```bash
sudo bash /root/sensor-fix-backup-20261111-173439/rollback.sh
```

备份目录同时有用户可读副本：`/home/<user>/sensor-fix-backup-20261111-173439/`。

## 遗留

- suspend/resume PD 崩溃、同 boot assign 不可重复（P1，需实验）。
- 罕见 attach >60ms 超时：现在日志会记录 `device=... after Nms`，可观察。
- initramfs 里仍有旧 slpi.mbn 副本（无害，后续 update-initramfs 会清）。
- `/home/<user>/xiaomi-elish-sensors-v1-final/SHA256SUMS` 因文档更新需要刷新。

---

# 第二轮修复（17:47）：iio 重启风暴 + D-Bus 激活路径

第一次重启后：传感器数据本身正常（orientation=right-up、LightLevel≈60 lux、registry 全量读取），
但 iio-sensor-proxy 出现**独立故障**：D-Bus service 缺 `SystemdService=` → dbus 直接 exec 绕过 systemd；
iio 无传感器时 `exit(0)` + `Restart=always` → 无限重启风暴；风暴中 GNOME claim 被拒/丢失 →
自动亮度/旋转不可用（用户体感"还是不行"）。

已修复并实测：
- iio 源码补丁：无传感器/设备移除时不再退出，常驻等待 udev 热插（保留原 claim 轮询补丁）；
  新二进制已安装，sha256 `6d0e0f46…`。
- D-Bus service 增加 `SystemdService=iio-sensor-proxy.service`（`systemctl reload dbus`）。
- drop-in：`Restart=on-failure`（干净退出不循环，崩溃仍重拉）。
- `slpi-sensors-start.sh`：iio 步骤 `restart` → 幂等 `start`。
- 实测：`NRestarts=0`、单实例、cgroup 正确；D-Bus 激活经 systemd；GNOME 自动 re-claim 无失败；
  LightLevel 59.9、Orientation right-up。
- 新增开机自动验证：`sensor-boot-report.service`（enabled）→
  `/home/<user>/sensor-boot-report.txt` + `sensor-boot-report-history.txt`。
- 备份/回滚：`/root/sensor-fix2-backup-20261111-174444/rollback.sh`（用户可读副本 `/home/<user>/iio-fix2-backup-20261111-174444/`）。

下一轮：重启后由自动化任务（`~/.local` DSH schedule）验证冷启动表现。

---

# 第三轮修复（18:00）：iio 热插回调缺失

第二轮重启验证：启动链路 OK（无 auto_boot、registry 全量、无 PDM 崩溃），但属性全空。
根因：上游只有 `find_sensors()` 成功才连接 `uevent` 热插信号；常驻补丁提前 return 导致热插回调没注册；
同时 delayed-invocation 数组未初始化。

修复（iio v3，sha256 `c6e707c4…`）：
- `name_acquired_handler()` 开头无条件初始化 delayed 数组 + 连接 uevent，再做 find_sensors。
- 启动脚本增加保险：设备出现后检查 `HasAmbientLight`，仍为空则 warm restart iio 一次。
- 归档 `iio-sensor-proxy.v3-hotplug` / `.c.v3-hotplug` / `all-fixes-v3-hotplug.patch`；文档追加第四十二节。

当前 boot 实测（补丁后重启 iio）：NRestarts=0、单实例、LightLevel=59.6、Orientation=right-up、
无 claim 失败、boot 报告 VERDICT=OK。**下一次冷启动验证热插路径。**

---

# 第二次冷启动验证（18:14）结果

- **传感器功能正常**：LightLevel=59.6 lux、Orientation=right-up、HasAmbientLight=true；
  iio 单实例 active、NRestarts=0、无 claim 失败；dmesg 早期只有 `Direct firmware load ... failed`、
  无 PDM 崩溃；SLPI 由服务在 boot+82s 拉起、assign try[0]:0、registry 全量 1901。
- **热插路径仍未生效**（`[start] iio has no SSC sensors after device appeared (hotplug missed); doing one warm restart`）：
  设备 hotplug 的瞬间 SSC PD 还没就绪，`ssc_sensor_*_new_sync()` 失败，属于时序问题；
  启动脚本的 **warm restart 兜底成功**（18:03:22 `sensors OK`），GNOME 自动 re-claim。
  结论：hotplug 代码修复保留作为长期路径，短中期由 warm restart 保证开机可用。
- **sensor-boot-report.service 未运行**：systemd 报 ordering cycle
  （multi-user → report → slpi-sensors → multi-user），启动 job 被删除。已改为 **timer 触发**
  （`sensor-boot-report.timer`，`OnBootSec=5min`；service 不再 WantedBy=multi-user），
  `disable service` + `enable timer`，并手动执行 service 验证成功（当前 boot 已入 history）。

下一次（第三次）重启：端到端验证 timer 报告 + 热插兜底 + 传感器数据。

---

# 事故分析（18:16–18:34）：无法解锁不是传感器/指纹问题

**现象**：用户 18:16 左右合盖离开，18:32 唤醒后锁屏无法解锁，密码和指纹都反复失败；18:34 机器重启后恢复。

**时间线证据**（`/var/lib/suspend-watch.log`）：
```
START 18:16:42  boot=561ea080   <- 合盖 suspend（我们安排的定时 reboot 正好预定 18:16:41）
END   18:32:25                  <- 唤醒（15.7 分钟后）
START 18:32:53 / END 18:33:19   <- 再次休眠/唤醒
START 18:33:35                  <- 再次休眠（无 END）
boot  18:34:17                  <- 当前 boot
```
我们 18:15:41 执行了 `shutdown -r +1`（预定 18:16:41 重启）用于验证传感器修复；用户几乎同时合盖休眠。
shutdown 与 suspend 撞车 → 系统停在"半关机 + 休眠"的中间状态。唤醒后 gdm/会话/PAM/fprintd
处在该中间状态，认证必然失败（密码和指纹都失败），最终只能强制重启（18:34:17）。

**不是**传感器/指纹硬件/驱动回归：当前 boot 传感器正常（report timer 18:39:25 触发，VERDICT=OK，
LightLevel≈64.8、right-up、iio NRestarts=0），`fprintd-list` 正常识别 Goodix 设备与已录入指纹，
TA loader/qsee-supplicant 均 active。

**修复/防复发**：
1. 删除剩余的自动化重启任务（schedule 已删）；以后不再在用户可能离开时留 pending reboot。
2. 新增 `/usr/lib/systemd/system-sleep/40-cancel-scheduled-shutdown.sh`：
   suspend 前若存在尚未执行的定时 shutdown/reboot，自动 `shutdown -c` 取消并记录，
   避免再次出现"半关机+休眠"的不可解锁状态；同时记录 pre/post 诊断
   （systemd/gdm/fprintd/TA/gnome-shell 状态）到 `/var/lib/suspend-watch.log`。
   **已实测**：安排 5 分钟后重启 → 执行 hook pre → 定时重启被取消、无 pending job。
3. 清理 `suspend-watch.log` 的 NUL 损坏（硬断电遗留）。

---

# 延迟对照实验结果（2026-11-11T19:10:37+08:00）
```
===== 传感器启动延迟对照实验结果 2026-11-11T19:10:37+08:00 =====
结束原因: all rounds done
总轮次: 10

各延迟成功率:
PRE_DELAY=0s: 3/3 OK
PRE_DELAY=10s: 3/3 OK
PRE_DELAY=20s: 2/3 OK
PRE_DELAY=45s: 1/1 OK

最终选择: PRE_DELAY=10s
  规则：在 10/20/45 中挑最小的 100% 成功档（0s 仅作对照观察，不作为默认）；没有 100% 档则回退 45s。
  IIO_DELAY=8 保持不变。

说明：当前 boot 仍使用实验前配置，新配置下次开机生效。
完整日志: /var/lib/sensor-delay-exp/log.txt

--- 原始日志 ---
=== experiment initialized 2026-11-11T18:48:01+08:00  plan: 45x1, 20x3, 10x3, 0x3 (0s 仅对照) ===
2026-11-11T18:52:48+08:00 round=0 delay=45 cfg=45 result=OK why="orientation=(<'right-up'>,)" auto_boot=no pdm=0 iio=active iio_nr=0 iio_inst=1 reg=1901 last_ok="OK: orientation=(<'right-up'>,)"
2026-11-11T18:52:48+08:00 next: PRE_DELAY=20 (round 1/10)
2026-11-11T18:57:44+08:00 round=1 delay=20 cfg=20 result=FAIL why="timeout" auto_boot=no pdm=0 iio=active iio_nr=0 iio_inst=1 reg=0 last_ok=""
2026-11-11T18:57:44+08:00 next: PRE_DELAY=20 (round 2/10)
2026-11-11T18:59:32+08:00 round=2 delay=20 cfg=20 result=OK why="orientation=(<'right-up'>,)" auto_boot=no pdm=0 iio=active iio_nr=0 iio_inst=1 reg=1901 last_ok="OK: orientation=(<'right-up'>,)"
2026-11-11T18:59:32+08:00 next: PRE_DELAY=20 (round 3/10)
2026-11-11T19:01:18+08:00 round=3 delay=20 cfg=20 result=OK why="orientation=(<'right-up'>,)" auto_boot=no pdm=0 iio=active iio_nr=0 iio_inst=1 reg=1901 last_ok="OK: orientation=(<'right-up'>,)"
2026-11-11T19:01:18+08:00 next: PRE_DELAY=10 (round 4/10)
2026-11-11T19:02:56+08:00 round=4 delay=10 cfg=10 result=OK why="orientation=(<'right-up'>,)" auto_boot=no pdm=0 iio=active iio_nr=0 iio_inst=1 reg=1901 last_ok="OK: orientation=(<'right-up'>,)"
2026-11-11T19:02:56+08:00 next: PRE_DELAY=10 (round 5/10)
2026-11-11T19:04:32+08:00 round=5 delay=10 cfg=10 result=OK why="orientation=(<'right-up'>,)" auto_boot=no pdm=0 iio=active iio_nr=0 iio_inst=1 reg=1901 last_ok="OK: orientation=(<'right-up'>,)"
```

---

# 延迟对照实验分析（2026-11-11 19:10 完成）

结果（10 轮自动重启验证，判定条件：真实传感器数据 + 无 auto_boot + 无 PDM 崩溃 + iio 单实例 + registry>1000）：

| PRE_DELAY | 成功/总 | 备注 |
|---|---|---|
| 45s | 1/1 | 对照 |
| 20s | 2/3 | **1 次失败**：boot -8 中 attach 只读了 hw_platform 就静默卡住（无 PDM 崩溃、无 assign -22），start 脚本 90s 后判失败 |
| 10s | 3/3 | |
| 0s | 3/3 | 仅对照，不作为默认 |

**最终配置：PRE_DELAY=10, IIO_DELAY=8**（开头到传感器可用约 90 秒，比 45s 快约 35 秒）。
实验 timer 已自动禁用，系统无残留 pending reboot。

**重要发现（残留风险）**：20s 那轮的失败是**与延迟无关的随机静默失败**——PD 在读完 hw_platform 后
不再请求 registry，但没有崩溃（pdm=0）、没有 assign -22；同款故障在历史上也出现过。近期干净 boot 约
1/14 会出现一次，恢复手段只有干净重启。也就是说：**开机失败率已从"约一半"降到"偶发一次"，
但尚未归零**；延迟高低不影响这个概率（0s/10s 本次全过，20s 反而挂了一次）。
另：本轮结束时用户触发了一次 3 秒 suspend，PD 报 frpc_dsp 崩溃，但监听器重连后 sensors 恢复了
（suspend 后恢复能力正常）。

---

# 延迟对照实验结果（2026-11-11T19:29:38+08:00）
```
===== 传感器启动延迟对照实验结果 2026-11-11T19:29:38+08:00 =====
结束原因: all rounds done
总轮次: 5

各延迟成功率:
PRE_DELAY=0s: 5/5 OK

最终选择: PRE_DELAY=0s
  规则：0s 专项验证：全部成功 -> PRE_DELAY=0；出现失败 -> 回退 10s（并在结果里给出失败轮诊断）
  IIO_DELAY=8 保持不变。

说明：当前 boot 仍使用实验前配置，新配置下次开机生效。
完整日志: /var/lib/sensor-delay-exp/log.txt

--- 原始日志 ---
=== 0s 专项验证 2026-11-11T19:19:28+08:00  5 轮；全部成功则最终 PRE_DELAY=0，有失败则回退 10 ===
2026-11-11T19:23:33+08:00 round=0 delay=0 cfg=0 result=OK why="orientation=(<'right-up'>,)" auto_boot=no pdm=0 assign0=0 hw=3 seg=2816 reg=1901 iio=active iio_nr=0 iio_inst=1 last_ok="OK: orientation=(<'right-up'>,)"
2026-11-11T19:23:33+08:00 next: PRE_DELAY=0 (round 1/5)
2026-11-11T19:25:04+08:00 round=1 delay=0 cfg=0 result=OK why="orientation=(<'right-up'>,)" auto_boot=no pdm=0 assign0=0 hw=3 seg=2816 reg=1901 iio=active iio_nr=0 iio_inst=1 last_ok="OK: orientation=(<'right-up'>,)"
2026-11-11T19:25:04+08:00 next: PRE_DELAY=0 (round 2/5)
2026-11-11T19:26:37+08:00 round=2 delay=0 cfg=0 result=OK why="orientation=(<'right-up'>,)" auto_boot=no pdm=0 assign0=0 hw=3 seg=2816 reg=1901 iio=active iio_nr=0 iio_inst=1 last_ok="OK: orientation=(<'right-up'>,)"
2026-11-11T19:26:37+08:00 next: PRE_DELAY=0 (round 3/5)
2026-11-11T19:28:07+08:00 round=3 delay=0 cfg=0 result=OK why="orientation=(<'right-up'>,)" auto_boot=no pdm=0 assign0=0 hw=3 seg=2816 reg=1901 iio=active iio_nr=0 iio_inst=1 last_ok="OK: orientation=(<'right-up'>,)"
2026-11-11T19:28:07+08:00 next: PRE_DELAY=0 (round 4/5)
2026-11-11T19:29:38+08:00 round=4 delay=0 cfg=0 result=OK why="orientation=(<'right-up'>,)" auto_boot=no pdm=0 assign0=0 hw=3 seg=2816 reg=1901 iio=active iio_nr=0 iio_inst=1 last_ok="OK: orientation=(<'right-up'>,)"
2026-11-11T19:29:38+08:00 === experiment finished (all rounds done) ===
```

---

# PRE_DELAY=0 专项验证结果（2026-11-11 19:29 完成）

- 本轮 5/5 OK；加上上一轮实验 0s 的 3/3，**累计 8/8 全部成功**。
- 每轮诊断字段完全一致：`auto_boot=no, pdm=0, assign0=0, hw=3, seg≈2816, reg=1901, iio=active/NRestarts=0`
  —— 与 10s/45s 成功 boot 的形态相同，**没有任何"时序异常"特征**（没有 attach 提前/监听器未就绪/assign 异常）。
- 结论：0s 可行。保留 **PRE_DELAY=0, IIO_DELAY=8**。
- 实测开机到传感器可用约 **79 秒**（boot+37s 服务启动 → boot+39s SLPI → boot+79s sensors OK），
  比之前的 45s 配置（约 2 分钟）快约 1 分钟。
- 备注：已知的**随机 PD 静默卡死**风险仍在（与延迟无关，近期观察约 1/14），本次 0s 8 轮未命中；
  遇到时干净重启即可恢复。实验编排 timer 已自动禁用，无残留 pending reboot。
