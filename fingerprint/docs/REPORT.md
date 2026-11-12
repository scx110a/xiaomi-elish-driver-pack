# 指纹传感器移植调研（阶段 0）

日期：2026-11-09
设备：Xiaomi Mi Pad 5 Pro (elish / SM8250)，运行 Armbian (6.18.44)

---

## 一、硬件与驱动架构（已确认）

| 项目 | 结论 | 证据 |
|---|---|---|
| 传感器品牌 | **Goodix**（SIP 电容式，侧边） | Android 内核字符串 `goodix_fp`、`gf_spi` |
| 接口 | **SPI**（但 OS 不直接访问） | 见下 |
| DT 匹配 | `compatible = "goodix,fingerprint"` | 驱动 of_match 表 |
| DT 属性 | `goodix,gpio-irq`、`goodix,gpio-reset`、`fp-gpio-pwr` | `platform.c` |
| 供电 | regulator 名 `fp_vdd_vreg` | `gf_spi.c:595` |
| 时钟 | `core_clk` / `iface_clk` | `gf_spi.c:190,197` |
| **工作模式** | **TEE 模式**（指纹逻辑在安全世界） | 见下 |

### TEE 模式的关键证据

1. **defconfig**：`CONFIG_FINGERPRINT_GOODIX_TA=y`（TA = Trusted Application）
2. **内核驱动没有 SPI 传输代码**：`gf_spi.c` 里 `spi_sync`/`spi_write`/`spi_read`/`spi_transfer` **一个都没有**
3. 驱动只做：电源、复位、中断、时钟、DRM notifier、输入设备（导航手势）
4. 文件头注释：`TEE driver for goodix fingerprint sensor`

即：**Linux 内核只负责给传感器上电/复位/收中断，SPI 数据由安全世界的 TA 直接驱动**。
成像、模板、匹配全部在 TEE 里的**签名闭源 TA**中完成。

## 一之补充：Android 侧实测确认（2026-11-09）

在 Android（root）实测：

| 检查 | 结果 | 结论 |
|---|---|---|
| `getprop ro.boot.fpsensor` | **goodix** | 引导程序传入，最权威 |
| `/proc/devices` | `goodix_fp_spi` | Goodix 驱动注册了字符设备 |
| `/dev` | `goodix_*` | 设备节点存在 |
| kmsg | `fpc1020_probe` | FPC 驱动也尝试了，但无硬件→退出 |
| kmsg | `fpc_1020_reset_*` pinctrl | DT 里有 FPC 引脚状态（双变体支持） |
| `ro.hardware.fp.vendor` | `fpc,goodix` | vendor 镜像双支持，运行时按硬件选择 |

**结论：实际传感器 = Goodix**（FPC 是同一 vendor 镜像支持的另一个变体）。

**驱动总线类型**：`USE_PLATFORM_BUS`（不是 SPI 总线），
`compatible = "goodix,fingerprint"`，DT 节点必须有：
`goodix,gpio-irq`、`goodix,gpio-reset`、`fp_vdd_vreg-supply`

## 一之补充 2：硬件资源已完整拿到（2026-11-09）

### 关键发现：板级 DT 在 `dtbo_a` 分区

`dtbo_a`（32 MB）装着 **多个机型的完整板级 DT**，其中：

```
dt12 = "Qualcomm Technologies, Inc. xiaomi elish"   ← 就是本机
```

已保存为 `elish-board.dts`（940 KB）和 `elish-fingerprint-dt-fragments.txt`。

之前找不到是因为只查了 `vendor_boot`（里面是 kona 通用参考 DT）。

### 指纹节点完整参数（运行时可读 + 板级 DT 交叉验证）

```dts
fingerprint_goodix {
    compatible = "goodix,fingerprint";
    goodix,gpio-reset = <&tlmm 125 GPIO_ACTIVE_LOW>;    /* 低电平复位 */
    goodix,gpio-irq   = <&tlmm 23  GPIO_ACTIVE_HIGH>;   /* 高电平中断 */
    fp_vdd_vreg-supply = <&L10A>;                       /* PM8150A LDO10, 3.008V */
    status = "ok";
};
```

