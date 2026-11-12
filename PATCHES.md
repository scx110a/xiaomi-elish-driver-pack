# 补丁与配置修改总清单（合并包）

本包所有代码修改以补丁/源码文件形式提供；**不含编译产物、固件**。

## 一、内核（多项目共用一次构建，详见 `kernel/README.md`）

### 1. 传感器：fastrpc.c 两处（`kernel/patches/0001-...patch`）

- 基准：Linux v6.18.44 pristine `drivers/misc/fastrpc.c`
- hunk 1：`fastrpc_get_args()` 中 `if (ctx->fl->sctx->sid && ctx->fl->pd != SENSORS_PD)`
  → sensors PD 强制 remote heap，避免 SMMU context fault；
- hunk 2：`fastrpc_rpmsg_probe()` SDSP 分支增加 VMID 候选表
  `{3,5}` 优先（`SDSP assign try [%u]: %d`），全失败 `dev_warn` 继续。
- 完整修改版：`kernel/patches/fastrpc.c.modified`。

### 2. 指纹：内核文件（相对 6.18.44 + Armbian 补丁）

| 文件 | 修改 | 安装位置 |
|---|---|---|
| `kernel/qseecom/` | 新增 QSEECOM TEE 驱动（`core.c/Kconfig/Makefile`） | `drivers/tee/qseecom/`（含 Kconfig/Makefile 集成） |
| `kernel/mdt_loader.c` | 增加 ELF64 支持（`fingerpr` 用；`gfenu` 是 ELF32） | `drivers/soc/qcom/mdt_loader.c` |
| `kernel/qcom_scm.c` | 增加 `qcom_scm_qseecom_app_get_id/app_load/app_unload` 与响应解析 | `drivers/firmware/qcom/qcom_scm.c` |
| `kernel/goodix_fp_spi/` | Goodix GF3626 SPI 传感器驱动（out-of-tree 模块） | 编译为 `goodix_fp_spi.ko` |
| `kernel/config-used-6.18.44` | 实际编译配置（TEE/QSEECOM/MDT/BTF/FASTRPC 等） | 内核 `.config` |
| `kernel/ARMBIAN-BASELINE.md` | 如何获取 Armbian sm8250 基线（原归档补丁集不完整，未随包） |
| `kernel/install-new-kernel.sh` | 备份/重建 boot 镜像/写 boot_b/回读校验 | 手动执行 |

### 3. LPASS pinctrl：时钟超时改为 -EPROBE_DEFER（独立于传感器/指纹）

`kernel/patches/0002-pinctrl-lpass-lpi-defer-on-clock-timeout.patch`（`pinctrl-lpass-lpi.c`，1 hunk）：
`-ETIMEDOUT/-EAGAIN/-EBUSY` 返回 `-EPROBE_DEFER`，修 pogo 键盘/USB 随机失效。部署需
`update-initramfs` + 重建 boot 镜像；详见 `kernel/PINCTRL-LPASS.md`。

### 4. 内核集成与 initramfs hook（不是驱动补丁，但缺了会编译失败/破坏不变量）

- `kernel/include/` 3 个头文件 + `kernel/INTEGRATION.md`：qcom_scm.h / mdt_loader.h / tee.h，
  以及 drivers/tee Kconfig/Makefile 集成点、QCOM_MDT_LOADER 依赖；
- `kernel/initramfs/hooks/xiaomi-elish-firmware`：修复版 hook，跳过 `slpi.mbn` 符号链接/`.disabled`，
  防止真固件被打进 initramfs。

### 4a. QSEECOM 完整系列（权威补丁）

`kernel/patches/fingerprint/qseecom-tee-series.patch`（112,886B，14 文件）：QSEECOM TEE 驱动、
`drivers/tee`/`drivers/soc/qcom` Kconfig/Makefile 集成、`qcom_scm.c`/`mdt_loader.c`、
3 个头文件、Documentation/MAINTAINERS。从零重建时优先应用此补丁；包内也提供逐文件副本。

### 4b. QCOM_MDT_LOADER 用户可选（Kconfig）

`kernel/patches/0003-qcom-mdt-loader-user-selectable.patch`：pristine 6.18.44 的
`QCOM_MDT_LOADER` 是裸 `tristate`（无 prompt），会被 `QCOM_Q6V5_PAS=m` select 成 m，
而 `TEE_QSEECOM=y` 依赖它 → 必须改为用户可选并显式 `=y`。

### 5. DTB（非补丁，配置改动）

- 传感器：`qcom,vmids = <3 15 5 6>`（SLPI fastrpc 节点）；
- 指纹：`/fingerprint` 节点（`compatible="goodix,gf3626"`、`firmware-name="gfenu"`、
  `interrupts=<23 1>`、`reset-gpios=<&tlmm 125 GPIO_ACTIVE_LOW>`、L10A 3.008V、pinctrl IRQ）；
- GNSS（2026-11-12）：`/soc@0/geniqup@9c0000/i2c@994000` 改为 `qcom,geni-uart` + `status=okay`
  （`pinctrl-0` = `qup-uart5-default-state`，gpio12–15）；根 `/bcm4775`（`gps-ldo`、PMIC
  `gps-clk-default`、`gps-enable-active/suspend`）；
