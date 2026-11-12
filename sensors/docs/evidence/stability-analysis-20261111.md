# 传感器启动稳定性分析（2026-11-11 17:30）

设备：Xiaomi Mi Pad 5 Pro (elish, SM8250)，Armbian，kernel `6.18.44-current-sm8250`
分析对象：
- 交接文档：`/home/<user>/xiaomi-elish-sensors-v1-final/docs/HANDOFF-FULL-20261111.md`（重点第 29~39 节）
- 归档：`/home/<user>/xiaomi-elish-sensors-v1-final/`、`/home/<user>/sensor-test/`
- 现场：本次 boot（17:11:31 开机）日志、`/var/log/hexrpc-boot.log`、`journalctl -b -5..0`、调参实验数据

---

## 0. 一句话结论

**交接文档把问题定性为"概率型启动竞态"，但实测证据表明：现在观测到的"随机不稳定"主要是一条确定性的软件级联 bug，不是传感器本身或延迟档位的随机性。**

级联链：

```
健康检查永远判假（探针字符串不会出现在 journal）
  → 健康检查"失败"时脚本故意保留内核固件 slpi.mbn
  → 在 120s 健康窗口内重启 / 强制断电 时，systemd 不执行 ExecStop（服务还在 activating）
  → 固件残留到下次开机
  → 内核 boot+5s 看到固件，抢先 auto_boot SLPI（监听器还在 boot+83s 才起）
  → sensor_process PD 等文件服务，40s 后 USER-PD DOG 打死它（dog_virtual_user）
  → SLPI SSR；同一 boot 内第二次 SCM assign 六个候选全部 -22
  → fastrpc 设备在、但内存保护没配上；hexagonrpcd attach 只读到 hw_platform 就挂死
  → 整个 boot 里传感器彻底不可用，只能重启
```

最近 6 个 boot 里的统计（journal 期 12:28~17:23）：

| 情形 | 次数 | 结果 |
|---|---|---|
| 开机时固件存在（泄漏） | 3 | **3/3 全部机械性失败**（12:28、13:01、17:11） |
| 开机时固件不存在（干净） | 2（另 1 个 13:09 boot 被硬断电打断，未跑到服务） | **2/2 成功**；共 3 段 attach 全部读到完整 registry 序列（12:40、12:57 重连、13:01），12:59 boot 的 gdbus OK |
| 中断/未知 | 1 | 13:09 boot 日志被硬断电截断 |

也就是说：**"失败"几乎完全由"开机时固件在不在"决定，而这件事由一个可修的脚本 bug 决定**。干净路径下 attach 窗口、warm attach、数据完整性目前都没有暴露出随机失败。

另外两个独立的真实风险：
1. **suspend/resume**：实测一次合盖 suspend 后 PD `frpc_dsp` 崩溃（12:54:43 suspend → 12:57:05 crash）；该次监听器重连后恢复了全量服务，但如果它触发 SLPI SSR，则同 boot assign 已死 → 传感器到重启前都不可用。
2. **失败后无法同 boot 恢复**：`qcom_scm_assign_mem` 在同一 boot 内不可重复，任何 PD 崩溃/SSR 之后都只剩"重启"一条路。这会把任何一次性故障放大成整次开机不可用。

**当前这次 boot（17:11）就处在这个确定性失败态**：`LightLevel=0`、`AccelerometerOrientation=undefined`、`HasAmbientLight=false`，`/dev/fastrpc-sdsp` 存在、hexagonrpcd 卡死、iio 每 30s 报 `'registry' sensor timed out`。同 boot 无法恢复，需要一次**干净重启**（见第 7 节）。

---

## 1. 本次 boot（17:11）失败证据链

时间基准：真实 boot = 17:11:31（`uptime -s`）；fake-hwclock 在 boot 早期把钟回拨，journal 开头显示的 17:10:2x 是假象，以下用 dmesg 的 boot 相对时间。

