# 升级影响评估（软件包 / 内核升级会破坏什么，哪些能修，哪些必须保持）

> 结论基于 2026-11-11 本机实测状态（kernel 6.18.44-current-sm8250，armbian-firmware-full 26.8.3）。
> 原则：**升级前先读本文件；内核/固件/boot 镜像是高风险项。** 任何升级后都跑一遍
> `install/check-upgrade.sh` 与 `install/verify.sh`。

> **boot 槽位**：本文以本机布局（Linux = `boot_b`、Android = `boot_a`）为例。若你的 Linux 在别的槽，
> 把下文 `boot_b` 替换为 `/boot/armbianEnv.txt` 的 `abl_boot_partition_label`；本包脚本会自动跟随。

## 0. 一页结论

| 组件 | dpkg 管理? | 升级后会发生什么 | 影响 | 能否修补 |
|---|---|---|---|---|
| `linux-image-current-sm8250` | ✅ | **覆盖** `/usr/lib/linux-image-*/qcom/sm8250-xiaomi-elish-csot.dtb`（丢 vmids + 指纹 DTB 节点）、**覆盖 fastrpc.ko**（丢 VMID/SMMU 补丁）；postinst `zz-update-abl-kernel` 会用**包内 DTB**（不是我们改过的）重建 boot 镜像并 **dd 写 Linux 启动槽** | 传感器失效；指纹可能失效；严重时启动异常 | ⚠️ 可修补但**必须手动重做**：新内核源码重打补丁重编模块、重改 DTB、重建 boot 镜像；不能直接 apt 升级后不管 |
| `linux-dtb-current-sm8250` | ✅ | 覆盖 `/boot/dtb-*/.../sm8250-xiaomi-elish-csot.dtb`（vmids/指纹节点丢失） | 同上（DTB 部分） | 可重做 |
| `linux-headers-current-sm8250` | ✅ | 与内核同步升级；旧源码树/模块无法再编译 | 无法重编 fastrpc.ko（版本不匹配） | 需匹配版本源码树 |
| `armbian-firmware-full` | ✅ | 覆盖 `/lib/firmware/.../elish/slpi.mbn`（把**悬空符号链接**换回普通文件）；可能恢复厂商拆份固件/数据；update-initramfs 可能把固件打进 initramfs | 中：普通文件若能被内核加载会触发 auto_boot → 本次开机传感器失效（确定性级联）；当前包的 8380B 拆份头缺段文件，通常不会 auto_boot，且启动脚本会在 +45s 自愈 | ✅ 可修补：升级后立刻恢复悬空符号链接 / 移走拆份固件；建议 hold 或加 dpkg hook |
| `iio-sensor-proxy`（发行版包） | ❌ 未安装（我们用源码 ninja install） | apt upgrade **不会动**；但**一旦安装/重装发行版包**会覆盖 `/usr/libexec/iio-sensor-proxy` 与 unit/dbus 配置 | 传感器数据 0/undefined 或 iio 重启风暴 | ✅ 可修补：重打三处补丁、重编、恢复 D-Bus/drop-in |
| `libssc` | ❌（/usr/local） | 只有手动升级才变；升级可能改 SONAME/API | 中：iio 链接失败（libssc.so.2） | ✅ 重编/回退 0.4.4 |
| `hexagonrpcd`（/opt/hexrpc-032b） | ❌ | 只有手动升级才变；**0.5.x 与我们的内核/用法不兼容**（Unsupported handle） | 致命（attach 失败） | ❌ 不能直接升：必须保持 0.3.2 + warm-attach 补丁 |
| `/usr/share/qcom-ssc-elish` 数据母本 | ❌ | apt 不碰 | 无（但不要手动用新 firmware 数据覆盖） | 保持已验证版本 |
| `/etc` 自定义 unit/drop-in/udev/hook/调参 | ❌（非 dpkg 文件） | systemd/udev/dbus 升级保留 `/etc` | 无 | 若被误删，用包内 install 脚本重装 |
| `gnome-shell`/`gsd-power` | ✅ | claim 时机/行为可能变化 | 低：通常不影响；极端时自动亮度/旋转异常 | ⚠️ 无法打 GNOME 补丁：重启 iio/重编 iio 适配；一般无需保持原样 |
| 指纹/TEE、音频、桌面等无关包 | ✅ | 与传感器无关 | 无（指纹栈自身升级可能影响指纹，属另一个项目） | 与传感器无关 |

