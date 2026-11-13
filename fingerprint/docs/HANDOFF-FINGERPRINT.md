# 指纹项目历史交接（指纹章节，去除传感器部分）

> 从完整交接文档抽取第 1–16 节（指纹移植）。完整时间线中传感器部分见 sensors/docs/HANDOFF-SENSORS.md。
> 用户路径/用户名已替换为 <user>。

# 永久红线（最高优先级）

> **boot_a 是 Android 的启动分区（/dev/sde12），绝对不允许写入。**
> 只允许只读读取；任何需要改启动/DTB 的操作一律改 rootfs 上的文件。
> 红线以本文件与 `../NOTES.md` 为准。

# 指纹移植项目完整交接文档

> 日期：2026-11-10 08:10
> 状态（历史，2026-11-10）：录入流程打通；后续验证/GDM/sudo 均已解决，当前状态见 `README.md`/`INSTALL.md`。
> 设备：Xiaomi Mi Pad 5 Pro (SM8250 / elish)，Armbian 6.18.44

---

## 一、项目概述

### 目标
在 Armbian (Linux) 上启用 Goodix 指纹传感器，实现：
1. 指纹录入
2. 指纹验证
3. 系统登录/解锁（PAM/GDM）

### 硬件
- **SoC**: Qualcomm SM8250
- **指纹芯片**: Goodix GF3626ZS1（侧边电容式）
- **接口**: SPI（**由 TEE 安全世界控制**，Linux 只能控制电源/复位/中断）
- **GPIO**: 中断 GPIO 23（高有效）、复位 GPIO 125（低有效）
- **供电**: PM8150A LDO10 (L10A) @ 3.008V

### 软件架构
```
Linux 用户态
  ├── libfprint / fprintd（待做）
  ├── qsee-supplicant（已部署）← 提供文件服务给 TA
  └── gfharness（测试客户端）
        ↓ TEE 接口 (/dev/tee0, /dev/teepriv0)
Linux 内核
  ├── qseecom TEE 驱动（已移植）← 我们自己加的
  ├── goodix_fp_spi 驱动（已编译）← 传感器电源/复位/IRQ
  └── mdt_loader（已加 ELF64 支持）← 加载 TA 镜像
        ↓ SCM 调用
Qualcomm TEE (QSEE)
  └── gfenu TA（Goodix 的签名程序，处理 SPI 采集和算法）
        ↓ SPI
指纹传感器
```

---

## 二、已完成的工作

### 阶段 1：TA 镜像（✅ 完成）

| 项目 | 内容 |
|---|---|
| TA 镜像位置 | Android `/vendor/firmware_mnt/image/` → 提取后放 `/lib/firmware/` |
| 正确的 TA | **`gfenu`**（不是 `fingerpr`）|
| 文件 | `gfenu.mdt` + `gfenu.b00~b07`（约 4.8 MB）|
| 备份位置 | `ta-image-gfenu/`、`ta-image/`（fingerpr）|

**关键发现**：Android 固件里有两套 Goodix TA：
- `gfenu` ← **Goodix HAL 实际使用的**（正确）
- `fingerpr` ← 给 FPC HAL 或旧方案的（错误，所有命令都失败）

### 阶段 2：内核驱动（✅ 完成）

| 组件 | 说明 |
|---|---|
| **qseecom TEE 驱动** | 从参考实现 `wrobelda/linux` 分支 `qcom-qseecom-tee` 移植 |
| **mdt_loader ELF64** | 我们加的（原版只支持 ELF32，而 gfenu 使用 ELF32、fingerpr 是 ELF64）|
| **goodix_fp_spi** | 传感器驱动，编译为模块 `/lib/modules/.../extra/goodix_fp_spi.ko` |
| **DTB** | 添加了指纹节点（GPIO 23/125、L10A 供电）|

**设备节点**：`/dev/tee0`、`/dev/teepriv0`、`/dev/goodix_fp`

### 阶段 3：用户态（✅ 大部分完成）

| 组件 | 状态 |
|---|---|
| `gfharness` | ✅ 已编译，支持 `--load/--bringup/--capture/--auth/--raw` |
| `qsee-supplicant` | ✅ 已编译安装，systemd 服务 active |
| 安全存储 | ✅ `/var/lib/qsee-supplicant/data/vendor/fpdump/` |
| **指纹录入** | ✅ **完全成功** |

---

## 三、关键突破点（踩坑记录）

### 坑 1：内核模块 ABI 三件套

每次改内核配置，必须验证：

| 检查 | 命令 | 期望 |
|---|---|---|
| vermagic | `strings vmlinux \| grep mod_unload` | `6.18.44-current-sm8250 SMP mod_unload aarch64` |
| `struct module` 大小 | `pahole -C module vmlinux` | **1280 字节** |
| BTF | `CONFIG_DEBUG_INFO_BTF=y` + `CONFIG_MODULE_ALLOW_BTF_MISMATCH=y` | 都开 |

