# 传感器项目历史交接（从原 HANDOFF 抽取：第 17–45 节，去除指纹项目部分）

> 原文档还包含指纹移植（Goodix GF3626）章节 1–16，与本传感器安装包无关，已剔除。
> 本文件用于追溯传感器栈是怎么一步步跑通的，安装请以 `INSTALL.md` 为准。

## 十七、传感器（SLPI）进展与下次继续（2026-11-10 12:15）

**已打通：**
- 内核 fastrpc SDSP probe 不再失败（模块打补丁：SCM assign 失败可继续）
- `/dev/fastrpc-sdsp` 出现；hexagonrpc 0.5.0 从源码编好装到 `/usr/local`
- hexagonrpcd-sdsp 用 `-f /dev/fastrpc-sdsp -d sdsp -s -R /lib/firmware/qcom/sm8250/xiaomi/elish`
- libssc 0.4.4 + iio-sensor-proxy（带 ssc-support）已装

**当前阻塞：**
- sensor PD 崩溃：`dog_virtual_user: stalled initialization` / `SNS_REG_INIT` 失败
- hexagonrpcd 报 `Unsupported handle: 4294967295` 后退出
- 判断：SDSP 内存权限未建立（我们绕过了 `qcom_scm_assign_mem`），需正确 VMID
- Android DTB（只读）线索：`/soc/qcom,msm_fastrpc` 有 `qcom,adsp-remoteheap-vmid = <0x16 0x25>`

**本次已做：** fastrpc.ko 改为按候选表依次尝试 assign（{0x16,0x25} {3,0x16,0x25} {3,5} {5} {3} {0x16} {0x25}），dmesg 会打印 `SDSP assign try [i]: err`。
模块热重载时卡在 rmmod（refcnt=-1），SLPI crashed → 已配置好开机自启 + 持久 journal，**需重启恢复**。

**下次继续（重启后）：**
1. `sudo journalctl -b -1 -k | grep "SDSP assign try"` 看哪个候选成功
2. 若成功：从候选表确定 vmids 值（可撤回 trial 逻辑或保留）
3. 验证 `/dev/fastrpc-sdsp`、`systemctl status hexagonrpcd-sdsp`、`monitor-sensor`
4. 若 sensor PD 仍崩，检查 hexagonrpcd 是否真的 serve 了 registry（`-R` 路径、`/usr/share/qcom` 软链）
5. 成功后：GNOME 自动旋转/自动亮度；并把 hexagonrpcd-sdsp 恢复稳定自启
6. 注意：**boot_a 绝对不能写**；boot_b 已打 vmids+指纹补丁并有备份

## 十八、传感器突破：SM8250 SDSP 正确 VMID（2026-11-10 12:30）

**关键突破：**
- fastrpc 模块按候选表尝试 assign，**第一个候选 `{0x16, 0x25}` 成功**（dmesg: `SDSP assign try [0]: 0`）
- 这正是 Android DTB 里 `qcom,adsp-remoteheap-vmid = <0x16 0x25>` 的值（VMID 22/37）
- 现在 `/dev/fastrpc-sdsp` 稳定存在、SLPI running、hexagonrpcd-sdsp active、sensor PD 不再崩溃

**新阻塞：**
- hexagonrpcd 手动运行输出：
  `Unsupported handle: 4294967295`
  `Starting /usr/local/bin/hexagonrpcd (INIT_ATTACH_SNS) on /dev/fastrpc-sdsp`
- 说明内核 fastrpc 的 `FASTRPC_IOCTL_INIT_ATTACH_SNS` → `fastrpc_init_attach(fl, SENSORS_PD=2)`
  返回了无效 handle（0xffffffff），SSC（sensor core）没起来 → libssc 仍 `'registry' sensor timed out`
- 代码参考：`drivers/misc/fastrpc.c:2145`（INIT_ATTACH_SNS）、`:104`（SENSORS_PD=2）、`:1702`（fastrpc_init_attach）

**下一步：**
1. 重新 clone https://github.com/linux-msm/hexagonrpc 并 grep "Unsupported handle"，确认是哪个 ioctl/返回值导致
2. 对比 pMOS 内核 fastrpc 是否有 INIT_ATTACH_SNS 相关补丁（我们的 6.18.44 可能缺 sensorspd 支持）
3. 备选：用旧式 attach 方式（hexagonrpcd 0.3.x 行为）或给 fastrpc 补 `qcom,fastrpc-cb`/sensorspd 相关 DT 属性
4. VMID 修复已完成：fastrpc.ko 候选表可简化成只试 `{0x16,0x25}`（现已验证成功）

### 下一步关键判断（重要）
pMOS 设备包要求 `hexagonrpcd >= 0.3.2-r3`，而我们装的是 **0.5.0**（新版用 `INIT_ATTACH_SNS` ioctl，报 `Unsupported handle: 0xffffffff`）。
**很可能应改用 0.3.2（pMOS r3 版本）**，其 attach 机制与主线 fastrpc 驱动匹配：
```bash
git clone https://github.com/linux-msm/hexagonrpc
git checkout v0.3.2   # 或找 pMOS 的 r3 补丁
# 编译安装后：systemctl start hexagonrpcd-sdsp && monitor-sensor
```
VMID 已确定 `{0x16,0x25}`，fastrpc.ko 候选表可固定为这一组。

## 十九、系统 hang 事件与"安全锁死"状态（2026-11-10 13:06）

**hang 机制（有证据）：**
- SLPI 由 remoteproc `.auto_boot=true` 自动拉起 → sensor_process 等 registry 超时（40s dog）→ 崩溃 → remoteproc SSR 自动恢复 → 无限循环 → 系统硬冻结（日志里 crash #14/#15/#16/#17）
- 每次卡死时，日志都能看到 sensor_process 崩溃循环

**已做锁死措施（保证 SLPI 不会再自启）：**
1. SLPI 固件移走：`slpi.mbn` → `slpi.mbn.disabled`，分段 `slpi.b*` → `slpi-disabled/`（内核 request_firmware 直接失败，SLPI 不会启动）
2. `slpi-start` / `hexagonrpcd-sdsp` / `iio-sensor-proxy` 全部 disabled
3. fastrpc 模块已回退为原厂 `.ko`（备份 `.bak-tolscm-20261110-115844`）；候选 VMID 实验模块备份 `.bak-candidate-*`
4. 验证：SLPI=offline、sensor 崩溃 0、failed units=0

**注意：最近一次 hang 发生在 SLPI 已锁死后（编译 make -j8，同时 Firefox 高负载、CPU 66°C）**，暂无法完全归因于 SLPI，需观察；如再发生，优先怀疑热/负载或其它驱动。

**恢复传感器的正确路径（下次）：**
1. 先编译安装 `qcom_q6v5_pas.ko` 的 `auto_boot=false`（源码已改，模块未安装）→ 重启 → SLPI 不会自启
2. 再修复 hexagonrpcd（0.3.2 listener 模式）并**全部离线验证**（不启动 SLPI）
3. 单次受控测试：监听器先起 → 手动 start SLPI → 40s 内看 registry 是否成功；任何 sensor_process 崩溃立即 stop SLPI
4. 成功后恢复固件/服务自启