| boot 相对时间 | 事件 | 证据 |
|---|---|---|
| 5.02s | SLPI/adsp/cdsp remoteproc 注册 | dmesg |
| **5.044928s** | `remoteproc0: powering up slpi` ← **没有任何用户态写 state，auto_boot** | dmesg |
| **5.051503s** | `Booting fw image qcom/sm8250/xiaomi/elish/slpi.mbn, size 7633828` ← 开机时固件在 `/lib/firmware` | dmesg |
| 5.258897s | `remote processor slpi is now up` | dmesg |
| 6.034208s | `SDSP assign try [0]: 0`（第一次 probe 成功） | dmesg |
| 38.2s | `slpi-sensors.service` ExecStart 开始（multi-user），45s 延迟从这里算 | journal |
| **45.277442s** | `PDM: service 'sensor_process' crash: ...dog_virtual_user` = SLPI up 后 **40.0s** USER-PD 看门狗超时 | dmesg |
| 45.297s | `USER-PD DOG detects stalled initialization` / crash #1 / recovering | dmesg |
| 45.516~45.519s | `qcom_scm: Assign memory protection call failed -22` ×6 → `all SDSP assign attempts failed (-22), continuing` | dmesg |
| **83.2s** | 监听器才 attach（17:12:55），只发了一个 `hw_platform` 请求（6B）后挂死 | hexrpc-boot.log:8460-8466 |
| 17:14:55 | 健康检查写 `sensors FAILED (fw kept)` | hexrpc-boot.log:8466 |

失败的本质：
- 监听器在 **boot+83.2s** 才起（service 在 boot+38.2s 启动 + PRE_DELAY=45），而 PD 看门狗是 **40s**。只要固件在开机时存在，监听器必然晚 38s 以上，PD 必然被打死——**这是确定性的，不是概率的**。
- PD 死后 SLPI SSR，fastrpc 重新 probe 时 SCM assign 全 -22（同 boot 第二次 assign 不可重复），此后 attach 永远只能读到一个 `hw_platform` 就卡住：本次 hexrpc 日志 openat 1 次、read 2 次、`sns_reg_config`/registry/`sns_reg_version` 全 0；对比成功 boot 是 openat 202 次、read 608 次、registry 相关 1902 行、`sns_reg_config` 6 次。
- 症状一致：`dmesg | grep PDM` 有 `dog_virtual_user`、`dmesg | grep SDSP assign` 有 `try [0]: -22`、hexrpc 日志只有 `hw_platform`、iio 反复 `registry timeout`。

补充：当前 `/dev/fastrpc-sdsp` **是存在的**（`systemctl status 'dev-fastrpc\x2dsdsp.device'` → active (plugged) since 17:12:17），hexagonrpcd（PID 4554）也还在；问题是 SCM assign 失败后 PD 起不来，attach 空转。不要依据普通 shell 里 `ls /dev/fastrpc-*` 判断（DSH 的 bash 跑在 bwrap 私有 /dev 里，看不到这个节点）。

---

## 2. 固件为什么会留在开机路径上（三个叠加的 bug）

### 2.1 健康检查永远是假阴性（探针写错）

`/usr/local/sbin/slpi-sensors-start.sh` 的成功判据是：

```bash
journalctl -b -u iio-sensor-proxy | grep -q "Found SSC"
```

但 iio-sensor-proxy 源码里 4 处 `Found SSC light/accel/compass/proximity` 全是 `g_debug`（`/home/<user>/iio-sensor-proxy/src/drv-ssc-*.c`），而 unit 里 `G_MESSAGES_DEBUG=all` 是**注释掉的**（`/usr/lib/systemd/system/iio-sensor-proxy.service:10`）。→ 该字符串永远不会出现在 journal。

实测：`/var/log/hexrpc-boot.log` 里 `sensors FAILED` 5 次、`sensors OK` **0 次**；连归档的"成功 boot"日志（`logs/final/hexrpc-boot.log` 末尾 10:17:46）也是 `FAILED (keep system safe)`。而 12:39 那个 boot 在 12:41:23 已经有 iio `Mount matrix ... all 0`（=SSC 传感器已创建）——功能成功却被判 FAILED。