**教训**：
- 关 BTF → `struct module` 变小 → 所有 Armbian 模块拒绝加载 → **黑屏（只有背光）**
- 不同 pahole 版本 → 模块 BTF 校验失败（`-EINVAL`）→ 同样黑屏
- 必须保持 `CONFIG_LOCALVERSION="-current-sm8250"`，否则 vermagic 不匹配

### 坑 2：TA 镜像必须 ELF32/ELF64 兼容

- `fingerpr.mdt` 是 **ELF64**（原始 mdt_loader 不支持）
- `gfenu.mdt` 是 **ELF32**（原生支持）
- 我们给 `mdt_loader.c` 加了完整的 ELF 类抽象（`mdt_is_elf64` 等）

### 坑 3：TA 名字必须用 `gfenu`

Android 的 `libgf_ca.so` 里唯一的 TA 名字符串是 `gfenu`，logcat 确认：
```
[gf_ca_open_session] Loading gfenu Succeed.
```

用 `fingerpr` 时所有命令返回 `0xFFFFFFFF`（QSEECOM_RESULT_FAILURE）。

### 坑 4：SAVE 需要真正的 supplicant

- harness 自带的 `--supp` 是**测试实现**，SAVE 返回 `1035 GF_ERROR_WRITE_SECURE_OBJECT_FAILED`
- 部署真正的 `qsee-supplicant` 后 SAVE 成功

### 坑 5：CMA 内存耗尽

TA 加载需要从 CMA 分配 TZ 内存（约 5-6 MB）。多次加载失败会泄漏，导致后续 `ENOMEM`。
- 检查：`grep CmaFree /proc/meminfo`
- 恢复：重启或重新加载

---

## 四、当前状态

### ✅ 完全成功的部分

```
内核 TEE 驱动           ✅ /dev/tee0 + /dev/teepriv0
TA 加载（gfenu）        ✅ app_id=4
TA 会话                 ✅ session=1, ret=0x0
DETECT_SENSOR (1000)    ✅ GF_SUCCESS
INIT (1001)             ✅ GF_SUCCESS（几何 160×36）
INIT_FINISHED (1005)    ✅ GF_SUCCESS
GET_DEV_INFO (1089)     ✅ GF_SUCCESS（GF_CHIP_3626ZS1）
PRE_ENROLL (1006)       ✅ GF_SUCCESS
ENROLL (1007)           ✅ GF_SUCCESS
IRQ 采集 (1016)         ✅ 25 条样本全部接受
SAVE (1012)             ✅ GF_SUCCESS
──────────────────────────────────────────
指纹模板                 ✅ /var/lib/qsee-supplicant/data/vendor/fpdump/finger_0_0.so (127,680 字节)
```

### ⚠️ 待解决问题

#### 问题 1：指纹验证时传感器不可用（1062）

**现象**：
```
DETECT_SENSOR: status=1062 (GF_ERROR_SENSOR_NOT_AVAILABLE)
INIT:          status=1062
```

**可能原因**：
1. 传感器上电时序问题（L10A 使能后需要延时）
2. 上次会话的复位状态残留
3. 传感器驱动 ioctl 顺序问题

**排查方法**：
```bash
# 确认 /dev/goodix_fp 能正常打开
ls -l /dev/goodix_fp
# 检查 L10A 供电
grep vreg_l10a /sys/class/regulator/regulator.*/name 2>/dev/null
# 看内核日志
sudo dmesg | grep -iE "goodix|vreg_l10a" | tail -20
```

#### 问题 2：libfprint 驱动未完成

**参考实现**：`wrobelda/libfprint`（分支）

需要：
1. 克隆并编译
2. 适配我们的 `gfenu` TA 和 supplicant 路径
3. 对接 fprintd

#### 问题 3：fprintd / PAM 未配置

待做：
- fprintd 服务
- PAM 指纹认证（sudo/登录/锁屏）
- GDM 指纹登录

#### 问题 4：DTB 的 firmware-name 需要改

当前 `/proc/device-tree/fingerprint/firmware-name` = `fingerpr`（错误）
应该改为 `gfenu`（需要用 dtc 改 DTB + 重建启动镜像）

---

## 五、完整复现步骤

### 前置条件
- 内核已部署（含 qseecom TEE 驱动、ELF64 mdt_loader、goodix_fp_spi 模块）
- DTB 含指纹节点
- `/lib/firmware/gfenu.*` 存在
- `qsee-supplicant` 已安装并运行

### 完整流程

```bash
# 1. 确认服务
systemctl is-active qsee-supplicant
ls /dev/tee0 /dev/teepriv0 /dev/goodix_fp

# 2. 加载 TA（保持进程运行）
cd ~/kernel-patches/fingerprint-port/goodix-tools/harness
(sleep 3600; echo) | sudo ./gfharness --load gfenu &

# 3. 采集指纹（25 次按压，150 秒）
sudo ./gfharness --capture 150 gfenu

# 输出应该看到：
#   --- interrupt N ---
#   irq mask=0x00000080 IMAGE
#   enrol: group=0 finger=0xXXXXXXXX remaining=N
#   ...
#   --- enrolment complete: POST_ENROLL + SAVE ---
#   invoke class 1 cmd 1012: status=0 GF_SUCCESS

# 4. 确认模板已保存
sudo ls -la /var/lib/qsee-supplicant/data/vendor/fpdump/
# 应该看到 finger_0_0.so（约 127 KB）

# 5. 验证（当前有 1062 问题）
sudo ./gfharness --auth --capture 40 gfenu
```