## 二十、暂停状态总结（2026-11-10 14:04）

**传感器进展（重大）：**
- 崩溃根因确认：sensor_process 需要 hexagonrpcd 通过 FastRPC 提供 registry 文件；服务缺失/attach 失败 → 40s 看门狗崩溃 → SSR 循环 → 系统 freeze
- **hexagonrpcd 0.3.2（listener + `-s`）实测让 sensor_process 稳定运行 45s 零崩溃**（此前 0.5.0 会 `Unsupported handle`）
- 正确 VMID 已找到：`{0x16, 0x25}`（Android DTB 的 `qcom,adsp-remoteheap-vmid`），SDSP 内存权限分配成功
- 但 SSC 仍未被 libssc 枚举（'registry' sensor timeout），需继续调（可能是 attach 时机/libssc 交互）
- 期间多次 hard freeze：freeze 前最后内核日志多停在音频 DSP（cs35l41/q6asm-dai）；磁盘无 IO/ext4 错误

**当前安全状态（暂停用）：**
- SLPI 固件锁死（`slpi.mbn.disabled` + `slpi-disabled/`）→ SLPI 不会自启
- 传感器相关服务全禁用；fastrpc 恢复原厂模块
- 已启用崩溃捕获：`/etc/sysctl.d/99-crash-debug.conf`（panic_on_oops/panic_on_rcu_stall，ramoops/pstore 已注册）
- swap 永久扩容：zram 5.4G（100%）+ /swapfile 4G（fstab prio=1）

**下次继续：**
1. 先观察是否还有 freeze；若有，读 `journalctl -b -1` + `/sys/fs/pstore`（ramoops）定位
2. 若稳定，继续传感器：用 hexagonrpcd 0.3.2 + 正确 VMID，重点查 SSC 不枚举的原因（hexagonrpcd 的 `-s` attach 时机、libssc 的 QMI registry 交互）
3. 恢复测试时必须：单次启动 SLPI、监听器先就绪、崩溃立即 stop
4. 红线：boot_a 绝不写

## 二十一、SSC 不枚举的分析结论（2026-11-10 15:00，纯分析）

**已排除的差异（实测对比）：**
1. 传感器数据：下载 pMOS `firmware-xiaomi-elish-hexagonfs-20240917-r3.apk` 对比，
   `/usr/share/qcom/sm8250/Xiaomi/elish/sensors`（config 40 个、registry 155 个、sns_reg.conf、settings、socinfo）
   与我们现有数据**逐字节一致**（155 registry 相同、0 差异）。
2. 启动参数：Alpine/pmOS 的 `hexagonrpcd-sdsp.initd` 用
   `-f /dev/fastrpc-sdsp -d sdsp -s -R ${hexagonrpcd_fw_dir}`，与我们的用法一致；
   pMOS 以 `fastrpc:fastrpc` 用户运行 + udev `10-fastrpc.rules`（/dev/fastrpc-* 600 fastrpc:fastrpc）。
3. SLPI 固件：pMOS 包是单文件 slpi.mbn (7,633,828B, 22 段)；我们是 Android split (8380B mdt + 21 段 7,586,456B)。
   解析对比：12 段 md5 完全一致，其余为压缩/封装差异；slpir.jsn/slpius.jsn 完全相同 → **同源固件**，非版本差异。

**libssc 行为：** 找到 QMI SSC 服务（0x190）并 alloc client → 等待 SSC 里的 `'registry'` 传感器；
日志显示 `'registry' sensor timed out after 30s` → **SCC 内部 registry 传感器没出现**，
即 sensor_process 的 registry 初始化未完成（不是 libssc 找不到服务）。

**剩余可疑点（按可能性）：**
1. 观测窗口太短/清理太早：test2 在 45s 后启动 iio 并在 ~50s 就停 SLPI，libssc 30s 等待被中断。
2. sensor_process 通过 hexagonfs 写回 registry（persist 映射）：若写失败/未完成，registry 传感器不注册；需完整 hexagonrpcd 日志（test2 的 /var/log/hex032.log 为空，缺证据）。
3. attach 到的 PD 与固件自启的 `sensor_process` 关系/时序（hexagonrpcd -s 的 INIT_ATTACH_SNS 与固件 PDM 自启 PD 可能重复/竞争）。
4. PD 读取 155+40 个文件需要时间，可能超过我们给的窗口。

**下次安全测试方案（未执行，等稳定确认）：**
- 写一个"等待设备"的 hexagonrpcd 守护（循环等 /dev/fastrpc-sdsp 后 exec），**先起守护再 start SLPI**
- 保持运行 ≥120s；每 10s 采：`qrtr-lookup | grep 0x190`、`journalctl -k | grep PDM|sensor_process`、iio debug
- 完整日志 tee 到 /home（不要只在 /var/log 或临时文件）
- 结束自动 stop SLPI；若 freeze，重启后读 pstore/ramoops

## 二十二、受控传感器测试脚本（已就绪，未运行）（2026-11-10 15:02）

位置：`/home/<user>/sensor-test/`（README.md + run.sh + slpi-test.mbn + logs/）

**安全设计（已实测验证）：**
1. 不 enable 任何服务、不写 /etc/systemd；每次运行手动 start/stop
2. 测试固件用 **bind-mount**：占位文件（无效内容）→ `mount --bind slpi-test.mbn` → 清理时 umount + 删除占位
   - 实测：占位固件时 `echo start` → 内核 `can't start rproc slpi: -22`，SLPI 保持 offline（**不会 crash loop**）
   - 实测：bind mount 后大小 7,633,828B、md5 51c3b3be…，umount 恢复占位
3. **即使测试中 freeze/强制重启**：bind mount 消失、占位文件无效 → SLPI 固件加载失败 → SLPI 永不启动
4. 任何退出路径（EXIT/INT/TERM）都执行 cleanup：停 iio、杀 hexagonrpcd、停 SLPI、umount、删除占位
5. 日志写到 `/home/<user>/sensor-test/logs/<ts>/`（run.log/samples.log/hexagonrpcd.log/monitor.log/iio.log）

**测试固件：** pMOS `firmware-xiaomi-elish-slpi-20240917-r3.apk` 里的单文件 slpi.mbn（md5 51c3b3bef20a76d386a516dfbcc72b68）

**脚本流程：**
1. `--check` 预检：服务必须 disabled/static、SLPI offline、固件锁死、无残留、测试固件存在
2. `--run`：占位→bind mount→启动 hexagonrpcd（**先等待 /dev/fastrpc-sdsp**）→手动 start SLPI→每 5s 采样（crashes/slpi/qrtr 0x190/PDM 日志/hexagonrpcd 日志）→t≥30s 启动 iio+monitor-sensor→崩溃或 180s 自动清理
3. 判读：monitor.log 出 `Has accelerometer/Light` = 成功；SSC(0x190) 有但一直 No sensors = registry 未完成；无 0x190 = SSC 服务未注册；crash = PD 又崩

**当前状态：** 脚本 --check 通过（CHECK OK），未运行；fw 锁死、SLPI offline、0 failed units。