### 2.2 失败时脚本故意保留固件，且"保留"没有收益

```bash
if journalctl ... grep -q "Found SSC"; then
  echo OK; rm -f "$FW/slpi.mbn"     # 永不执行
else
  echo "FAILED (fw kept)"           # 永远走这里
fi
```

设计意图是"运行时留着固件，PD 崩溃后 SSR 能重载"。但本次失败证明：**SSR 之后同 boot SCM assign 全 -22，fastrpc 内存保护配不上，留着固件并不能恢复任何东西**——它唯一的实际效果是给下一次开机埋雷。

### 2.3 ExecStop 在"健康窗口内重启"时不执行

服务是 `Type=oneshot` + `ExecStart` 里 `sleep 120` 后才做健康检查（start 脚本全程约 165s）。在 ExecStart 还没结束时重启，systemd 认为 unit 还在 activating，**不会跑 ExecStop**，于是 stop 脚本的 `rm` 没执行。

实测对得上：
- `/var/log/hexrpc-boot.log` 只有 3 行 `stop: removed kernel firmware`（12:39:15、12:59:48、13:09:58，对应 3 次正常关机）；
- 12:59 boot 在 13:01:48 重启（gdbus OK 后 ~6s，健康检查要到 ~13:02:57 才结束）→ 无 stop 行 → 13:01 boot 开机 4.26s 就 auto_boot 真固件（`Booting fw image ... 7633828`）→ 44.6s PD dog → assign -22。一模一样的链。

### 2.4 12:18 的调参改动把"安全不变量"改掉了（文档/现实漂移）

- 10:49 的版本（`slpi-sensors-start.sh.bak-symlink`，README-FINAL 描述的那版）把 `/lib/firmware/.../slpi.mbn` 做成指向 `/run/sensor-fw/slpi.mbn` 的**符号链接**：`/run` 开机清空 → 内核永远看不到固件 → 无论怎么断电/崩溃，下一次开机都不可能 auto_boot。这是一个真正的安全不变量。
- 12:18 为了自动调参，脚本改成"`rm` 内核路径 + `cp` 真实文件 + 120s 健康检查"（现行版本），删掉了符号链接方案。于是"重启后固件必不存在"的保证缩水成"上一次干净关机且 ExecStop 跑到了"。
- 结果就是第 2.1~2.3 叠加出的泄漏，再被第 0 节的确定性级联放大。README-FINAL 仍写符号链接、`ExecStop` 不删链接，与机器上的实际脚本不一致。

> 当前 boot 的固件来源：13:09:58 那次 boot 的 ExecStop 确实删过固件（kernel 有 `Direct firmware load ... failed` 为证），但 17:10 的内核又能加载 7,633,828B 真固件。最可能：13:09 boot 实际继续启动、脚本在 ~13:12 把固件 cp 回去，随后机器被强制断电，由 zram 上的 `/var/log` 与 journal 尾部一起丢失（journal 只到 uptime 13.1s）。这一段属于**推断**，缺串口/持久化日志无法 100% 证实，但不影响上面的机制结论。

---

## 3. 交接文档里需要纠正的几处