## 1. 最高危：内核 / DTB / boot 镜像

本机 boot 流程的关键 hook：`/etc/kernel/postinst.d/zz-update-abl-kernel`。每次
`linux-image-current-sm8250` 安装/升级会：

1. gzip 新内核；
2. 取 **`/usr/lib/linux-image-*-sm8250/qcom/sm8250-xiaomi-elish-${panel}.dtb`**（**dpkg 包内文件**）
   与内核拼成 boot 镜像；
3. `mkbootimg` 生成 `/boot/armbian-kernel-${panel}.img`；
4. **`dd` 直接写入 `/dev/disk/by-partlabel/${abl_boot_partition_label}`（= 本机 `boot_b`；以 armbianEnv 为准）**。

而本机的两项定制恰好都在这些文件上：

- `fastrpc.ko`（VMID 候选 `{3,5}` + 官方 SMMU sensors-PD 修复）→ dpkg 路径
  `/lib/modules/<ver>/kernel/drivers/misc/fastrpc.ko`，升级会被**原厂模块覆盖**；
- `sm8250-xiaomi-elish-csot.dtb`（`qcom,vmids=<3 15 5 6>` + 指纹项目添加的节点）→
  `/usr/lib/linux-image-*/qcom/...dtb` 与 `/boot/dtb-*/qcom/...dtb` 都被 dpkg 管理，
  升级会被**原厂 DTB 覆盖**；
- `zz-update-abl-kernel` 用包内 DTB 重建 boot 镜像 → 即使你改了 `/boot` 里的副本，
  下一次内核升级也会被覆盖。

**后果**：传感器需要的内核补丁全丢（`/dev/fastrpc-sdsp` 不出现或 assign 失败）、DTB VMID 属性丢失、
指纹 DTB 节点丢失；若 boot 镜像重建/写入失败还可能影响启动。

**能否修补**：能，但属于"每次内核升级都必须手动重做"：

1. 备份（升级前）：`sudo ../../install/backup-boot.sh`（自动检测槽位，备份槽镜像 + boot 镜像 + DTB）。
2. 准备**新内核版本**源码树（`apt-get source` 或 Armbian 源码 + 对应配置），
   重打 `../../kernel/patches/0001-fastrpc-sdsp-vmid-and-sensors-pd.patch`（新版本可能需 re-port；
   若官方 SMMU 修复已合入，可只保留 VMID 候选补丁），`make drivers/misc/fastrpc.ko`，
   覆盖安装 + `depmod`。
3. 重新给 DTB 加 `qcom,vmids`（见 `../../kernel/dtb/README.md`）；**不要用 dtc 往返**。
   指纹 DTB 节点属于指纹项目，需同步重做（否则指纹失效）。
4. 确认 `/usr/lib/linux-image-*/qcom/sm8250-xiaomi-elish-csot.dtb` 已是你改过的版本，
   再让内核 postinst 重建 boot 镜像（或手动 `mkbootimg` + `dd` 到 boot_b），并回读 `cmp` 校验。
5. 检查 initramfs 不应包含可加载的 SLPI 固件（见 §2）。重启后跑 `install/verify.sh`。

**建议**：不追求新内核就 **hold 内核三件套**：

```bash
sudo apt-mark hold linux-image-current-sm8250 linux-dtb-current-sm8250 linux-headers-current-sm8250
```

（注意：这会同时冻结安全更新；若升级，请按上面流程完整重做。）

## 2. 中危：`armbian-firmware-full`（固件/厂商数据）

升级会覆盖 `/lib/firmware/qcom/sm8250/xiaomi/elish/`：

- 我们精心维护的 `/lib/firmware/.../slpi.mbn` **悬空符号链接**（→ `/run/sensor-fw/slpi.mbn`）
  会被 dpkg 恢复成**普通文件**（包内是 8380B 的拆份头；本机包内没有 `slpi.b00..b20` 段文件）；