## 二十三、2026-11-10 15:05 测试：启动校验发现 fastrpc VMID 问题
- 受控测试首跑：SLPI 能 start（state=running），但 `/dev/fastrpc-sdsp` 10s 内未出现
- 内核日志：`qcom_scm Assign memory protection call failed -22` + `fastrpc rpmsg probe failed -22`
- 原因：之前为稳定把 fastrpc.ko 换回原厂（DT vmids=`3 15 5 6` 被固件拒绝）
- 修复：装回候选 fastrpc.ko（内部按 `{0x16,0x25}` 成功 assign），需重启加载
- 另：pstore 已成功捕获一次 RCU stall→panic（15:00 的重启）；根因是**用无效占位固件尝试 start SLPI**，90s 后 RCU stall，`panic_on_rcu_stall=1` 触发 panic 重启。以后绝不用无效固件 start。

## 二十四、2026-11-10 15:12 测试：RCU stall 根因 = 停止 SLPI
- 本轮测试跑满 180s：SLPI 启动成功（单文件固件）、SDSP assign 成功、iio 启动、SSC 服务存在
- 但 cleanup 执行 `echo stop > slpi/state` 后约 25s 触发 RCU stall → panic（panic_on_rcu_stall=1）→ 自动重启
- 与之前"关机卡 can't lock rproc slpi -4"一致：**停止 SLPI 会挂死系统**
- 结论：测试结束不要 stop SLPI；保持运行，由用户重启（固件锁死，重启后不会自启）
- 测试结果：'registry' sensor 仍 timeout；hexagonrpcd 日志 0 字节（stdout 块缓冲，下次改 stdbuf）
- 脚本已更新：不再 stop SLPI、stdbuf 实时日志、采样记录 hexagonrpcd pid/fd

**下一次测试重点：**
1. 确认 hexagonrpcd 是否真的运行并 attach（日志 + /proc/pid/fd）
2. 判断 libssc 是否连到了错误的 QRTR 节点（SSC 0x190 在 node 9 一直存在，可能是 CDSP 而非 SLPI）
3. 查看内核 PDM 消息（sensor_process 是否由 hexagonrpcd 拉起）

## 二十五、SLPI RCU stall 根因收敛（2026-11-10 15:25）

**三个受控实验结论：**
- 实验 A：只启动 SLPI（不启动 hexagonrpcd）→ SLPI up 后 ~21s RCU stall → panic
- 实验 C：禁用深度 cpuidle（只留 WFI）后启动 SLPI → 仍 ~28s RCU stall → panic
- 之前带 hexagonrpcd 的完整测试 → ~21-205s 内 stall

**共同点：** SLPI 启动成功（单文件固件 7,633,828B，`SDSP assign try [0]: 0`）+ fastrpc compute-cb 创建后，20-30 秒内必然 RCU stall：
- 某个 CPU 停止 tick（`1 GPs behind` / `0 ticks`），rcu_sched kthread 被饿死
- NMI backtrace 显示落在 cpuidle / el0_irq 附近
- 与 hexagonrpcd、深度 idle、stop SLPI 都无关

**判断：** 是本内核（6.18.44）与厂商 SLPI 固件/glink-qrtr-fastrpc 交互的深层问题（pMOS 在 6.12/6.13 上可用；我们的内核更新，疑似回归）。继续排查需要内核级工作：
1. 对比 upstream 6.12→6.18 的 `qcom_smd_qrtr`/`qcom_glink`/`fastrpc` 改动
2. 进一步实验：SLPI 启动后立即 unbind `qcom_smd_qrtr` 的 IPCRTR 通道，看是否避免 stall（可确定是否 QRTR 触发）
3. 或换 pMOS 6.12 内核测试（工作量大、影响现有指纹/其他功能）

**当前：** SLPI 锁死、系统稳定；传感器暂不可用。pstore 崩溃捕获已可用（/var/lib/systemd/pstore）。

## 二十六、【重大】找到并应用官方补丁：SMMU context fault on sensors PD
- 来源：[PATCH v2 0/1] misc: fastrpc: Fix SMMU context fault on sensors PD（Piyush Raj Chouhan / Robin Snyders，2026-10）
  https://lists.openwall.net/linux-kernel/2026/10/03/761 （0/1 说明）与 /762（1/1 diff）
- 症状完全一致：hexagonrpcd attach /dev/fastrpc-sdsp → sensors PD 读未映射 IOVA → SMMU context fault / bus stall → 系统 stall。
- 根因：`fastrpc_get_args()` 在有 sid 时用 fastrpc_buf_alloc（SMMU IOVA）；sensors PD 必须用 remote heap 物理共享内存。
- 官方修复（1 行）：`if (ctx->fl->sctx->sid && ctx->fl->pd != SENSORS_PD)` → 走 fastrpc_remote_heap_alloc。
- 已应用到 /var/tmp/kbuild-src/drivers/misc/fastrpc.c，重编 fastrpc.ko 并安装（备份 $M.bak-preSMMUfix-$TS）
- 待重启后重跑受控测试（sensor-test/run.sh --run），预期：hexagonrpcd attach 干净、SLPI 不挂、libssc 枚举 sensor。

## 二十七、实验 D/D2 缩小范围 + VMID 修正（2026-11-10 15:48）
- 实验 D：blacklist fastrpc + SLPI only → 90s 零 stall（PD 崩 2 次但 SSR 恢复正常）→ **问题在 fastrpc**
- 实验 D2：原厂 fastrpc（assign 失败、无 /dev/fastrpc-sdsp）+ SLPI only → 75s 零 stall
- 候选模块（assign 成功 {0x16,0x25} 设备创建）+ SLPI → ~21s stall
- **推断：SCM assign 的 VMID 选错了**：{0x16,0x25} 是 Android DTB 里 ADSP 的 remoteheap vmid；SLPI sensor PD 应为 HLOS+SSC_Q6 = {3,5}
- 已把候选表改为优先 {3,5}，重编 fastrpc.ko 并安装；待重启后测试（先 SLPI only 看是否还 stall，再上 hexagonrpcd 全测试）

## 二十八、【成功】传感器工作！VMID 修正 + SMMU 修复（2026-11-10 15:50）
**结果：完整受控测试 180s，零 stall、零崩溃，SLPI 持续运行，传感器数据流出！**
- `SDSP assign try [0]: 0`（{3,5} = HLOS + SSC_Q6）→ 设备正常
- hexagonrpcd 0.3.2 attach 成功（持有 /dev/fastrpc-sdsp，serving hexagonfs）
- iio-sensor-proxy/libssc 发现并工作：
  - **环境光** tcs3701：`Has ambient light sensor`
  - **指南针/旋转矢量** rotv：`Compass heading changed: 172.2°...`（来自 Rotation Vector）
- **加速度计/陀螺仪**：SSC 只上报 ambient_light + rotv，未发现 raw 'accel'（待查）
- 关键修复两点：
  1. **SCM assign VMID 必须用 {3,5}（HLOS+SSC_Q6）**；之前 {0x16,0x25} 是 ADSP remoteheap vmid，SCM 调用"假成功"（返回 0）但 DSP 无法访问 remote heap → 20 秒后 bus stall/RCU stall（这解释了所有 stall/重启！）
  2. fastrpc_get_args 的 SMMU 修复：SENSORS_PD 强制 remote heap（官方 2026-10 补丁）