| 文档说法 | 实际证据 |
|---|---|
| §38 "P0 10+8 / P1 10+2 / P2 5+2 / P3 0+2 各 3 轮"，"0/15/30s 失败、10s 4 轮中 1 失败" | 现存编排脚本（`/usr/local/sbin/sensor-orch.sh`，12:59:42）实际是 PRE=(30,20)、IIO=(8,8)、ROUNDS=4、2 个 phase；12:28–12:59 那轮数据已被 `rm -rf /var/lib/sensor-validate` 清掉，0/15s 档**没有任何原始数据**。"12:28 round1 OK"也不成立：12:28 那个 boot 本身就是固件泄漏（4.25s auto_boot → 44.5s dog → 失败）。 |
| §39 "不是干净的时间依赖，而是概率型竞态；45s 多数成功但 17:11 仍失败" | 观测到的失败 3/3 都是"开机固件存在"的确定性级联；干净 boot 2/2 成功（3 段 attach 均完整）。调参期间"每隔一次失败"的主因是泄漏，而且轮次之间不独立（reboot 落在 120s 窗口里 → 下一轮被污染）。"45s 最优"没有被数据支持（有效样本：30s 1/1 成功、45s 3/3 成功，n 太小）。 |
| "健康检查通过后删除固件" | 健康检查判据写错，可能从未通过（0 次 OK）。 |
| README-FINAL：固件路径是 `/run/sensor-fw` 符号链接、ExecStop 不删链接 | 现行脚本是普通文件 + ExecStop 删文件；符号链接版只在 `.bak-symlink`。 |

其余历史结论仍然成立，且与本次无关：VMID `{3,5}`、fastrpc SMMU sensors-PD 补丁、warm attach 补丁、iio claim 竞态修复——这些是把"干净路径"跑通的关键，也是 12:39/12:57/13:01 三次 attach 能读全 1902 行 registry 的原因。

关于 attach 窗口（matrix 实验）：0ms 档 delta=7ms 成功，50/100/200/500/1000ms 档 delta=60/108/213/511/1009ms 全失败——这是真实存在的敏感性，但它**不是本次失败的触发点**：本次设备在 17:12:17 就已存在，17:12:55 才 attach，晚 38s，且内核侧 assign 已经失败，warm attach 的微秒级重试救不了。

---

## 4. 发生率与概率（分层评估）

样本很小（journal 只保留 11-11 12:28 之后 6 个 boot；11-10 的日志已轮转丢失），以下给区间而不是精确值：