- 已验证联合 DTB 随包：`kernel/dtb/sm8250-xiaomi-elish-csot.dtb`（sha256 `d3c2f8b5…`）；
- 不要用当前树 DTS 重建（会丢 17 节点/109 属性）；dtc 往返破坏的是 **L10C（regulators-1/ldo10）**，
  不是 L10A。

## 二、用户态补丁

### 4. hexagonrpc v0.3.2 warm-attach（传感器）

`userspace/hexrpc/patches/0001-hexrpc-0.3.2-warm-attach.patch`（位于 `sensors/` 子包内）：
`HEXAGONRPC_WARM_ATTACH=1` 时 warm open 重试；attach `EPERM` 50µs 重试。

### 5. iio-sensor-proxy 三处（传感器）

`sensors/userspace/iio-sensor-proxy/patches/0001-iio-sensor-proxy-hotplug-stayalive.patch`：
claim→polling、无传感器常驻、无条件连接 uevent。

### 6. libfprint goodixqsee 日志补丁（指纹）

`fingerprint/src/libfprint/patches/libfprint-goodixqsee-logging.patch`：
`bring_up()`/worker 失败日志（TA 看门狗依赖）。

### 7. GNSS：新增代码（非补丁）

本机 AG3335 无主线驱动，GNSS 由本包新增的源码/配置实现（不是对现有驱动的 patch）：

- `gnss/kernel/airoha_gps_driver.c`：从 Android 下游移植的电源/时钟/pinctrl 控制模块；
- `gnss/userspace/airoha-gnssd.c`：用户态守护进程（上电 + `$PAIR002` + NMEA→pty + 自愈）；
- `gnss/systemd/`：`airoha-gnssd.service` + gpsd drop-in；`gnss/install/install.sh` 负责安装。

## 三、系统配置改动（随包文件，不是源码补丁）

| 类别 | 内容 |
|---|---|
| 传感器 | `sensors/systemd/`（slpi-sensors、slpi-fw-guard、sensor-boot-report、iio drop-in、udev ×3、休眠钩子、tuning、D-Bus） |
| 指纹 | `fingerprint/system/`（pam.d/sudo 指纹栈（可选安装）、qsee-supplicant/loader/看门狗 unit、fprintd drop-in、udev、弹窗脚本） |
| 内核集成 | `drivers/tee/Kconfig`/`Makefile` 集成点；`CONFIG_LOCALVERSION`/BTF 保持；`q6v5_pas auto_boot=true` 恢复 |
| GNSS | `gnss/systemd/`（airoha-gnssd.service、gpsd drop-in）、`/etc/default/gpsd`（DEVICES=/dev/ttyGNSS0）、`/etc/modules-load.d/airoha-gps.conf` |

## 四、应用校验

- 传感器 fastrpc 补丁：对 pristine v6.18.44 `patch -p1 --dry-run` 通过（打包时校验）。
- hexrpc/iio 补丁：对 v0.3.2 / `ca0d5b0` 通过。
- 指纹内核文件为整文件替换/新增，需按 `kernel/README.md` 放入源码树后编译验证（ABI 三件套）。
- GNSS：`kernel/dtb/verify-combined-dtb.sh`（GNSS 节点）+ `gnss/install/verify.sh`（设备/服务/NMEA/gpsd）；
  本机合并自检 `install/check-all.sh` 已包含。

---

## 五、相机（elish-camera，2026-11-12 晚新增）

相机管线不新增内核补丁，复用现有联合 DTB 与 OV8856/OV13B10/CN3927V/PM8008 驱动，用户态实现：

- `camera/elish-camera/`：C 源码（capture/gpu_debayer/venus_enc/loopback/main）、tools、docs；
- `camera/install.sh`：编译并安装 `/usr/local/bin/elish-camera`、`elish-camera-*` 辅助脚本、
  udev（VCM/DMA heap）、v4l2loopback 配置、桌面启动器/自启，以及 `camera/system/` 的
  休眠唤醒恢复（worker / sleep hook / oneshot service）；
- **udev 规则**：让 video 组可写 VCM 的 `/sys/bus/i2c/devices/20-000c/focus`（否则普通用户对焦写入静默失败）；
- **v4l2loopback 参数**：`camera/elish-vcam.modprobe.conf` 改为两个设备
  `/dev/video2`（前）+ `/dev/video20`（后），NV12 全分辨率；
- **重要注意（驱动侧）**：`kernel/patches/0005-ov8856-bggr-vflip.patch` 把 `V4L2_CID_VFLIP`
  默认值设为 1，会令前摄 3280×2464 **完全无帧**；elish-camera 每次打开都显式 `vflip=0`。
  长期应移除该 hack，改为 libcamera/sensor properties 方案；
- **Venus 设备号**：encoder/decoder 的 video 节点号不跨启动稳定（曾从 video16/video15 互换），
  elish-camera 按 sysfs 名 `qcom-venus-encoder` 发现，不要在脚本里硬编码；
- Android 取证结论见 `camera/elish-camera/docs/android-camera-facts.md`（三 sensor RAW10/分辨率/
  2-4 lane、OV13B10 EEPROM LSC 表、Hi556 上电顺序）。