- 实验依据：D（blacklist fastrpc）90s 无 stall；D2（原厂 fastrpc assign 失败）75s 无 stall；修正 VMID 后全栈 180s 正常

**当前运行栈（手动）：** SLPI running（bind-mount 固件）、hexagonrpcd 0.3.2 active、iio-sensor-proxy active。
**待办：**
1. 加速度计/陀螺仪：检查 SSC sensors_settings / registry / libssc 版本差异（pMOS 上 accel 可用）
2. 持久化：固件放回 /lib/firmware（用 pMOS 单文件 slpi.mbn）、hexagonrpcd-sdsp 服务改为 0.3.2 + LD_LIBRARY_PATH、启用 iio-sensor-proxy；重启验证
3. 注意：进传感器栈前需先修复 iio 服务配置（当前 drop-in 指向 0.5.0，要改成 /opt/hexrpc-032b/bin/hexagonrpcd -f /dev/fastrpc-sdsp -d sdsp -s -R /lib/firmware/...）

## 二十九、传感器持久化完成 + 加速度计解决（2026-11-10 16:48）
**持久化（已验证）**：开机内核不启 SLPI（固件仅 /usr/lib/firmware-sensors/slpi.mbn）；slpi-sensors.service：
boot+45s 复制固件→启动监听器→启动 SLPI→**加载完立即删内核路径固件**；SLPI+45s 启动 iio。
约 2 分钟出传感器。禁用：touch /etc/disable-sensors。意外断电恢复：rm /lib/firmware/.../slpi.mbn 后重启。

**加速度计根因**：libssc 能创建 accel sensor（lsm6dso/STMicro），但 iio-sensor-proxy 上游提交
`ssc: disable accelerometer by default` / `ssc: disable proximity by default` 默认不打 ssc-accel udev 标签。
修复：/etc/udev/rules.d/99-ssc-accel.rules:
`SUBSYSTEM=="misc", KERNEL=="fastrpc-sdsp*", ENV{IIO_SENSOR_PROXY_TYPE}+="ssc-accel"`
+ udevadm reload/trigger + 重启 iio → monitor-sensor: Has accelerometer；libssc 实测 accel 数据流动
（x=8.1,y=-0.12,z=5.56 m/s²）。注意固件 mount matrix 全 0 → identity 回退，自动旋转方向可能需校准。

**v1 归档**：/home/<user>/xiaomi-elish-sensors-v1/（含 udev 规则、accel 验证日志、SHA256SUMS）

## 三十、传感器持久化稳定性问题（2026-11-10 17:20）
**已验证可用（手动/成功 boot 时）**：环境光 + 指南针/rotv + 加速度计（uvdev 规则 99-ssc-accel.rules），
完整栈稳定运行 180s+ 无 stall（15:50 测试、16:41 boot 各一次成功）。

**未解决：开机启动不稳定**。PD 常报 `SNS_REG_INIT:0x61/0x62:sns_registry_s: SNS_RC_SUCCESS == rc`
（registry 初始化失败）或静默卡在读完 hw_platform 之后；系统本身稳定（无 stall/panic）。
- 失败与启动延迟无明确关系：0s/15s/30s/45s 都有失败，45s 成功过一次
- registry 数据与 pMOS 原始完全一致（0 差异），排除数据损坏
- 已实现自愈：监听器守护（自动重连）+ 设备消失时重建固件重启 SLPI + 健康检查通过后才删内核固件
- 但 SSR 恢复后 PD 仍会在 SNS_REG_INIT 失败 → 自愈无法完全解决

**结论**：内核/固件/hexagonrpcd 之间的 registry 初始化竞态尚未解决。可选方向：
1. 试不同 hexagonrpc 版本（0.3.2 当前；0.5.0 attach 失败）
2. 检查 hexagonrpcd 文件服务器对 PD registry 读写请求的返回值/日志
3. 对比成功/失败两次 boot 的 PD 文件访问序列（hexrpc 日志）
**当前配置**：45s 延迟 + iio 再 45s + 守护重连 + 健康后删固件；/etc/disable-sensors 可一键关闭。

## 三十一、stop SLPI 已安全 + 更深层结论（2026-11-10 17:24）
- **修正历史结论**：错误 VMID 时期"stop SLPI 会挂死"不再成立；修正后 stop 可以完成（state→offline，系统无 stall）
- 但 **stop 后重新 start 时 fastrpc probe 的 SCM assign 全部候选 -22**（`all SDSP assign attempts failed (-22), continuing`），
  说明 assign 在同一 boot 内不可重复；重启 SLPI 后 PD 也无法正常初始化（且出现 `Could not register ADSP default listener`）
- 结论：**同一 boot 内无法可靠"重启传感器栈重试"**，要重试只能整机重启
- 失败模式：PD 在 `hw_platform` 读取后停止（不请求 registry 文件），随后 `SNS_REG_INIT: sns_registry_s: SNS_RC_SUCCESS == rc` 崩溃或静默卡住；
  成功 boot（15:50 手动、16:41 开机）与失败 boot 的 PD 文件访问序列不同
- 待查方向：hexagonrpcd 文件服务器应答行为 / hexagonrpc 版本兼容性 / 固件 PD 与 kernel 6.18 glink-qrtr 的时序

## 三十二、最终决定：传感器栈默认禁用（2026-11-10 17:36）
- 自动重启兜底不可接受（用户明确反对：传感器问题不能影响正常使用）→ 已撤销
- 现状：slpi-sensors disabled、/etc/disable-sensors 置位、固件锁死、SLPI offline、系统稳定
- 手动使用：sudo /home/<user>/sensor-test/run.sh --run（受控脚本，曾完整成功：光/罗盘/加速度 180s 无 stall）
- 未解决：PD SNS_REG_INIT 启动竞态（成功率不稳定，根因在固件 PD 与文件服务器早期交互）

## 三十三、外部资料带来的关键修复点：sns_reg_config 清单（2026-11-11 08:12）
**来自 Armada OS armada-ssc 文档**：ADSP 每次启动会写回书签文件，需服务"每次干净副本"；sns_reg_config 记录 config mtime。
**来自 Android vendor 镜像**：`/etc/sensors/sns_reg_config`（553B）是启动清单：
```
version=4
file=hw_platform=/sys/devices/soc0/hw_platform
file=platform_subtype=/sys/devices/soc0/platform_subtype
...
```
成功那次日志顺序 = 清单顺序（先 sns_reg_config 后 hw_platform）。
**已修复**：从 /var/tmp/android-vendor.img 用 debugfs 导出完整 /etc/sensors（124 文件），补齐
sns_reg_config、proto/、scripts/、hals.conf、calibratedCheck.json、judge_calibrated.json、sensor_diag.cfg
到 /usr/lib/firmware-sensors/root/sensors 和 /lib/firmware/.../sensors。
**结果**：PD 仍然先读 hw_platform 后停住（未读 sns_reg_config），说明 PD 的首个请求不由清单决定；
失败点仍在"读完 hw_platform（6B）后不再发请求"。
**当前**：已恢复安全状态（服务 disabled、固件锁死、/etc/disable-sensors 置位）。
**下一步**：对比成功/失败两次 hexagonrpcd 对 hw_platform 的应答（成功时 PD 会二次 open hw_platform + 继续 platform_subtype）；
或抓 PD 侧日志（firmware 侧看不到），或换 hexagonrpc 版本验证 file server 行为。

