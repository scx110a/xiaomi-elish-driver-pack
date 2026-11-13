# 补丁与配置修改清单

本包所有代码修改都以补丁形式提供（**不含编译产物**）。内核 1 个 + 用户态 6 个（hexrpc 5、iio 1）补丁均已在专属基准上做 `patch -p1` / `git apply` 校验（见 §5）。

## 1. 内核：fastrpc.c（2 个 hunk）

- 文件：`../kernel/patches/0001-fastrpc-sdsp-vmid-and-sensors-pd.patch`
- 基准：Linux **v6.18.44** pristine（`/var/tmp/linux-6.18.44.tar.xz` 内 `drivers/misc/fastrpc.c`）
- 应用：`cd <内核源码树> && patch -p1 < 0001-...patch`
- 修改后的完整文件也放在 `../kernel/patches/fastrpc.c.modified`（方便 diff/对照）。

### hunk 1 — 官方 SMMU sensors-PD 修复（`fastrpc_get_args()`）

```diff
-	if (ctx->fl->sctx->sid)
+	if (ctx->fl->sctx->sid && ctx->fl->pd != SENSORS_PD)
 		err = fastrpc_buf_alloc(...);
 	else
 		err = fastrpc_remote_heap_alloc(...);
```

来源：`[PATCH v2 0/1] misc: fastrpc: Fix SMMU context fault on sensors PD`（Piyush Raj Chouhan / Robin Snyders，2026-10）。
原因：sensors PD 不能用 SMMU IOVA 消息缓冲，必须 remote heap；否则 attach 时 SMMU context fault → bus stall。

### hunk 2 — SDSP SCM assign 的 VMID 候选（`fastrpc_rpmsg_probe()` SDSP 分支）

```c
static const u32 cand[][3] = {
    {3, 5, 0}, {3, 0, 5}, {5, 3, 0},
    {3, 0, 0}, {5, 0, 0}, {0x16, 0x25, 0},
};
/* 逐个 qcom_scm_assign_mem()，成功 break；
 * 失败 dev_warn("all SDSP assign attempts failed (%d), continuing") 并继续 probe */
```

正确值 `{3,5}` = HLOS(3) + SSC_Q6(5)。历史错误值 `{0x16,0x25}` 是 ADSP remoteheap vmid：
SCM 返回 0（"假成功"）但 SLPI sensor PD 无权限 → 20 秒后 bus stall / RCU stall → panic。
dmesg 期望：`SDSP assign try [0]: 0`。

> 备选/可选：boot 镜像 DTB 的 SLPI fastrpc 节点加 `qcom,vmids = <3 15 5 6>`（历史补丁）。
> 由于驱动内已有候选表，它不是唯一必需；见 `../kernel/dtb/README.md`。

## 2. hexagonrpc v0.3.2：warm-attach

- 文件：`userspace/hexrpc/patches/0001-hexrpc-0.3.2-warm-attach.patch`
- 基准：https://github.com/linux-msm/hexagonrpc tag `v0.3.2`（commit `b7eff23`）
- 应用：`git apply 0001-...patch`
- 修改 `hexagonrpcd/rpcd.c`：
  1. `HEXAGONRPC_WARM_ATTACH=1` 时 `open("/dev/fastrpc-sdsp")` 失败按 20µs 重试（最多 50000 次）；
  2. attach ioctl 遇 `EPERM` 时 50µs 重试（最多 2000 次 ≈ 100ms）；其它 errno 立即返回。
- 原因：attach 必须在设备出现后 5–20ms 窗口内；太早 EPERM、太晚 PD 初始化走错并崩溃。

## 2b. hexagonrpcd v0.3.2：M2 可写 sensor registry（2026-11-13 验证）

- 文件：`userspace/hexrpc/patches/0003-hexagonrpcd-add-writable-hexagonfs.patch`、
  `0004-hexagonrpcd-support-writable-sensor-registry.patch`、
  `0005-hexagonrpcd-support-large-and-extended-callbacks.patch`