### 测试单条命令

```bash
sudo ./gfharness --raw <cmd> <len> [--set <off>:<val>] gfenu

# 例：DETECT_SENSOR
sudo ./gfharness --raw 1000 520 --set 512:4 gfenu
```

---

## 六、文件位置

### 项目目录 `/home/<user>/kernel-patches/fingerprint-port/`

| 文件/目录 | 内容 |
|---|---|
| `REPORT.md` | 阶段 0 硬件调研报告 |
| `PORTING-PLAN.md` | 分阶段移植计划 |
| `TODO-阶段3.md` | 待办清单 |
| `CAPTURE-HANDOFF-20261110.md` | Android 抓包交接（PC 侧 DSH）|
| `ANDROID-HELP-NEEDED.md` | 向 Android 侧求助的需求 |
| `ANDROID-ANSWER-20261110.md` | **Android 侧的答复（关键：TA 名字是 gfenu）** |
| `ta-image-gfenu/` | gfenu TA 完整文件 |
| `ta-image/` | fingerpr TA（错误版本，仅存档）|
| `android-storage-fpdump/` | Android 的安全存储对象（5 个）|
| `capture-android-20261110/` | Android 运行时命令抓包（payload 完整）|
| `vendor-libs/` | 提取的 Goodix 厂商库（libgf_hal.so 等）|
| `goodix-tools/` | 参考实现仓库（含 harness、文档、抓包工具）|
| `qsee-supplicant/` | supplicant 源码（已编译）|
| `kernel-build/` | 内核构建配置和安装脚本 |
| `driver-src/` | goodix_fp_spi 驱动源码 |

### 系统文件

| 路径 | 内容 |
|---|---|
| `/var/tmp/kbuild-src` | 内核源码树（1.7 GB，含所有补丁）|
| `/lib/firmware/gfenu.*` | TA 镜像（9 个文件）|
| `/lib/firmware/fingerpr.*` | 错误的 TA（可删）|
| `/lib/modules/.../extra/goodix_fp_spi.ko` | 传感器驱动 |
| `/var/lib/qsee-supplicant/data/vendor/fpdump/` | **安全存储（含已录入模板）** |
| `/etc/modules-load.d/goodix-fp.conf` | 传感器驱动自启 |
| `/etc/systemd/system/` | qsee-supplicant 服务 |

### 重要备份

```
/boot/armbian-kernel-csot.img.ORIGINAL-good              ← 原始可启动镜像
/boot/armbian-kernel-csot.img.pre-tee-*                  ← 各次安装前备份
/boot/armbian-kernel-csot.img.BAD-wrong-version          ← 坏版本，勿用
```

---

## 七、内核构建说明

### 配置要点

```bash
# 必须的配置
CONFIG_TEE=y
CONFIG_TEE_QSEECOM=y          # 新增
CONFIG_QCOM_QSEECOM=y         # 新增
CONFIG_QCOM_MDT_LOADER=y
CONFIG_DEBUG_INFO_BTF=y
CONFIG_MODULE_ALLOW_BTF_MISMATCH=y
CONFIG_LOCALVERSION="-current-sm8250"
```

### 构建命令

```bash
cd /var/tmp/kbuild-src
# 需要 4GB 磁盘 swap（BTF 生成需要）
sudo swapon /swapfile

make ARCH=arm64 -j2 Image

# 安装
sudo bash ~/kernel-patches/fingerprint-port/kernel-build/install-new-kernel.sh
```

### 源码树的内容

- mainline 6.18.44
- Armbian sm8250 补丁集（68 个，已全部应用）
- 我们加的：
  - `drivers/tee/qseecom/`（TEE 驱动）
  - `drivers/firmware/qcom/qcom_scm.c` 的 qseecom 部分
  - `drivers/soc/qcom/mdt_loader.c` 的 ELF64 支持
  - 两个 Armbian 补丁的 6.18 API 适配（`bq25970_charger.c`、`qcom_fg.c`）

---

## 八、下一步建议

### 优先级 1：修复验证时的 1062

1. 检查传感器上电时序（`gfharness` 的 `sensor_power_up`）
2. 参考 `goodix_fp_spi.c` 的 ioctl 实现
3. 可能需要在上电后加延时，或调整复位顺序

### 优先级 2：libfprint 集成

1. 克隆 `wrobelda/libfprint`
2. 编译（依赖：glib、libfprint 开发包）
3. 配置 fprintd 使用我们的 supplicant

### 优先级 3：系统集成

1. fprintd 服务
2. PAM 配置（先在 sudo 上测试）
3. GDM 指纹登录

### 优先级 4：清理