| 参数 | 值 | 来源 |
|---|---|---|
| 节点名 | `fingerprint_goodix` | /proc/device-tree |
| IRQ GPIO | **23**，高电平有效 | 运行时 + 板级 DT 一致 |
| 复位 GPIO | **125**，低电平有效 | 运行时 + 板级 DT 一致 |
| 供电 | **L10A**（PM8150A LDO10），`0x2de600` = **3.008 V** | 板级 DT fragment@132 |
| 所在位置 | `/soc` 下 | fragment@130 |

### 主线需要补的内容

| 项 | 状态 |
|---|---|
| 指纹节点 | ❌ 主线 elish DTS 没有，需添加（参数已齐） |
| L10A 稳压器 | ❌ 主线 DTB 里没有 `l10a`，需添加定义（3.008V） |
| 驱动 | ❌ 用 wrobelda 的 `goodix_fp_spi.c`（传感器）+ `qseecom` TEE 驱动 |
| TA 协议 | ⚠️ 最大未知量，需在 Android 上抓包比对 |

## 一之补充 3：TA 协议抓包对比（2026-11-09，决定性）

用 wrobelda 的 frida 脚本在 Android 上抓取了一次完整操作（录入 + 解锁 + 删除）。

### 捕获到的命令序列

```
PRE_ENROLL(1006) ×1
ENROLL(1007) ×1
IRQ(1016) ×296            ← 手指按压过程中的帧
SAVE(1012) ×1
SET_ACTIVE_GROUP(1014) ×1
GET_AUTH_ID(1011) ×1
AUTHENTICATE(1010) ×4
AUTHENTICATE_FINISH(1086) ×4
REMOVE(1013) ×1
SCREEN_ON(1017) ×6 / SCREEN_OFF(1018) ×6
DUMP_TEMPLATE(1079) ×4
```

### 与参考文档逐项对比

| 项目 | 抓包值 | 文档值 | 结论 |
|---|---|---|---|
| PRE_ENROLL 长度 | 112 | 112 | ✓ |
| ENROLL 长度 | 180 | 180 | ✓ |
| IRQ 长度 | **327624** | 327624 | ✓ |
| AUTHENTICATE_FINISH 长度 | **124** | 124 | ✓ |
| SET_ACTIVE_GROUP 长度 | 104 | 104 | ✓ |
| REMOVE 长度 | 192 | — | ✓ |
| FINISH +104 (IRQ mask) | 1 | 来自 IRQ +100 | ✓ |
| FINISH +108 (operation) | 128 | 来自 IRQ +104 | ✓ |
| FINISH +112 (group) | 0 | — | ✓ |
| FINISH +116 (finger id) | 1369987086 | — | ✓ |
| IRQ +12 = 返回状态 | 1012/1045 与 ret 一致 | 文档明确"镜像到 +12" | ✓ |

### IRQ 状态码分布（实际类型）

| ret | 含义 | 次数 |
|---|---|---|
| 0 | 成功/继续 | 229 |
| 1064 | GF_ERROR_MATCH_FAIL_AND_RETRY | 32 |
| 1012 | GF_ERROR_IMAGER_DIRTY | 18 |
| 1110 | 未在文档中（新增/厂商扩展） | 14 |
| 1006 | 未在文档中 | 2 |
| 1045 | GF_ERROR_DUPLICATE_AREA | 1 |

其中 1012、1045、1064 都在参考文档的错误码表里 ✓；1110、1006 是这台设备的新增项。

### 结论

**elish 的 Goodix TA 使用与参考实现完全相同的协议：**

- ✅ 命令 ID 完全一致（1000-1101 体系）
- ✅ payload 长度逐一匹配（含 327624 字节的 IRQ 大包）
- ✅ 字段偏移逐一匹配（+12/+100/+104/+112/+116）
- ✅ 状态码体系一致

**参考实现的客户端栈可以直接移植**，主要工作量转移到：
1. 内核驱动移植（TEE + 传感器）
2. 安全存储 listener 实现（见 `docs/03-listener-services.md`）
3. TA 名称确认（QSEECOM 打开会话需要）

### TA 镜像已定位（2026-11-09）

modem 分区（`/dev/disk/by-partlabel/modem_a`，vfat 格式，**Armbian 可直接只读挂载**）
的 `/image/` 目录下：