| 条件 | 估计 | 依据 |
|---|---|---|
| P(本 boot 失败 \| 开机时 `/lib/firmware/.../slpi.mbn` 存在) | **≈100%** | 3/3 观测；机制上监听器必然晚于 40s 看门狗 |
| P(开机时固件存在 \| 上一次是在健康窗口内重启或强制断电) | 高 | 调参期约一半 reboot 造成泄漏；硬断电必泄漏 |
| P(开机时固件存在 \| 每次都用 `systemctl reboot` 且等健康窗口结束) | 低 | 干净关机 ExecStop 会删；但健康检查恒假导致"失败保留"仍可能留雷，取决于是否等到脚本退出 |
| P(干净 boot 能枚举传感器) | **观测 3/3（0.8~0.95）** | 12:39/12:57/13:01；历史上 10:15、16:42 也成功；剩余风险见下 |
| P(每次开机传感器可用 \| 当前脚本、随手重启/关盖） | **粗估 50~70%** | 6 个 boot 里 1 个干净可用、1 个可用后 suspend 崩、3 个机械失败、1 个中断 |
| suspend 后 PD 崩溃率 | 有实测 1 次（12:54:43→12:57:05） | 崩溃后监听器重连曾恢复；若触发 SSR 则同 boot 不可用 |

真正剩余的未知风险（干净 boot 下仍可能失败）：
1. attach 实际 delta 是否偶尔落到 >60ms（系统负载高时）——目前没有把 delta 写进日志，无法统计；
2. suspend/resume 的 PD 崩溃与 SSR 后不可恢复；
3. 6.18.44 内核 + 厂商固件在"崩溃后再 attach"这条路径上是否有 pMOS 6.12 没有的问题（无对照，属弱证据）。

结论：**修掉固件泄漏这条确定性链后，预期"开机可用"会从 ~50–70% 升到 90%+（推测，需实测验证）；但 suspend 后的可用性和罕见 attach 超时仍是遗留风险，且任何 PD 崩溃都只能重启。**

---

## 5. 建议的修复（按性价比排序）

### P0：脚本/服务层，不需要改内核（预计能把主要失败模式清零）

1. **健康检查换成真实判据**（或至少不要用它决定删不删固件）：
   - 用 `gdbus call ... Get ... LightLevel / AccelerometerOrientation` 或 `timeout 10 monitor-sensor` 解析输出；
   - 或直接读 hexrpc 日志：出现 `sensors/registry` 文件访问即成功；
   - 顺手把 `/usr/lib/systemd/system/iio-sensor-proxy.service` 的 `#Environment="G_MESSAGES_DEBUG=all"` 打开，否则 `Found SSC` 永远不出现。
2. **失败也删固件**：健康窗口结束（不论成败）一律 `rm -f /lib/firmware/.../slpi.mbn`。SSR 保固件没有收益（assign 已死），只制造下次开机的雷。
3. **恢复"开机时内核看不到固件"的不变量**：
   - 回到 `.bak-symlink` 的符号链接方案（`/run/sensor-fw/slpi.mbn`，`/run` 开机清空），或
   - 加一个 early-boot oneshot（`After=local-fs.target`、`Before=multi-user.target`）无条件 `rm -f` 内核路径固件。
   - 两者建议都做（符号链接防"到运行时"，early unit 防"任何遗留"）。
4. **不要依赖 ExecStop**：它只在 unit 已 active 时执行；健康窗口内重启会跳过。真正的保险是第 3 条的 early-boot `rm`。
5. **启动脚本防呆**：启动前检查——若 SLPI 已经 `running`、或 `/dev/fastrpc-sdsp` 已存在、或内核在 boot 早期已加载固件（`dmesg | grep 'Booting fw image.*slpi'`），立即写一条 `auto_boot_detected` 日志并快速失败，不要白等 120s。
6. **诊断增强**（下次失败 5 秒定位）：在 hexrpc-boot.log 记录——attach 相对设备的精确 delta(ms)、SLPI power up 的 boot 相对时间、`SDSP assign try[0]` 结果、健康判据的实际输出。

### P1：suspend 与真实竞态

7. suspend 策略需要实验：现在 hook 只停 iio/重启监听器。合盖后 PD `frpc_dsp` 崩是已知的；如果恢复流程会触发 SLPI SSR，则同 boot 无解。可选路线：
   - (a) suspend 前把整个栈停掉（stop SLPI 已证实安全），resume 后按"开机流程"重来——但注意同 boot 重新 start SLPI 会 assign -22，可能不可行；
   - (b) 确认 resume 后 PD 崩溃是否只是 PD 级（可重连恢复，如 12:57 那次），而不是 SLPI SSR；用 `dmesg` 区分 `crash detected in slpi` 与 `frpc_dsp`。
   - (c) 若都不可靠：接受"合盖一次后传感器可能失效，重启恢复"，并在 UI/通知上提示。
8. initramfs 里被打进了真 `slpi.mbn`（11:31 `update-initramfs` 时 `/lib/firmware` 还是符号链接，工具把目标内容 7,633,828B 打进去了；`lsinitramfs /boot/initrd.img-6.18.44-current-sm8250` 可见）。本次启动已确认固件是从 rootfs 加载的，但这是一个不该存在的副本，建议重建 initramfs 清掉（避免任何早期 probe 抢跑的可能）。
9. 长期对照：pMOS 6.12/6.13 内核 + 本归档二进制/数据复测，验证"崩溃后重 attach"是否是 6.18 回归（文档原建议，仍是有效方向）。

### P2：文档纠偏

10. 修正 §38/§39 的调参结论与"12:28 round1 OK"；把"健康检查恒假"、ExecStop 窗口、"开机固件存在 ⇒ 必失败"写进交接；归档当前实际生效的脚本（而不是 README-FINAL 描述的旧方案）。

---

## 6. 一句话给"不稳定可能性"的答案

- 现在：**只要一次开机时固件残留，这次开机 100% 没有传感器；而残留很容易触发（失败保留 + 窗口内重启/硬断电），所以体感"一半概率坏"。**
- 传感器/固件链路本身（attach、registry、iio）在干净路径下目前没看到随机失败：最近的干净 boot（12:39、12:59）全部成功，历史 10:15、16:42 的归档成功案例也一致。
- 因此"不稳定的可能性"主要是**流程缺陷的概率**，可以通过脚本修复大幅消除；剩下的是 suspend 与罕见的 attach 超时，以及"失败后不能同 boot 恢复"这个硬限制。

---

## 7. 现在怎么恢复（本 boot 已确定无法自愈）

本 boot 的 SCM assign 已经失败，`systemctl restart slpi-sensors` / 重启 iio 都没用，只能整机重启：

```bash
# 1) 清掉本次失败的固件残留，保证下次内核不会 auto_boot
sudo rm -f /lib/firmware/qcom/sm8250/xiaomi/elish/slpi.mbn
# 2) 干净重启（不要长按电源/强制断电）
sudo reboot
```

重启后约 2 分钟做健康判定：

```bash
# 开机早期应看到 "Direct firmware load ... slpi.mbn failed"（说明内核没抢到固件）
sudo dmesg | grep -E 'powering up slpi|Booting fw image|PDM|SDSP assign'
# SLPI 应由服务在 boot+80s 左右拉起，hexrpc 日志应出现完整 registry 读取
grep -c 'registry' /var/log/hexrpc-boot.log
# 最好用的用户侧判据
gdbus call --system --dest net.hadess.SensorProxy --object-path /net/hadess/SensorProxy \
  --method org.freedesktop.DBus.Properties.Get net.hadess.SensorProxy AccelerometerOrientation
monitor-sensor
```

如果重启后仍没传感器：`sudo touch /etc/disable-sensors && sudo reboot`（整条链不启动，系统回到完全正常可用），然后再按 P0 修改脚本。

> 注意：**不要长按电源强制断电**。硬断电会保留固件，导致下一次开机必失败（就是今天这一次的成因之一）。

---

## 附：关键证据索引

- 本次失败时序：`dmesg`（raw：[5.044928] powering up slpi、[5.051503] Booting fw、[45.277442] dog_virtual_user、[45.516583] assign -22）
- 本次 attach 片段：`/var/log/hexrpc-boot.log:8460-8466`
- 成功 attach 全量序列：`/var/log/hexrpc-boot.log:9-2821`（12:40:48，2815 行 / registry 1902 行）、`logs/final/hexrpc-boot.log`（10:15:46，2814 行）
- 健康检查判据：`/usr/local/sbin/slpi-sensors-start.sh`；`/usr/lib/systemd/system/iio-sensor-proxy.service:10`（G_MESSAGES_DEBUG 被注释）；`/home/<user>/iio-sensor-proxy/src/drv-ssc-light.c:43`（g_debug）
- 固件保留/清理：`slpi-sensors-start.sh:10-14`（rm/cp）、`:19-25`（健康检查/保固件）、`slpi-sensors-stop.sh:2`（rm）、`/usr/local/sbin/slpi-sensors-stop.sh`、`/etc/systemd/system/slpi-sensors.service`（Type=oneshot + RemainAfterExit）
- 安全不变量旧版：`/usr/local/sbin/slpi-sensors-start.sh.bak-symlink`
- 调参编排：`/usr/local/sbin/sensor-orch.sh`、`/var/lib/sensor-validate/{log.txt,SUMMARY.txt,phase,n}`、归档 `logs/sensor-tuning-validation/`
- matrix attach 窗口：`/home/<user>/xiaomi-elish-sensors-v1-final/matrix/log.txt`（delta 7ms ok，60/108/213/511/1009ms fail）
- 数据完整性：`diff -rq /usr/share/qcom-ssc-elish/sensors /run/sensors-rootsvc/sensors` 无差异（298/299 文件，registry 155/155）