1. 改 DTB 的 `firmware-name` 为 `gfenu`
2. 删除 `/lib/firmware/fingerpr.*`
3. 删除 `/boot/armbian-kernel-csot.img.BAD-wrong-version`

---

## 九、参考资料

- **参考实现仓库**：`https://github.com/wrobelda/goodix-fp-spi-linux`
  - 文档在 `goodix-tools/docs/`（00-08）
  - 协议文档 `02-ta-protocol.md` 最重要
- **qsee-supplicant**：`https://github.com/wrobelda/qsee-supplicant`
- **抓包工具**：`goodix-tools/tracing/`（frida 脚本）
- **Android 侧协助**：另一个 DSH 在 PC 上通过 adb 操作

---

## 十、验证命令速查

```bash
# 测 TEE
sudo ./gfharness
# 期望：impl_id=5, OPEN_SESSION('gfenu') 成功

# 加载 TA
sudo ./gfharness --load gfenu

# 完整 bring-up
sudo ./gfharness --bringup gfenu
# 期望：4 个命令全 GF_SUCCESS

# 录入
sudo ./gfharness --capture 150 gfenu

# 验证
sudo ./gfharness --auth --capture 40 gfenu

# 单条命令
sudo ./gfharness --raw 1000 520 --set 512:4 gfenu

# 存储检查
sudo ls -la /var/lib/qsee-supplicant/data/vendor/fpdump/

# 服务状态
systemctl status qsee-supplicant
systemctl status goodix-fp-spi  # 如果做成服务
```

---

## 十一、续作状态（2026-11-10 08:20，DSH 会话 2 接手）

### 结论：指纹排查被 CMA 碎片阻塞，已安排重启 + 开机自检

接手时实际状态与上面「验证命令速查」的前提不同：

- **没有 loader 进程**；`/dev/tee0` 上只有 supplicant（旧的 `--load` 早退出了）
- 之前 5 次 `--load gfenu`（dmesg 里 app 4~8）留下的 CMA 碎片导致再次加载失败：
  ```
  load 'gfenu': Cannot allocate memory
  cma: __cma_alloc: reserved: alloc failed, req-size: 2244 pages, ret: -16
  ```
  `/proc/meminfo` CmaFree 还有 14 MB，但内核拿不到 **9 MB 物理连续块**；
  `drop_caches` / `compact_memory` / msm shrinker 都只回收 1~2 MB，无效。
- **TA 未加载时客户端 `OPEN_SESSION('gfenu')` 返回 ENOENT 是正常的**：
  `/dev/tee0` 客户端不能加载 TA；必须由 `/dev/teepriv0` 的 loader 先加载并**保持会话**，
  driver 的 app 注册表在最后一个会话关闭时卸载。**不要反复 `--load`**（每次要 ~9 MB 连续 CMA，
  `PAGE_ALIGN(img_len)+4 MB` 对齐；多了必把 CMA 打碎）。

### 本次会话做的改动

- `systemctl enable qsee-supplicant`（原来 disabled，重启后原本不会自启）
- 新增开机自检：`fp-boot-test.service` → `/usr/local/bin/fp-boot-test.sh`
  - 开机等 `/dev/tee0`、`/dev/teepriv0`、`/dev/goodix_fp`、supplicant 就绪
  - 加载 gfenu 并用 `sleep infinity` 管道**让 loader 常驻**（注册表保持）
  - 依次执行 `--bringup gfenu`、`--auth --capture 40 gfenu`
  - 日志：`/var/log/fp-boot-test.log`（loader 输出 `/tmp/fp-load.log`）
  - 跑完自动 `disable`；带计数保险（最多 2 次，防机器复位循环）
- 设备已重启（08:2x），以清空 CMA 碎片。

### 重启后下一步（新会话从这里开始）

1. 终端里重新起 DSH：`dsh web`，浏览器打开 `http://127.0.0.1:3080`
2. `cat /var/log/fp-boot-test.log` —— 里面已有本次 bringup/auth 的真实 status
3. 若 `DETECT_SENSOR` 仍是 **1062**（GF_ERROR_SENSOR_NOT_AVAILABLE）：
   - 先确认 loader 常驻：`pgrep -a gfharness`、`grep CmaFree /proc/meminfo`
   - `--power 30` 打开 `/dev/goodix_fp` 期间再发 `--raw 1000 520 --set 512:4 gfenu`
   - 查 L10A：`for f in /sys/class/regulator/regulator.*/name; do echo "$f: $(cat $f)"; done | grep -i l10`
   - 重点怀疑：SAVE(1012) 后 TA 把 sensor 置 sleep/reset，新会话需要更强复位/电源循环；
     driver 里 open 时 `regulator_enable` + 复位拉低（逻辑 1），`GF_IOC_RESET` 只打 3 ms 复位脉冲
   - 对照：`goodix-tools/docs/02-ta-protocol.md` 第 236 行（"sensor held in reset → 1062"）
4. 若 bringup 全 GF_SUCCESS：直接 `--auth --capture 40` 看 1062 是否随 CMA 修复消失
5. 之后按原优先级：libfprint（`wrobelda/libfprint`）→ fprintd/PAM → DTB `firmware-name` 改 `gfenu`