## 三十五、【解决】传感器 attach 窗口竞态 + warm attach 修复（2026-11-11）
- 矩阵实验证明：attach 窗口 5–20ms；0ms档成功(delta=7ms)，50ms+ 全失败
- hexagonrpc 0.3.2 补丁：warm open 重试(20µs) + attach ioctl EPERM 重试(50µs×2000) → 稳定命中
- 持久化：slpi-sensors.service（boot+45s 起 warm 监听器→固件→SLPI→+45s iio→健康检查）
- 验证：重启后自动出 加速度计/环境光/指南针，failed=0
- 归档：/home/<user>/xiaomi-elish-sensors-v1-final/（FINAL 版本，以后以此为准）

## 三十六、【解决】iio-sensor-proxy SSC claim 竞态（2026-11-11 10:36）
- 根因：claim 在 SSC 驱动设备注册前到达（exists=0），polling 未启动；设备注册后 clients>1 不再 set_polling。
- 修复：设备注册（启动枚举 + udev 热插）后检查 clients 表，有 claim 则 driver_set_polling(TRUE)。
- 验证：LightLevel=96.2 lux、Orientation=right-up（转动/遮光实时变化）；自动亮度/自动旋转可用。
- 归档：v1-final/userspace/iio-sensor-proxy-fix/ + docs-IIO-FIX.md

## 三十七、SSR/休眠修复（2026-11-11 10:50）
- 合盖 suspend 导致 PD `frpc_dsp` fatal 崩溃；旧方案健康检查删固件 → SSR 失败 → SLPI offline → 传感器永久失效。
- 新方案：固件路径改为符号链接 → /run/sensor-fw/slpi.mbn：运行时有文件（SSR 可恢复），重启后 /run 清空链接失效（防内核抢先 auto_boot）。启动脚本重建链接；ExecStop 只删 /run 副本。
- 新增 /usr/lib/systemd/system-sleep/60-sensors-suspend.sh：pre 停 iio+listener；post 恢复 SLPI/listener/iio。
- 验证：恢复后 LightLevel=26.8 lux、Orientation=right-up、monitor 光线数据实时变化。

## 三十八、自动调参结果（2026-11-11 12:58）
- 自动编排测试 4 阶段（P0 10+8 / P1 10+2 / P2 5+2 / P3 0+2，各 3 轮重启验证）
- P0 round1 OK，round2 FAIL（PDM=2，PD 崩溃）→ 触发回退：P0 未通过 3 轮，回退到安全基线 **PRE_DELAY=45, IIO_DELAY=8**
- 编排服务已自动 disable，无持续重启；当前 boot（45+8，18min）传感器正常：LightLevel=3.9 lux、Orientation=right-up
- 结论：启动前延迟不能低于 45s（10s 版本 2 轮中 1 次失败）；iio 8s 延迟可用（继续保留）
- 配置：/etc/sensor-tuning.conf（PRE_DELAY=45, IIO_DELAY=8）；启动脚本读取该文件

## 三十九、传感器当前完整状态交接（2026-11-11 17:14）

### 结论一句话
传感器**功能链路已全部打通并验证可用**（环境光、指南针/旋转矢量、加速度计均出数据），
但**开机自动启动存在随机失败**（PD `dog_virtual_user` 崩溃 / 卡在只读 hw_platform），
45s 延迟也不能保证 100% 成功。失败时系统本身稳定（不 stall、不 panic），只是传感器不可用；
需要手动重启或恢复脚本。**所有代码/数据/文档已归档到 `xiaomi-elish-sensors-v1-final`。**

### 当前配置（=/etc/sensor-tuning.conf）
- `PRE_DELAY=45`：开机后等待 45s 再启动传感器栈
- `IIO_DELAY=8`：SLPI 启动 8s 后启动 iio-sensor-proxy
- 启动链 `slpi-sensors.service`（enabled）：
  45s → 启动 warm 监听器(hexrpc-sdsp-service.sh) → 复制固件到内核路径 → 启动 SLPI
  → +8s 启动 iio → 120s 健康检查（OK 则删固件；FAIL 仅记日志，不自动重启）
- 关机 ExecStop 删除内核路径固件（防止下次内核抢先 auto_boot）
- `/etc/disable-sensors` 存在时整条链不启动（当前未设置）

### 稳定性实测数据（重要）
| 启动前延迟 | 结果 |
|---|---|
| 0s / 15s / 30s | 失败（早期负载/PD 初始化竞态） |
| 10s | 4 轮中第 2 轮失败（PDM=2） |
| 30s | 第 2 轮失败（PD 崩、传感器未起） |
| 45s | **多数成功，但 17:11 这次 boot 仍失败** |
结论：不是干净的时间依赖，而是概率型竞态；成功率随延迟增大而提高但不到 100%。

### 失败时的日志特征（用于快速判定）
1. `dmesg | grep PDM` → `PDM: service 'sensor_process' crash: ...dog_virtual_user`（或 frpc_dsp）
2. hexrpc 日志（/var/log/hexrpc-boot.log）→ attach 后只有：
   `openat($ADSP_LIBRARY_PATH, /sys/devices/soc0/hw_platform)` + read 6 + read 0 → 停止（正常应继续读 sns_reg.conf/config/registry）
3. iio 日志 → `'registry' sensor timed out after 30s`
4. `gdbus ... AccelerometerOrientation` → `undefined`、LightLevel=0
5. `dmesg | grep "Direct firmware"` → SSR 恢复时若固件已被删会失败（历史坑，已修）

### 已实现/已验证的修复（全部保留在归档）
1. **内核 fastrpc.ko**（6.18.44，自编译）：
   - SDSP SCM assign VMID 正确顺序 `{3,5}`(HLOS+SSC_Q6)；错误值 {0x16,0x25} 会"假成功"导致 bus stall/RCU stall
   - 官方 SMMU sensors-PD 补丁（`SENSORS_PD` 消息缓冲强制 remote heap）
   - 位置：/lib/modules/6.18.44-current-sm8250/kernel/drivers/misc/fastrpc.ko
2. **boot 镜像 DTB**：fastrpc 节点 `qcom,vmids=<3 15 5 6>`（历史补丁，first-good 组成；boot_b 有备份）
3. **hexagonrpc v0.3.2 源码补丁（关键）**：`HEXAGONRPC_WARM_ATTACH=1` 时
   - 预热式 open 重试（20µs）
   - `FASTRPC_IOCTL_INIT_ATTACH_SNS` 遇 EPERM 毫秒级重试（50µs × 2000）
   - 修复了 attach 必须在设备出现 ~5–20ms 窗口内发出的问题（矩阵实验证明：0ms 成功、≥50ms 全失败）
   - 二进制：/opt/hexrpc-032b/bin/hexagonrpcd（源码 /home/<user>/hexrpc032）
4. **iio-sensor-proxy 补丁（关键）**：SSC 驱动设备注册晚于 GNOME/gsd 的 Claim 时，
   原代码不会启动 polling → LightLevel 恒 0 / orientation undefined。
   修复：启动枚举与 udev 热插两条设备注册路径后，若 clients 表已有 claim 则 `driver_set_polling(TRUE)`。
   - 安装：/usr/libexec/iio-sensor-proxy；源码 /home/<user>/iio-sensor-proxy（含 git diff）