- `sensors/`、`socinfo/` 等厂商数据会被更新（**但我们的运行时用的是自己的
  `/usr/share/qcom-ssc-elish` 副本，不受影响**）。

**风险机制**：若该普通文件能被内核在开机早期加载（例如未来版本包同时提供拆份段文件，
或 initramfs 里带上了可加载固件）→ 内核 boot+5s 抢先 auto_boot SLPI →
监听器来不及 → PD 看门狗崩溃 → 本次开机传感器永久失效（见 `NOTES.md` §1 的确定性级联）。

**当前包的安全性**：包内仅 8380B 拆份头、缺少段文件 → 内核加载失败，通常**不会** auto_boot；
而且启动脚本开头会检测"普通文件"并移走、重建符号链接，再从 `/run` 单文件固件启动（自愈）。

**升级后立即检查/修复**（重启前！）：

```bash
ls -l /lib/firmware/qcom/sm8250/xiaomi/elish/slpi.mbn
# 必须是： symbolic link -> /run/sensor-fw/slpi.mbn
# 若是普通文件：
sudo mv -f /lib/firmware/qcom/sm8250/xiaomi/elish/slpi.mbn /run/sensor-fw/leaked-slpi-$(date +%s).mbn
sudo ln -sfn /run/sensor-fw/slpi.mbn /lib/firmware/qcom/sm8250/xiaomi/elish/slpi.mbn
# 若出现 slpi.b* 段文件，也移入 slpi-disabled/
sudo mkdir -p /lib/firmware/qcom/sm8250/xiaomi/elish/slpi-disabled
sudo mv -f /lib/firmware/qcom/sm8250/xiaomi/elish/slpi.b* /lib/firmware/qcom/sm8250/xiaomi/elish/slpi-disabled/ 2>/dev/null || true
```

还可加 **dpkg post-invoke hook** 自动修复（可选，放在 `/etc/dpkg/dpkg.cfg.d/` 或
`/etc/apt/apt.conf.d/99sensor-fw-fix`），逻辑同上。若不想每次处理：
`sudo apt-mark hold armbian-firmware-full`。

## 3. 中低危：iio-sensor-proxy / libssc / hexrpc（非 dpkg，apt 不动）

- 三者都**不在 dpkg 数据库**（`/usr/libexec/iio-sensor-proxy`、`/usr/local/lib/.../libssc.so.2`、
  `/opt/hexrpc-032b/`），`apt upgrade` **不会**碰它们。
- 但注意：
  - **不要 `apt install iio-sensor-proxy`**：发行版包会覆盖补丁二进制和 unit/dbus 配置。
    若误装：卸掉包 → 按 `userspace/iio-sensor-proxy/README.md` 重编安装 → 恢复
    `systemd/dbus/net.hadess.SensorProxy.service`（必须含 `SystemdService=`）与 drop-in。
  - **不要把 hexrpc 升到 0.5.x**：0.5 会 `Unsupported handle`；保持 0.3.2 + warm-attach 补丁。
  - libssc 保持 **0.4.4**（`libssc.so.2`）；若要升级，需重编 iio 并重新验证。
- 依赖库升级（`libglib/gio/gudev/polkit/libqmi-glib.so.5/libqrtr-glib.so.0`）：
  Ubuntu 稳定版通常不改 SONAME，风险低；若 SONAME 变了 → iio/libssc 加载失败 → 重编即可修复。

## 4. 低危 / 无关

- `systemd`/`udev`/`dbus`：我们的自定义配置在 `/etc` 与 `/usr/share`（非 dpkg），升级保留；
  若 `iio-sensor-proxy` 包被你手动安装，才会覆盖 `/usr/share/dbus-1/system-services/...`。
- GNOME/gsd：一般无需处理；若升级后自动亮度/自动旋转异常，先 `sudo systemctl restart iio-sensor-proxy`，
  仍异常则按补丁版重编 iio；无法直接给 GNOME 打补丁。