### 关键提醒

- **重新跑 `--load` 前先看 `CmaFree`**：需 ≥ 10 MB 且尽量在刚开机时加载一次就常驻
- `/tmp` 是 tmpfs，重启即清；持久日志看 `/var/log/fp-boot-test.log`
- 已录入模板仍在 `/var/lib/qsee-supplicant/data/vendor/fpdump/finger_0_0.so`（127,680 B）

---

## 十二、生产用户态栈打通（2026-11-10 08:36，DSH 会话 2 续）

### 结论：`fprintd` 录入 + 验证全链路成功

```
fprintd-enroll -f right-index-finger <user>  → Enroll result: enroll-completed
fprintd-verify <user>                        → Verify result: verify-match (done)
```

底层 harness 也复核过：bringup 4 条命令全 `GF_SUCCESS`；auth 7 次 `RECOGNISED`
（日志已存 `fingerprint-port/logs/`）。

### 已安装/配置的组件

| 组件 | 状态 |
|---|---|
| `qsee-supplicant.service` | ✅ enabled + active（开机自启）|
| `qsee-app-loader@.service` + udev 规则 `60-goodix-qsee.rules` | ✅ udev add → 自动起 `qsee-app-loader@fingerpr`（本机 DT 名字见下）|
| libfprint `wrobelda/libfprint` 分支 `goodix-qsee`，1.94.100 | ✅ 源码在 `fingerprint-port/libfprint/`；`meson -Ddrivers=goodixqsee` 编译，`ninja install` 覆盖 distro 1.94.7 |
| `fprintd` 1.94.3 + `libpam-fprintd` | ✅ apt 安装 |
| fprintd 设备沙箱 drop-in | ✅ `/etc/systemd/system/fprintd.service.d/60-goodix-qsee.conf`（DeviceAllow `goodix_fp` + `tee0-3`）|
| PAM | ✅ Ubuntu 自带 `/etc/pam.d/gdm-fingerprint`（含 `pam_fprintd.so`），未改动 |

### 当前 TA 名仍是 `fingerpr`（临时兼容，DTB 未改）

- DT `/proc/device-tree/fingerprint/firmware-name` = **fingerpr**（旧值），udev/libfprint 都用这个名字
- 处理：把 `/lib/firmware/fingerpr.*` **覆盖成 gfenu 镜像**（`fingerpr.mdt` md5 == `gfenu.mdt`），
  于是 `qsee-app-loader@fingerpr` 实际加载的是 gfenu TA；libfprint 设备 id = `profile-1:fingerpr`
- 原始错误 fingerpr TA 备份在 `ta-image/`，未丢
- 持久 print：`/var/lib/fprint/<user>/goodixqsee/profile-1:fingerpr/7`

### 注意

1. **不要同时启用 `fp-boot-test.service` / `gfharness --load`**：生产路径由 udev 起
   `qsee-app-loader@<firmware_name>`；两个 loader 抢同一 TA 会 EINVAL/ENOENT。
   `fp-boot-test.service` 已 disabled（勿再 enable）。
2. **sudo 有 zenity 审批弹窗（30s 超时自动拒绝）**：批量执行 sudo；超时会报
   `sudo: pam_open_session：未知错误 -4`。这是设计行为，不是损坏。
3. **apt 升级 `libfprint-2-2` 会覆盖自编译的 goodixqsee 驱动**：升级后需重新
   `meson setup ... && ninja -C libfprint/build install`（源码目录保留）。
4. 重启后链路：supplicant 自启 → `goodix_fp` 出现 → udev 规则起 loader → fprintd 由 DBus 激活。

### GDM 锁屏指纹解锁已实测成功（08:43）

- 锁屏（Super+L）后密码框右侧出现指纹图标，按压传感器**直接解锁**，无需密码
- Ubuntu 24.04 的 GNOME Shell 46 自带支持：`/usr/lib/gnome-shell/libshell-14.so` 里
  `FINGERPRINT_SERVICE_NAME='gdm-fingerprint'`，锁屏通过 GDM 的 `gdm-fingerprint` PAM 服务认证
- 首次锁屏曾失败一次：`fprintd: Authorization denied ... 'Claim' ... Device was already claimed`
  （之前 `fprintd-list/verify` 的会话残留占着 claim），fprintd 空闲退出后重试即正常。
  若以后锁屏不出指纹图标：`systemctl restart fprintd` 或等 fprintd 空闲退出后再锁屏
- 证据日志：`logs/gdm-fingerprint-unlock-20261110.log`

### 剩余工作