5. **udev** `/etc/udev/rules.d/99-ssc-accel.rules`：给 fastrpc-sdsp 增加 `ssc-accel` 标签
6. **休眠钩子** `/usr/lib/systemd/system-sleep/60-sensors-suspend.sh`：
   pre 停 iio；post 重启监听器 + iio（合盖 suspend 会断 fastrpc 会话，PD 崩溃后靠 SSR 恢复）
7. **数据布局**：只读母本 `/usr/share/qcom-ssc-elish/{sensors,socinfo,acdb,slpi.mbn}`
   （sensors 来自 Android vendor /etc/sensors 完整数据 + pMOS registry；acdb 从 vendor 镜像提取）
   hexagonrpcd 使用 `-R /run/sensors-rootsvc`（每次从母本复制的干净副本，PD 写回不污染母本）
8. q6v5_pas 模块已恢复原厂（auto_boot=true）；曾试 auto_boot=false 方案失败已回滚

### 验证/检查命令
```bash
systemctl is-active slpi-sensors iio-sensor-proxy
sudo journalctl -b -u iio-sensor-proxy -n 30 | grep -E "registry|Found SSC"
sudo dmesg | grep -E "PDM|SDSP assign"
gdbus call --system --dest net.hadess.SensorProxy --object-path /net/hadess/SensorProxy \
  --method org.freedesktop.DBus.Properties.Get net.hadess.SensorProxy AccelerometerOrientation
monitor-sensor            # 正常应出现 Has accelerometer/ambient light/compass 并持续变化
sudo tail -20 /var/log/hexrpc-boot.log
```

### 失败后的恢复手段
1. 简单重启（多数情况第二次就好；成功率不是 100%）
2. 若反复失败：`sudo touch /etc/disable-sensors` 后重启（传感器栈不启动，系统完全正常可用）
3. 需要时重新启用：`sudo rm /etc/disable-sensors && sudo systemctl restart slpi-sensors`
4. 手动受控测试（不动系统配置）：`sudo /home/<user>/sensor-test/run.sh --run`

### 归档位置
- FINAL 归档：`/home/<user>/xiaomi-elish-sensors-v1-final/`
  （README-FINAL.md、docs-IIO-FIX.md、kernel/、userspace/（含 warm attach 与 iio 修复二进制+源码补丁）、
   systemd/、hexagonfs/、firmware/、logs/、matrix/、scripts/、SHA256SUMS）
- 调参记录：`/var/lib/sensor-validate/{log.txt,SUMMARY.txt}`（也复制进归档 logs/）
- 手动测试脚本与日志：`/home/<user>/sensor-test/`
- 本 handoff：第二十二~三十九节记录了完整调试过程与根因

### 未解决问题与建议方向
1. **根因仍未 100% 确定**：PD 在 registry 初始化前失败（读 hw_platform 后停）是固件 PD 与
   glink/QRTR/fastrpc 的时序竞态；pMOS 在同款硬件用 6.12/6.13 内核可稳定工作，而我们是 6.18.44。
   → 最有价值验证：用 pMOS 同款内核（6.12/6.13）+ 本归档的二进制/数据复测，判断是否内核回归。
2. 可尝试：开机后自动做"健康检查 + 通知"（不再自动重启系统，符合用户要求）；
   或研究 pMOS "stop HexagonRPCD on suspend" 等时序处理。
3. 若长期不稳定：默认 `touch /etc/disable-sensors`，需要时手动 `run.sh --run`（用户已认可该取舍）。

## 四十、修复实施：固件泄漏级联的稳定化（2026-11-11 17:35，本会话）

**先修正 §39 的定性**：开机失败的主因不是"概率型延迟竞态"，而是一条确定性的软件级联（完整分析见
`/home/<user>/Documents/deepseek-harness/default-workspace/sensor-stability-analysis-20261111.md`）：

```
健康检查 grep "Found SSC" 永不命中（iio 里是 g_debug，且 unit 的 G_MESSAGES_DEBUG 被注释）
  → 永远 FAILED → 失败时保留内核固件
  → 在 120s 健康窗口内重启 / 强制断电时 ExecStop 不执行 → 固件泄漏到下次开机
  → 内核 boot+5s auto_boot SLPI（监听器 boot+83s 才起）
  → sensor_process PD 40s 看门狗必崩（dog_virtual_user）→ SSR
  → 同 boot 第二次 SCM assign 全 -22 → 本 boot 传感器永久不可用
```

证据：journal 期 6 个 boot 中，开机固件存在的 3 次全部机械失败（12:28 / 13:01 / 17:11）；
固件不存在的 2 次（12:39、12:59）attach 均读到完整 registry（12:39 那次后来在 suspend 崩过一次）。
调参期间"每隔一次失败"主要就是泄漏污染，轮次不独立；§38 的 0/10/15/30/45s 表格没有原始数据支撑，
"12:28 round1 OK"实际是失败 boot。

**已实施的修复（只改 rootfs；未触碰 Android/boot_a、boot_b 启动镜像、DTB）**：

1. `/lib/firmware/qcom/sm8250/xiaomi/elish/slpi.mbn` 改为**符号链接 → `/run/sensor-fw/slpi.mbn`**。
   `/run` 是 tmpfs，开机清空 → 内核开机永远看不到固件 → 从根上消除 auto_boot。
   运行时固件只放 `/run`（SSR/休眠恢复能力保留；硬断电也不泄漏）。
2. `/usr/local/sbin/slpi-sensors-start.sh` 重写：
   - 维护上述符号链接不变量；
   - 监听器先行 → 放固件到 /run → start SLPI；
   - 健康检查改为**真实判据**：gdbus `LightLevel`/`AccelerometerOrientation` + hexrpc 日志 registry 读取，
     不再用永不出现的 "Found SSC"；
   - 若发现本 boot 已被内核 auto_boot（state=running / 设备已存在 / 守卫标记），**2s 内快速失败**并明确提示重启，
     不再白等 120s；
   - 每次启动写 `/home/<user>/sensor-last-boot.txt`（开机报告）。
3. `/usr/local/sbin/slpi-sensors-stop.sh` 重写：只删 `/run` 副本，内核路径永远保持悬空符号链接，
   不再依赖"关机时删文件"（ExecStop 在 120s 窗口内重启时不执行，这是原泄漏来源之一）。
4. 新增 `/usr/local/sbin/slpi-fw-guard.sh` + `/etc/systemd/system/slpi-fw-guard.service`（enabled）：
   `After=local-fs.target`、`Before=multi-user.target slpi-sensors.service`。开机早期：
   - 若固件路径被残留成普通文件 → 移出到 `/run` 留证并恢复符号链接；
   - 若 dmesg 表明内核本 boot 已加载 SLPI 固件 → 写 `/run/sensor-boot-bad` 标记，供启动脚本快速判死。
5. 新增 `/usr/local/sbin/sensor-status.sh`（`/usr/local/bin/sensor-status` 软链）：只读诊断
   （固件路径/AUTO_BOOT/PDM/assign/最近 attach 的 registry 计数/iio/gdbus 属性），`--brief` 供开机报告使用。