```
fingerpr.mdt     7,464 字节      ← TA 主描述符
fingerpr.b00       512 字节
fingerpr.b01     6,952 字节
fingerpr.b02 6,105,848 字节
fingerpr.b03       186 字节
fingerpr.b04 4,193,264 字节
fingerpr.b05    10,576 字节
fingerpr.b06       304 字节
fingerpr.b07   493,208 字节
────────────────────────────
总计约 10.8 MB
```

这就是 Goodix 的**签名 TA 镜像**，已保存到 `ta-image/`。

- TA 名称候选：`fingerpr`（文件名）/ `fingerprint`（完整词）
- 内核驱动用 `request_firmware()` 从 `/lib/firmware` 加载 `<name>.mdt` + `.bNN`
- 部署时把这 9 个文件放到 `/lib/firmware/` 即可
- **注意**：TA 是签名且绑定 QSEE 的，只能原样使用，不能修改

## 二、Xiaomi 开源内核调研

分支：`MiCode/Xiaomi_Kernel_OpenSource` → **`elish-r-oss`**（Android 11 / kona）

| 项目 | 是否开源 |
|---|---|
| 内核源码 | ✅ |
| **板级 DTS（sm8250-xiaomi-elish.dts）** | ❌ **没有**（全仓库只有 3 个 elish 文件：2 个 defconfig + 1 个串口驱动） |
| Goodix 驱动源码 | ✅ `drivers/input/fingerprint/goodix/{gf_spi.c,platform.c,netlink.c}` |
| FPC 驱动（未使用） | ✅ `drivers/misc/fpr_FingerprintCard/fpc1020_platform_tee.c` |

**影响**：硬件资源（SPI 实例、GPIO 号、稳压器名）不在开源代码里，
必须从 Android 运行时或闭源 HAL 反推。

## 三、Android 侧资源（已定位）

super 分区（9.1 GB）扫描结果：

| 资源 | 位置 | 说明 |
|---|---|---|
| `libgf_hal.so` | 0x12557ac7e 附近 | Goodix 用户态 HAL，导出 `gf_hal_authenticate/enroll/close/...` |
| `fingerprint.goodix.so` | 0x12557c7af | Android HAL 模块 |
| VINTF 兼容矩阵 | 0x184371e1 | 确认 fingerprint HAL 1.0 |

HAL 是闭源 ARM 二进制，App 与 TA 之间走 QSEECOM。

## 四、社区现状（关键发现）

### ✅ 已有完整参考实现