1. ~~**DTB `firmware-name` 改 `gfenu`**~~ **已完成（2026-11-10 08:54）**：
   - 采用**就地二进制替换**（`fingerpr\0` → `gfenu\0\0\0\0`，长度不变，只改 8 字节），
     **没有用 dtc 反编译/重编译**——实测 dtc 往返会把 `regulators-1/ldo10` 的 4 字节
     `regulator-min/max-microvolt`（`00 32 4b 00`）改成 3 字节，属于语义破坏（L10A 相关，危险）。
   - 已改 `/usr/lib/linux-image-6.18.44-current-sm8250/qcom/sm8250-xiaomi-elish-csot.dtb`，
     重建 `/boot/armbian-kernel-csot.img`（与旧镜像仅差 8 字节 + header SHA1），
     写 `/dev/disk/by-partlabel/boot_b` 并 `cmp` 校验通过。
   - 备份：`/boot/armbian-kernel-csot.img.pre-dtb-gfenu-20261110-085445`、
     `.../sm8250-xiaomi-elish-csot.dtb.pre-gfenu-20261110-085445`；日志 `/var/log/dtb-gfenu-*.log`
   - 另外注意：`dtc` 反编译出的 DTS 里 `fingerprint` 节点为 `firmware-name = "fingerpr"`；
     修改只能就地改二进制，或改内核 DTS 源码重编（源码树里目前没有该节点）。
   - **重启后**：udev 会按新 `firmware_name=gfenu` 自动起 `qsee-app-loader@gfenu`，
     libfprint 设备 id 变为 `profile-1:gfenu`，**已录指纹（profile-1:fingerpr）失效，需重新
     `fprintd-enroll -f right-index-finger <user>`**；之后可删 `/lib/firmware/fingerpr.*`。
2. ~~**GDM 锁屏/登录实测**~~ **已完成**：锁屏指纹图标出现并成功解锁（见上）。
3. **开机自启验证**：已在 08:46 重启后验证 —— `qsee-supplicant` active、
   `qsee-app-loader@fingerpr` 开机自动加载 TA、已录指纹仍在。
   **本次 DTB 改动重启后需再验一次**（应变成 `qsee-app-loader@gfenu`）。
4. 可选：把 PAM 指纹也加到 sudo（`/usr/share/pam-configs/fprintd` 或 `/etc/pam.d/sudo`），
   先小范围测试，避免锁死。
5. 清理：`fp-boot-test.service` 已 stop+disabled；DTB 改动重启后可删 `/lib/firmware/fingerpr.*`。

### 识别率诊断（2026-11-10 08:50）

- 用户反馈"识别经常失败"。用 `fprintd-verify` 连测：**4 次 `verify-no-match`（TA 1064，真不匹配）**、
  1 次 `verify-unknown-error`（重试类状态）；没有 `verify-retry-scan`/`verify-finger-not-centered`
  等图像质量类错误 → 归类为**真识别失败**（手汗改变脊线图像也会落到这一类）。
- 日志：`logs/fprintd-verify-fails-20261110.log`
- 结论：传感器/TA/驱动链路正常；失败是匹配算法层面的（小面积侧边传感器 160×36 + 手汗对录入质量敏感）。
  重新录入时尽量用干燥、位置一致的手指多按几次可提高识别率。

---

## 十三、最终收尾状态（2026-11-10 09:07）

### DTB 改动后重启验证通过

- `firmware_name = gfenu`（新 DTB 生效），udev 自动起 **`qsee-app-loader@gfenu`**，
  `qsee-supplicant` active，TA `loaded 'gfenu' as app 4`
- libfprint 设备 id 变为 **`profile-1:gfenu`**
- 用户已重新录入并确认可用：`fprintd-list` → `#0: left-index-finger`
- 旧设备 id 的 print 记录 `profile-1:fingerpr` 已删除（孤儿）

### 已清理

| 项目 | 结果 |
|---|---|
| `/var/lib/fprint/<user>/goodixqsee/profile-1:fingerpr/`（孤儿，84 B）| ✅ 已删 |
| `/lib/firmware/fingerpr.*`（临时 gfenu 镜像副本，4.8 MB）| ✅ 已删 |
| `/boot/armbian-kernel-csot.img.BAD-wrong-version`、`.tee-broken-20261110`（105 MB）| ✅ 已删 |
| `.pre-fp`、`.pre-tee-20261110-010046/011516/055552`（~219 MB）| ✅ 已删 |
| 保留 | 当前 img、`.bak`（原厂）、`.ORIGINAL-good`、`.pre-dtb-gfenu-20261110-085445` |

### 旧录入数据的占用（回答用户）

- **不占 TEE/Secure World 内存**；TEE 里只有载入的 `gfenu` TA（CMA ~15 MB，与录入数量无关）
- TA 侧旧模板只剩 **`finger_0_0_bak.so`（129,313 B，密封 blob，存磁盘）** —— 是 TA 在 SAVE 时
  自动保留的上一个模板（回滚用），不要手删；`fpdump/` 共 300 KB
- libfprint 侧旧记录只有 84 B 元数据（不含图像/模板），已删
- 结论：残留 ~126 KB 磁盘 + 6 个密封对象 300 KB，属正常设计

### 仍可选

- PAM 指纹加到 sudo（`/usr/share/pam-configs/fprintd` 或 `/etc/pam.d/sudo`），先小范围测试
- 若想彻底清空 TA 侧旧模板：用 fprintd 删除该手指后重录（让 TA 重写 finger_0_0 系列），
  不建议直接删 `_bak.so`