- 指纹/TEE（qsee-supplicant、qsee-app-loader、fprintd/libfprint）、音频、显卡、网络等：与传感器无关。
  （注意：指纹栈自身升级可能影响指纹解锁，属另一个项目的风险。）

## 5. "必须保持原样" vs "可以修补/重做"

### 必须保持原样（不要升级/替换）
1. **hexagonrpcd = 0.3.2 + warm-attach**（不能换 0.5.x；不能换成发行版二进制）。
2. **iio-sensor-proxy = 我们的补丁版**（不能换发行版包二进制）。
3. **fastrpc.ko = 用补丁源码针对当前内核编译的版本**（不能用发行版原厂模块）。
4. **`qcom_q6v5_pas` = 原厂行为**（`auto_boot=true`；不要用实验性的 auto_boot=false 模块/源码）。
5. **DTB 的定制属性**（`qcom,vmids` + 指纹节点）与 **boot_b 镜像的定制内容**（内核升级会覆盖）。
6. **固件路径不变量**：`/lib/firmware/.../slpi.mbn` 必须是悬空符号链接（防内核 auto_boot）。
7. **数据母本 `/usr/share/qcom-ssc-elish`** 的当前已验证版本（不要被新 firmware 数据覆盖；
   若一定要更新，先备份并复测）。

### 可以修补 / 重做（升级后按步骤恢复）
1. 内核升级 → 新源码重打 fastrpc 补丁、重编模块、重做 DTB/boot 镜像（工作量较大）。
2. `armbian-firmware-full` 升级 → 恢复符号链接/移走拆份固件（可自动化）。
3. iio-sensor-proxy 被覆盖 → 重打三补丁、重编、恢复 D-Bus/drop-in。
4. libssc/依赖库 SONAME 变化 → 重编 libssc 与 iio。
5. 系统配置被误删 → 用包内 `install/install-system-files.sh` 重装。
6. initramfs 含固件 → `sudo update-initramfs -u` 前先确保 `/lib/firmware/.../slpi.mbn` 是悬空链接，
   或加 initramfs hook 排除 `slpi.mbn`；用 `lsinitramfs /boot/initrd.img-$(uname -r) | grep slpi` 检查。

## 6. 升级操作清单（建议照做）

**升级前**
```bash
# 1) 备份 Linux 启动槽 / boot 镜像 / DTB
sudo ../../install/backup-boot.sh
# 2) 备份数据母本与配置
sudo tar -czf /root/sensor-data-backup-$(date +%Y%m%d).tar.gz /usr/share/qcom-ssc-elish /etc/sensor-tuning.conf
# 3) 记录基线
sensor-status > /tmp/sensor-baseline.txt; sudo dmesg > /tmp/dmesg-baseline.txt
# 4) 看本文件的风险表，决定 hold 或重做
```

**升级后（按顺序）**
```bash
# 1) 固件路径不变量
ls -l /lib/firmware/qcom/sm8250/xiaomi/elish/slpi.mbn   # 必须是悬空符号链接
# 2) 内核模块是否仍是补丁版
modinfo -F vermagic /lib/modules/$(uname -r)/kernel/drivers/misc/fastrpc.ko
strings /lib/modules/$(uname -r)/kernel/drivers/misc/fastrpc.ko | grep 'SDSP assign try'
# 3) DTB vmids
fdtget -t x /usr/lib/linux-image-$(uname -r)/qcom/sm8250-xiaomi-elish-csot.dtb \
  /soc@0/remoteproc@5c00000/glink-edge/fastrpc qcom,vmids
# 4) 一键检查（本包提供）
./install/check-upgrade.sh
# 5) 重启后验证
./install/verify.sh
```

**建议的 hold（若近期不想折腾内核/固件）**
```bash
sudo apt-mark hold linux-image-current-sm8250 linux-dtb-current-sm8250 \
  linux-headers-current-sm8250 armbian-firmware-full
# 撤销： sudo apt-mark unhold ...
```

## 7. 升级频率与自动化（2026-11-11 观察）

### 7.1 关键结论：这些包**不会自动升级**

- 本机 `unattended-upgrades` 的 `Allowed-Origins` 只包含 Ubuntu 的 `noble` / `noble-security`
  （未包含 Armbian 源），因此 `apt.armbian.com` 提供的内核/固件包**不会**被自动安装升级；