[`wrobelda/goodix-fp-spi-linux`](https://github.com/wrobelda/goodix-fp-spi-linux) —— **正是我们这种情况**：

> "TEE support for the reference platform on Qualcomm QSEE is complete."

包含：

- **内核层**：
  - 传感器驱动 `drivers/input/misc/goodix_fp_spi.c`（Goodix 下游驱动的 mainline 移植版，
    来自 Xiaomi msm-5.4 的 TEE variant）
  - TEE 驱动 `qcom-qseecom-tee` 分支（QSEECOM 后端，接入 Linux TEE 子系统）
- **用户态**：QSEECOM supplicant、TA 加载器、libfprint 驱动（fprintd + GDM 可用）
- **文档**：TA 协议全文档（`docs/02-ta-protocol.md`）、安全存储、监听服务
- **逆向工具**：frida hooks（`tracing/android-gfhook.js`、`android-listener-hook.js`）
  —— 可在 Android 上抓 HAL↔TA 通信，用于适配新设备
- **其它**：udev 规则、命令行测试客户端（gfenu）、CI

### 限制

- TA 协议是"**one versioned Goodix QSEE profile**"，对其他 OEM/型号/TA 名称的可移植性
  **尚未验证**
- 内核侧尚未上游（有若干 uapi 设计问题待与维护者确认）
- 参考平台不是 elish

### 其它相关工作

- [`Han-Haocheng/gxfp5130-linux`](https://github.com/Han-Haocheng/gxfp5130-linux)：
  Goodix GXFP5130（华为笔记本 **eSPI** 传感器）的完整 Linux 方案，含内核模块 +
  libfprint + PAM，已提交 LKML。**型号不同**，但可作为"完整移植长什么样"的参考。
- LKML：[Goodix GXFP5130 eSPI fingerprint sensor driver](https://lkml.iu.edu/hypermail/linux/kernel/2607.2/05734.html)

## 五、移植路线图（基于参考实现）

### 阶段 1：补齐硬件资源（1-2 天）

需要：SPI 控制器实例、片选、IRQ GPIO、reset GPIO、供电 regulator。

途径（按优先级）：

1. Android root 后读 `/proc/device-tree/` 里的 `goodix,fingerprint` 节点 ← **最快**
2. 从 Android 的 vendor DTBO 拿（本机 vendor_boot 只有 kona 参考 DT，板级 overlay 未找到）
3. 从 `libgf_hal.so` 反推（它可能硬编码 SPI 路径——ueventd.rc 里有 spidev 权限条目）
4. 逻辑分析仪抓 Android 启动时的 SPI 时序 ← 最后手段

### 阶段 2：内核驱动移植（1-2 周）

1. 移植 wrobelda 的 `goodix_fp_spi.c`（传感器：电源/复位/中断）到 elish
2. 移植 `qcom-qseecom-tee` TEE 驱动（QSEECOM 后端 + mdt_loader 扩展）
3. 写 DT 节点（用阶段 1 的资源）+ pinctrl
4. 验证：TA 能被加载、能收到 IRQ

### 阶段 3：TA 协议适配（**主要风险点，2-8 周**）

1. 用 wrobelda 的 frida 工具在 Android 上抓 elish 的 HAL↔TA 通信
2. 与 `docs/02-ta-protocol.md` 的 profile 对比
3. 若一致 → 直接用；若不同 → 适配协议层（他们有完整的协议反向工程方法论）

**风险**：若 elish 的 TA 强制要求 Gatekeeper 签名令牌（`docs/06-Gatekeeper-protocol.md`），
而参考实现只用了 TA 的 challenge-only fallback，则可能需要额外适配。

### 阶段 4：用户态 + 集成（1-2 周）

1. 编译 wrobelda 的 QSEECOM supplicant + TA 加载器 + libfprint 驱动
2. fprintd + pam_fprintd → 登录 / sudo / 锁屏
3. GDM 登录界面指纹
4. udev 规则

## 六、可行性结论

| 项 | 评估 |
|---|---|
| 是否有先例 | ✅ **有完整参考实现**（同架构、同 TEE） |
| 硬件资源可获取性 | 中（需 Android root 读 DT） |
| TA 协议匹配度 | 未知（最关键的未知量） |
| 内核移植难度 | 中（代码现成，需适配 6.18 + QSEECOM） |
| **总体工作量** | **2-4 个月业余时间**（协议一致的话可压缩到 1-2 个月） |
| 成功率 | **中高**（有完整方法论和工具） |

**与之前"几乎不可能"的判断相比，现在应该修正为：工程量大但有清晰路径。**

## 七、建议的下一步

**先做一件事就能大幅降低不确定性**：在 Android 里 root 后：

```bash
# 1. 拿到硬件资源（决定性）
cat /proc/device-tree/soc@0/*/goodix* 2>/dev/null
grep -r "goodix" /proc/device-tree/ 2>/dev/null

# 2. 看 TA 名称和协议版本
dmesg | grep -iE "goodix|gf_spi|qsee"
ls /vendor/lib64/hw/ | grep -i finger
```

拿到设备树节点后，就能判断与 wrobelda 的 profile 是否吻合，
从而决定是否值得投入。

## 八、参考链接

- [Goodix TEE Linux 参考实现](https://github.com/wrobelda/goodix-fp-spi-linux)
- [TA 协议文档](https://github.com/wrobelda/goodix-fp-spi-linux/blob/master/docs/02-ta-protocol.md)
- [Xiaomi 开源内核 elish-r-oss](https://github.com/MiCode/Xiaomi_Kernel_OpenSource/tree/elish-r-oss)
- [GXFP5130 eSPI 驱动（另一款 Goodix）](https://github.com/Han-Haocheng/gxfp5130-linux)
- [LKML: Goodix GXFP5130 eSPI driver](https://lkml.iu.edu/hypermail/linux/kernel/2607.2/05734.html)
- [文章：Fingerprint sensors on tuxified Android phones: Impossible?](https://emainline.gitlab.io/2021/12/12/fingerprint_P1.html)