- 识别率：小面积侧边传感器 + 手汗敏感，录入时保持手指干燥、位置一致

---

## 十四、sudo 指纹认证（2026-11-10 09:20）

### 改动（已实测通过）

- 删除 sudo 免密：`/etc/sudoers.d/020-nopasswd` → `020-nopasswd.disabled`
  （sudo 忽略文件名含 `.` 的文件）
- `/etc/pam.d/sudo` 认证栈加一行（放在 `@include common-auth` 之前）：
  ```
  auth	[success=done default=ignore]	pam_fprintd.so max-tries=1 timeout=8
  ```
  成功=跳到栈尾（通过）；失败/8s 超时=继续走 common-auth 的 `pam_unix` 密码
- 注释掉原 zenity 审批 hook（`pam_exec sudo-notify`）——认证本身即审批
- 备份：`/root/fp-pam-backup-20261110-091020/`（`sudo`、`020-nopasswd`、`sudo-notify`）

### 效果

- `sudo -v` 终端提示"请把您的左手食指放在指纹读取器上" → 按左手食指 → 通过（实测 rc=0）
- 8 秒内没按 → 自动出现密码提示，输密码即可
- **PAM 是串行不是真并行**：指纹优先，任选其一通过即可；指纹等待期间输入的密码会被缓冲，
  超时后立即生效
- **注意**：改造后 DSH 自己执行的 sudo 也要你按指纹（从后台执行时看不到提示，按手指即可）
- PAM 语法坑：这版 PAM 不认 `success=end`，会报
  `PAM pam_parse: expecting jump number` 并丢弃整行；正确写法是 `success=done`

### 重要坑：并发/中断会把 TA 弄坏（这就是原始 1062 的成因）

- 现象：`qseecom: scm call failed with error -22`；harness 里命令全部 `EINVAL`，
  `DETECT_SENSOR`/`INIT` 先返回 **1062**（GF_ERROR_SENSOR_NOT_AVAILABLE）
- 实测结论：
  - **顺序**使用：干净超时（30s deadline）或强杀客户端，TA 都**不会**坏
  - **短时间连续/并发**多个 verify（如脚本 5 连测）会把 TA 留在 armed 状态 → 坏
  - 原始交接文档的"验证时 1062"就是同类：操作被中断/并发后 TA 未复位
- **恢复（无需重启）**：
  ```
  sudo systemctl restart qsee-app-loader@gfenu      # 或 pkexec systemctl restart ...
  ```
  重载后 `--bringup` 4 条命令恢复 `GF_SUCCESS`（已实测）
- 若 sudo 失效且需要 root，用 `pkexec`（polkit，不走 /etc/pam.d/sudo）

### 回退方法

```
pkexec bash -c '
  cp /root/fp-pam-backup-20261110-091020/sudo /etc/pam.d/sudo
  mv /etc/sudoers.d/020-nopasswd.disabled /etc/sudoers.d/020-nopasswd
  cp /root/fp-pam-backup-20261110-091020/sudo-notify /usr/local/bin/sudo-notify
'
```
回退后恢复"免密 sudo + zenity 审批"；指纹仍用于 GDM 锁屏登录（不受影响）。

### sudo 指纹弹窗提示（2026-11-10 10:15，已实测）

问题：`pam_fprintd` 的提示只输出到终端，从 DSH 后台触发的 sudo 看不到，用户不知道何时按指纹。

方案（root hook 只发信号，用户会话守护进程负责弹窗）：

| 文件 | 作用 |
|---|---|
| `/usr/local/bin/fp-sudo-prompt` | `/etc/pam.d/sudo` 的 `auth optional pam_exec.so`；**以登录用户身份运行**（uid 1000，不是 root）。写 `/run/user/1000/fp-sudo-prompt.{msg,stamp}`（内容含 sudo 命令），并 `setsid` 派一个"关闭器"监视 sudo PID，sudo 退出时写 `fp-sudo-prompt.done` |
| `~/.local/bin/fp-sudo-prompt-watch` | 用户会话守护脚本：轮询 stamp 变化 → 弹 `zenity --info --no-markup --timeout=12` 显示"—<br>sudo 请求：<命令>…请按左手食指…"；看到 done 变化 → 立即杀掉该 zenity 窗口 |
| `~/.config/systemd/user/fp-sudo-prompt-watch.service` | 守护脚本的 systemd --user 服务（enabled，登录自启，Restart=always）|

效果（实测）：sudo 时桌面弹窗 → 按左手食指 → 验证通过 → **弹窗立即消失**，sudo 继续执行；未按则弹窗 12s 后自动关闭并回退密码。