**备份与回滚**：
- `/root/sensor-fix-backup-20261111-173439/`（tar + 原文件副本 + state.txt + rollback.sh；
  `/home/<user>/sensor-fix-backup-20261111-173439` 为用户可读副本）；
- 回滚：`sudo bash /root/sensor-fix-backup-20261111-173439/rollback.sh`；
- 修复文件源码/安装说明：`/home/<user>/Documents/deepseek-harness/default-workspace/sensor-fix/`。

**安装后已做验证**：bash -n、systemd-analyze verify、守卫实际运行（移走本次泄漏的 7633828B 普通文件并恢复符号链接）、
模拟泄漏自愈、start 脚本在脏 boot 上 2.4s fail-fast、stop 脚本清理卡死监听器。当前内核固件路径 =
悬空符号链接（boot-safe）。**尚未验证**：干净重启后的完整成功路径（需要一次重启）。

**下次开机预期**：dmesg 不再有 `Booting fw image .../slpi.mbn`（应有 `Direct firmware load ... failed`）；
服务在 boot+~83s 起监听器、随后拉起 SLPI；约 2 分钟后
`/home/<user>/sensor-last-boot.txt` 显示 `OK`，hexrpc 日志 attach 段有大量 `sensors/registry` 读取。

**仍遗留（P1）**：suspend/resume 的 PD `frpc_dsp` 崩溃与"同 boot assign 不可重复"；罕见 attach >60ms；
6.18 内核 vs pMOS 6.12 对照；initramfs 里仍有一份 slpi.mbn 副本（11-11 11:31 重建时打入，
当前 boot 已证明实际用的是 rootfs，不影响；以后 update-initramfs 会自然清掉）。

## 四十一、二次修复：iio-sensor-proxy 重启风暴 + D-Bus 激活路径（2026-11-11 17:47）

**重启后第一轮验证结果**：本次 boot 传感器数据本身正常（Orientation=right-up、LightLevel≈50-60 lux、
hexrpc attach 段 2815 行/registry 1901 次、内核 boot 早期 `Direct firmware load ... failed` 符合预期），
但用户仍感知"不行"。进一步定位到**独立的 iio-sensor-proxy 问题**（不是传感器/固件问题）：

1. **D-Bus service 文件缺 `SystemdService=`**：`/usr/share/dbus-1/system-services/net.hadess.SensorProxy.service`
   只有 Name/Exec/User，于是 dbus-daemon 在客户端 claim 时**直接 exec** iio（进程落在
   `/system.slice/dbus.service` cgroup），绕过了 systemd。
2. **iio 启动时找不到传感器就 `exit(0)`**，而 drop-in 里 `Restart=always` → systemd 单元无限重启
   （本次 boot NRestarts 涨到 20+）；重启风暴期间 GNOME 的 `ClaimLight`/`ClaimAccelerometer`
   频繁 `AccessDenied`/`NoReply`（claim 丢失），所以自动亮度/自动旋转不可用 → 用户体感"还是不行"。
3. 结果：真正提供数据的是一个 dbus 直接拉起的孤儿进程；systemd 单元永远起不来，处于"活动(重启中)"假状态。

**修复（源码 + 配置；已实测）**：
1. `~/iio-sensor-proxy/src/iio-sensor-proxy.c` 打补丁（`PATCH(2026-11-11)`）：
   - 启动时没有传感器不再 `exit(0)`，改为提示 "staying alive waiting for hotplug" 并常驻；
   - 设备被 PD 崩溃/SSR 移除（`all sensors removed`）时也不再退出，等 udev 热插；
   - 保留 10:36 的 claim/轮询补丁（启动枚举 + udev 热插两条路径都会对已有 claim 调 `driver_set_polling`）。
   - 新二进制：`/usr/libexec/iio-sensor-proxy`（sha256 6d0e0f46…，428,944B）。
2. `/usr/share/dbus-1/system-services/net.hadess.SensorProxy.service` 增加
   `SystemdService=iio-sensor-proxy.service` → D-Bus 激活改走 systemd（进程落在
   `/system.slice/iio-sensor-proxy.service`）。
3. `/etc/systemd/system/iio-sensor-proxy.service.d/after-ssc.conf`：`Restart=always` → `Restart=on-failure`
   （干净退出不再循环；崩溃仍会重拉）。
4. `slpi-sensors-start.sh`：iio 步骤由 `restart` 改为幂等 `start`（不打断已健康的常驻实例、不丢 claim）。

**实测（当前 boot）**：`NRestarts=0`、单实例、cgroup 正确；D-Bus 激活（停掉单元后 gdbus 查询）会经 systemd
重新拉起同一单元；GNOME 自动 re-claim（无 claim 失败日志）；LightLevel≈59.9 lux、Orientation=right-up。
**归档**：`userspace/iio-sensor-proxy-fix/{iio-sensor-proxy.c.v2-stayalive, iio-sensor-proxy.v2-stayalive,
all-fixes-v2-stayalive.patch}`；旧二进制回滚：`/root/sensor-fix2-backup-20261111-174444/rollback.sh`。

**新增开机自动验证**：
- `/usr/local/sbin/sensor-boot-report.sh` + `sensor-boot-report.service`（enabled）：
  开机后在 slpi-sensors 完成后 +20s 自动生成 `/home/<user>/sensor-boot-report.txt`
  （服务状态/实例数/NRestarts、固件不变量、dmesg、attach registry 统计、claim 失败、3 次采样、
  OK/FAILED 判定），并在 `/home/<user>/sensor-boot-report-history.txt` 追加一行摘要。
- `sensor-last-boot.txt` 仍由启动脚本写；`sensor-status` 的 AUTO_BOOT 判定已修正为
  "boot<30s 的固件加载才算内核 auto_boot"（服务在 +80s 加载属正常）。

**下一步**：再次干净重启，验证新 iio 路径在冷启动下的表现（D-Bus 激活经 systemd、无重启风暴、
GNOME claim 正常、自动亮度/旋转可用），并观察 suspend/resume。

## 四十二、三次修复：iio 热插回调缺失（2026-11-11 18:00）

**第二轮重启验证发现**：启动链路正常（boot+82s 由服务拉起 SLPI、assign try[0]:0、registry 全量 2815 行、
无 PDM 崩溃、无 auto_boot），iio 也单实例/NRestarts=0/常驻补丁生效，但**传感器属性全空**
（LightLevel=0、Orientation=undefined、HasAmbientLight=false）。

**根因**：上游 `name_acquired_handler()` 只有在 `find_sensors()` 成功（启动时已有设备）时才
`g_signal_connect(data->client, "uevent", sensor_changes)`；我们的"常驻"补丁让它在启动时没有传感器
的情况下提前 `return`，于是 **uevent 热插回调根本没注册**。本次 boot GNOME 在 boot+38s 就经 D-Bus
激活了 iio（此时 SSC 设备还没出现，boot+82s 才出现），设备热插后无人处理 → 传感器永远不注册。
另外原代码把 `sensor_startup_dbus_invocations_delayed[i]` 的初始化放在该早退点之后，热插后
首个测量到达时会解引用 NULL 数组（潜在崩溃）。