- 来源：社区 Armada OS 打包（`packages/hexagonrpc/PATCHES.md`），原始补丁由 suckbluefrog
  （SteamOS-ARM-SM8550）与 gennro（kettlelinux）提供；0004 的 `rpcd.c`/`rpcd_builder.c` hunk
  已按本机 v0.3.2 base 手工等价适配（本机 `fastrpc_apps_std_init` 在 `start_reverse_tunnel()` 内）。
- 应用顺序（writable 构建）：`0001` → `0003` → `0004` → `0005`；**不要**再叠 `0002`
  （0004 的可写 `/persist/sensors` 树替代 0002 的只读 `sns_reg_version` 映射；0002 的 regver-v1 仅作回退）。
- 验证：干净 v0.3.2 上 4 个补丁 `git apply --check` 全通过；重建二进制与部署 `.text` 相同；
  上机 boot 2026-11-13 00:01：temp.json 写拒绝 1570→0、PD 初始化 8.2s、开机传感器 ~22s。

## 3. iio-sensor-proxy：三处修复（1 个 patch，4 个小 hunk）

- 文件：`userspace/iio-sensor-proxy/patches/0001-iio-sensor-proxy-hotplug-stayalive.patch`
- 基准：https://gitlab.freedesktop.org/hadess/iio-sensor-proxy commit `ca0d5b0`（3.9-1）
- 应用：`git apply 0001-...patch`
- 修改 `src/iio-sensor-proxy.c`：
  1. **claim→polling**：启动枚举/udev 热插注册设备后，如 clients 表已有 claim，调用 `driver_set_polling(TRUE)`；
  2. **常驻**：启动时无传感器、以及设备被移除时，不 `g_main_loop_quit()`，打印 staying alive 后继续运行；
  3. **无条件 uevent**：把 `g_signal_connect(client,"uevent",...)` 与
     `sensor_startup_dbus_invocations_delayed[]` 初始化提前到 `find_sensors()` 之前。
- 修复的症状：`LightLevel=0` / orientation undefined（claim 竞态）；iio 无限重启风暴；
  提前激活时热插永远不注册。

## 4. 配置类修改（不是源码补丁，已随包提供文件）

| 文件 | 作用 |
|---|---|
| `systemd/dbus/net.hadess.SensorProxy.service` | 增加 `SystemdService=iio-sensor-proxy.service`，让 D-Bus 激活走 systemd（否则绕过 systemd、重启风暴） |
| `systemd/iio-dropin/after-ssc.conf` | iio drop-in：`ExecStartPre=sleep 2`、`Restart=on-failure`、`TimeoutStopSec=3`（加速改造后不再 `After=multi-user.target`） |
| `systemd/udev/99-ssc-accel.rules` | 启用 SSC accelerometer（上游默认 disabled） |
| `systemd/udev/81-libssc-xiaomi-elish.rules` | 加速度计挂载矩阵 `-1,0,0;0,-1,0;0,0,-1`（自动旋转方向） |
| `systemd/udev/80-iio-sensor-proxy.rules` | 上游规则（自带 ssc-light/ssc-compass 与 fastrpc-sdsp 标签） |
| `systemd/sensor-tuning.conf` | `PRE_DELAY=0` / `IIO_DELAY=8`（实测值） |
| `systemd/system-sleep/60-sensors-suspend.sh` | suspend pre 停 iio / post 重拉监听器+iio |
| `systemd/system-sleep/40-cancel-scheduled-shutdown.sh` | suspend 前取消 pending shutdown（防"半关机+休眠"） |
| 启动链脚本 | 见 `scripts/` 与 `install/FILES-MAP.md` |

## 5. 补丁应用校验记录（本次打包时）

- `../kernel/patches/0001-...` 在 pristine v6.18.44 `drivers/misc/fastrpc.c` 上 `patch -p1 --dry-run` 通过；
- `userspace/hexrpc/patches/0001-...` 在 v0.3.2 的 `hexagonrpcd/rpcd.c` 上 `patch -p1 --dry-run` 通过；
- `userspace/iio-sensor-proxy/patches/0001-...` 在 `ca0d5b0` 的 `src/iio-sensor-proxy.c` 上通过；
- 全部补丁（含 hexrpc 0003/0004/0005）同时也在本机已打补丁的工作树上做 `git apply --check --reverse` 通过。