踩坑记录（供以后改）：
- 不能在 pam_exec 里直接弹窗：hook 里启动的 zenity 在 PAM 上下文显示不出来（GTK/Wayland 凭据/会话问题）
- hook 里 `seteuid` 后 GTK 仍无法显示；`runuser` 手动运行可以，但 PAM 里后台进程不可靠
- pam_exec 默认以**登录用户**身份跑（不是 root），所以写不了 root 的 `/run`；要用 `/run/user/1000`
- zenity 的 `--text` 默认按 Pango markup 解析，命令里的特殊字符会导致渲染失败 → 必须加 `--no-markup`
- 弹窗自动关闭：**认证成功即关**，不依赖命令跑完
  - `/usr/local/bin/fp-sudo-prompt-done`（PAM **session** 阶段 `pam_exec`）在 `open_session` 时写
    `fp-sudo-prompt.done`；用户会话 watcher 看到就杀掉 zenity
  - 实测（`sudo sleep 8`）：10:30:36.25 `session opened` → 10:30:36.48 弹窗已关 →
    10:30:44.28 `session closed`（命令 8 秒后才结束）→ 证明关闭与命令时长无关
  - 注：`sudo -v`（仅校验、不开 session）会退回到 zenity 12s 超时自动关闭，属于可接受的小瑕疵
  - auth hook 里那个"监视 sudo 进程退出"的 closer 保留作为兜底（-v 等场景）

---

## 十五、TA 看门狗：自动检测拒服务并重载（2026-11-10 10:23，已验证）

### 背景

TA 被中断/并发操作搞坏后进入"拒服务"状态，之前只能手动
`systemctl restart qsee-app-loader@gfenu`。现在改为自动恢复。

### 检测难点与解决

- fprintd **不会把"打开失败"写日志**（只通过 D-Bus 返回客户端）；内核也只在 SCM 调用失败时留痕，
  TA 返回失败状态（如 `Goodix command 1005 returned status 1001`）时没有内核日志
- 解决：给 libfprint `goodixqsee` 驱动加失败日志（源码已改，需重新 `ninja -C build install`）：
  - `bring_up()` 失败：`fp_warn("Goodix QSEE bring-up command %u failed: %s")`
  - worker 任意失败：`fp_warn("Goodix QSEE %s failed: %s")`（open/list/delete/enroll/identify/clear）
  - 这些 warning 进 journal（`SYSLOG_IDENTIFIER=fprintd`）
- 代码位置：`fingerprint-port/libfprint/libfprint/drivers/goodixqsee/goodix-qsee.c`

### 看门狗

| 文件 | 内容 |
|---|---|
| `/usr/local/bin/fp-ta-watchdog` | `journalctl -f _TRANSPORT=kernel + SYSLOG_IDENTIFIER=fprintd` 监听；匹配 TA 失败特征 → `systemctl restart qsee-app-loader@gfenu`；冷却 45s |
| `/etc/systemd/system/fp-ta-watchdog.service` | root 服务，`Restart=always`，**已 enable**（开机自启）|
| `/var/log/fp-ta-watchdog.log` | 每次触发的证据行 |

匹配模式：`Goodix QSEE open failed` / `bring-up command` / `enroll failed` / `identify failed` /
`qseecom: scm call failed` / `Device reported an error during verify: QSEE` / `Goodix command ... returned status`；
并排除 `opening Goodix sensor`（设备忙，不是 TA 故障）。

### 验证结果（实测）

- TA 缺失模拟（停 loader）→ fprintd open 失败 → 驱动日志
  `Goodix QSEE open failed: opening Goodix TA: 没有那个文件或目录` → **0.5s 内**看门狗重启 loader →
  `application_loaded app=gfenu` → 健康恢复（`Verify started!`）
- 误报测试：`logger` 写入含关键字的普通日志 → **不触发**（loader 时间戳不变）
- 真报测试：写入内核日志 `qseecom: scm call failed` → **触发重载**（时间戳 10:22:21 → 10:22:57）
- 当前服务状态：`qsee-supplicant` / `qsee-app-loader@gfenu` / `fprintd` / `fp-ta-watchdog` 全 active；
  supplicant、watchdog、用户弹窗 watcher 均 enabled

### 注意

- 看门狗依赖 libfprint 的新日志；**apt 升级 libfprint-2-2 会覆盖**，需重新
  `cd fingerprint-port/libfprint && ninja -C build install`
- 冷却 45s：短时间内连续坏只重载一次；若仍坏可手动再重启
- 看门狗只监听 kernel + fprintd，避免命令文本/普通日志误触发

## 十六、Pending TODO（2026-11-10，延后）

- [ ] **闪光灯启动爆闪**：每次开机 PM8150L flash LED 会闪一下。已定位：`leds_qcom_flash` 是模块，DTS 节点 `pm8150l leds@d300/led-0`；驱动 probe 无点灯操作，推测是 PMIC 上电默认态或 ABL 阶段。
     验证/修复：① 黑名单模块重启看是否仍闪（区分 bootloader vs 驱动）；② 若驱动原因，在 probe 早期写 `MODULE_EN=0` + 清所有通道 strobe，重编 `leds-qcom-flash.ko`。
- [ ] **传感器（SLPI）**：SLPI 已能启动；阻塞在 `/dev/fastrpc-sdsp` probe 报 `-22`（`qcom,fastrpc` rpmsg probe 失败），hexagonrpcd-sdsp 无设备节点；libssc 亦需 meson>=1.4。