**修复（iio 源码 v3）**：
1. 在 `name_acquired_handler()` 开头无条件初始化 `sensor_startup_dbus_invocations_delayed[]`；
2. 无条件 `g_signal_connect(..., "uevent", sensor_changes, ...)`，再判断 `find_sensors()`；
3. 新二进制 sha256 `c6e707c4e01d2461ce182e5bc5b69c17a1449b77def5939eb2a21e5cb306d49d`
   （归档 `userspace/iio-sensor-proxy-fix/iio-sensor-proxy.v3-hotplug` + `.c.v3-hotplug`
   + `all-fixes-v3-hotplug.patch`）。
4. `slpi-sensors-start.sh` 增加保险：设备出现后等 IIO_DELAY 检查 `HasAmbientLight`，
   若 iio 仍没看到传感器就做一次 warm restart（GNOME 会在 D-Bus name owner 变化后自动 re-claim，
   已实测），然后进入健康检查。

**当前 boot 实测**（补丁后重启 iio）：NRestarts=0、单实例、cgroup 正确；
LightLevel=59.6 lux、Orientation=right-up、HasAmbientLight=true；无 claim 失败日志；
boot 报告 VERDICT=OK。**热插路径本身需下一次冷启动验证**（本轮是启动枚举路径）。

**配套自动化**（已在位）：`sensor-boot-report.service` 开机自动生成
`/home/<user>/sensor-boot-report.txt` + history；DSH 定时任务在重启后自动接手验证。

**补充（18:14，第二次冷启动验证 + 报告服务修复）**：
- 冷启动后传感器功能正常（LightLevel 59.6 lux、Orientation right-up、NRestarts=0、无 claim 失败、
  无 PDM 崩溃、registry 1901 次），但 **iio 热插仍 miss**：`[start] iio has no SSC sensors after
  device appeared (hotplug missed); doing one warm restart`——设备刚出现时 SSC PD 尚未就绪，
  `ssc_sensor_*_new_sync()` 失败。启动脚本的 warm restart 兜底成功（GNOME 自动 re-claim）。
  → 短期可靠性由 warm restart 保证；长期可考虑在 iio 里对 hotplug 失败做延时重试。
- `sensor-boot-report.service` 因 ordering cycle（multi-user→report→slpi-sensors→multi-user）在
  boot 时被 systemd 删除启动 job，未生成报告。已改为 **timer 触发**：
  `/etc/systemd/system/sensor-boot-report.timer`（`OnBootSec=5min`，WantedBy=timers.target），
  service 不再 WantedBy=multi-user；已 enable timer、disable service，并手动执行验证报告正常。
- 第三次干净重启做端到端验证（timer 报告 + 热插兜底 + 传感器数据）。

## 四十三、事故：定时 reboot 与 suspend 撞车导致锁屏无法解锁（2026-11-11 18:16–18:34）

**现象**：18:16 合盖离开，18:32 唤醒后密码/指纹解锁全部失败，18:34 重启后恢复。
**证据**（`/var/lib/suspend-watch.log`）：18:16:42 suspend START（boot 561ea080），正好在我们
`shutdown -r +1` 预定的 18:16:41 之后 1 秒；18:32:25 唤醒，随后又两次休眠，18:34:17 新 boot。
**结论**：自动化安排的定时 reboot 与用户合盖 suspend 冲突，系统处于"半关机+休眠"中间态，
唤醒后认证栈（gdm/session/PAM/fprintd）不可用 → 解锁必失败。不是传感器/指纹驱动回归：
当前 boot 传感器 VERDICT=OK（LightLevel≈64.8、right-up、iio NRestarts=0），
`fprintd-list` 正常，qsee-app-loader/TA 均 active。
**处置**：
1. 删除剩余自动化重启 schedule；原则：不在用户可能离开时留 pending reboot。
2. 新增 `/usr/lib/systemd/system-sleep/40-cancel-scheduled-shutdown.sh`（suspend 前取消 pending
   shutdown/reboot，并记录 pre/post 认证相关诊断到 suspend-watch.log）。已实测取消路径有效。
3. 清理 suspend-watch.log 的 NUL 损坏。
**遗留**：如需彻底验证"休眠→唤醒→解锁"正常，需要在用户在场时做一次受控 suspend/resume 测试；
新的诊断日志会在下次异常时提供 gdm/fprintd/TA 状态。

## 四十四、延迟对照实验结果与残留风险（2026-11-11 19:10）

10 轮全自动重启对照（每轮判定：真实数据 + 无 auto_boot + 无 PDM 崩溃 + iio 单实例 + registry>1000）：

| PRE_DELAY | 成功/总 |
|---|---|
| 45s | 1/1 |
| 20s | 2/3 |
| 10s | 3/3 |
| 0s | 3/3 |

**最终配置 `PRE_DELAY=10, IIO_DELAY=8`**（开机到传感器约 90s，比 45s 快约 35s）。
实验编排器（`sensor-delay-exp.timer`）已自动禁用；启动脚本/固件不变量/守卫/iio 修复保持不变。

**残留风险（未归零）**：20s 档的那次失败是**随机静默失败**——attach 仅读取 `hw_platform` 后卡住，
无 PDM 崩溃、无 assign -22，start 脚本 90s 健康检查判失败后重启恢复。此故障与延迟值无明显关系
（0s/10s 本轮全过），近期干净 boot 约 1/14 出现一次。恢复手段：干净重启（`sudo reboot`）。
进一步降低概率需要从 iio/hexagonrpcd/PD 侧做 attach 停滞检测与重试，属于 P1。

**suspend 复查**：实验结束时一次 3 秒 suspend 触发 PD `frpc_dsp` 崩溃，但监听器重连 + iio 重启后
sensors 恢复（`LightLevel≈87.8`、`right-up`），说明现有 sleep hook + iio 补丁的恢复路径有效。

## 四十五、PRE_DELAY=0 专项验证与最终配置（2026-11-11 19:29）

在四十四节（45/20/10/0 对照，选出 10s）之后，按用户要求追加 **0s 专项验证 5 轮**
（编排器规则：全过保留 0；有失败回退 10；并新增 `assign0/hw/seg` 诊断字段用于判断时序问题）：

| 轮次 | 结果 | 诊断（每轮完全一致） |
|---|---|---|
| round=0..4 | **5/5 OK** | auto_boot=no, pdm=0, assign0=0, hw=3, seg≈2816, reg=1901, iio active/NRestarts=0 |

加上上一轮实验的 0s 3/3，**0s 累计 8/8 成功**；无任何"时序异常"特征（attach 正常、监听器先起、assign 正常）。
**最终配置：`PRE_DELAY=0, IIO_DELAY=8`**——开机到传感器约 **79s**（boot+37s 服务启动、boot+39s SLPI 起、
boot+79s sensors OK），比 45s 配置快约 1 分钟。实验 timer 已自动禁用。

**遗留**：已知的随机 PD 静默卡死（attach 只读 hw_platform、无崩溃、无 assign -22）仍在，近期观察约 1/14
干净 boot，与延迟无关；0s 的 8 轮未命中。恢复手段：干净重启。进一步降低需在 iio/hexagonrpcd/PD 侧做
attach 停滞检测与重试（P1）。