- `apt-daily.timer` 只每天刷新索引，`apt-daily-upgrade.timer` 只处理允许的 Ubuntu 安全更新；
- 也就是说：**内核三件套与 `armbian-firmware-full` 的实际升级频率 = 你手动执行
  `apt upgrade` / `armbian-update` 的频率**。不动它，频率就是 0。

### 7.2 Armbian 稳定仓库的发布节奏（推断 + 本机证据）

| 证据 | 内容 |
|---|---|
| 版本号 | 本机内核/固件包版本 `26.8.3`；`/etc/armbian-image-release` 初始镜像 `25.2.2`，`/etc/armbian-release` 为 `26.2.1` |
| 内核构建时间 | `/boot/config-6.18.44-current-sm8250` 时间戳 **2026-08-15**，即 26.8 周期构建 |
| 本机安装时间 | `linux-image` 的 dpkg 记录 **2026-09-09**；固件包 **2026-11-06** |
| 当前仓库状态 | 2026-11-11：`apt-cache policy` 显示这些包 installed = candidate = **26.8.3**，`apt list --upgradable` 无内核/固件可升级项 |
| 下一版本 | 26.11 处于 trunk 测试（最新 weekly digest 2026-11-09；trunk 构建的内核已到 6.18.60），稳定版通常随后发布 |

Armbian 稳定版节奏约 **每季度一次**（2/5/8/11 月），周期内会有 **point release**（26.8.0→26.8.3
说明至少 3 次点版本）。因此若你每次 stable 升级都跟进：

- **内核三件套**（image/dtb/headers）：约 **4 次/年**（每季度一次）会需要重编模块 + 重做
  DTB/boot 镜像；若某个周期内核源码未变，仍会因包版本变化覆盖 fastrpc.ko/DTB；
- **armbian-firmware-full**：同样约 **4 次/年**，但多数情况启动脚本会自愈（普通文件→悬空链接），
  最坏情况需要在重启前手动修复；
- 每个季度内若出现 point release 且包含内核/固件更新，可能再多 1–2 次；
- **内核 minor 跳变**（6.18→6.19/7.x）更少（约半年~一年一次），但一旦发生，fastrpc 补丁可能
  需要 re-port，工作量最大。

### 7.3 估算表

| 组件 | apt 自动升级 | 若跟随 stable 升级的频率 | 每次需要做什么 | 保持不动时风险 |
|---|---|---|---|---|
| linux-image-current-sm8250 | ❌ 否 | ~4 次/年 | 重编 fastrpc.ko、重做 DTB/boot 镜像、重启验证 | 0（但无安全更新） |
| linux-dtb-current-sm8250 | ❌ 否 | ~4 次/年（常与内核同时） | 重加 vmids/指纹 DTB 节点 | 0 |
| linux-headers-current-sm8250 | ❌ 否 | ~4 次/年 | 用于匹配版本重编模块 | 0 |
| armbian-firmware-full | ❌ 否 | ~4 次/年（+point） | 恢复固件符号链接/移走拆份固件（脚本多数自愈） | 0 |
| iio/hexrpc/libssc（非 dpkg） | ❌ 不受 apt 影响 | 0（除非手动） | — | 0 |
| 依赖库（glib/qmi/qrtr 等） | ✅ 但仅 Ubuntu 源 | 安全更新不定期 | SONAME 变化才需重编 | 极低 |

### 7.4 建议

- **不想折腾**：`sudo apt-mark hold linux-image-current-sm8250 linux-dtb-current-sm8250
  linux-headers-current-sm8250 armbian-firmware-full`，把升级频率降为 0；每季度看一次
  `apt list --upgradable`，有空再按 §1/§2 流程升级。
- **想跟进 stable**：按季度批量处理，升级前备份 Linux 启动槽/DTB/数据，升级后跑
  `install/check-upgrade.sh` + `install/verify.sh`。
- 可在日历/提醒里设"每季度检查 Armbian 内核/固件更新"，避免错过 26.11/27.2 等版本。
