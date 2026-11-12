# 合并升级影响评估（传感器 + 指纹 + GNSS）

> 针对"多套功能共用一次内核/DTB/boot 镜像"的合并包。传感器侧详细频率分析见
> `../sensors/docs/UPGRADE-IMPACT.md`；本文件补充指纹与合并影响。

## 1. 升级影响总表

| 组件（apt 管理） | 升级后影响 | 传感器 | 指纹 | 能否修补 |
|---|---|---|---|---|
| `linux-image-current-sm8250` | 覆盖 fastrpc.ko、QSEECOM/mdt_loader/qcom_scm、goodix 模块目录、`/usr/lib/linux-image/*/qcom/*.dtb`；postinst 用**包内 DTB**重建 boot 镜像并写 boot_b | 🔴 失效 | 🔴 失效 | ✅ 必须手动重做统一内核 + 联合 DTB + boot 镜像（`kernel/README.md`） |
| `linux-dtb-current-sm8250` | 覆盖 `/boot/dtb-*` 下 DTB（丢 vmids/指纹节点/**GNSS 的 QUP5 UART+bcm4775**） | 🔴 | 🔴 | ✅ 重做联合 DTB（GNSS 也会失效：无 `/dev/ttyHS1`） |
| `linux-headers-current-sm8250` | 随内核升级；旧源码树/模块无法重编 | 🟠 | 🟠 | 需匹配版本源码 |
| `armbian-firmware-full` | 把 SLPI 固件路径从悬空符号链接恢复为普通文件；可能恢复拆份固件 | 🟠 可能 auto_boot（启动脚本多数自愈） | 🟢 无影响 | ✅ 恢复符号链接/移走拆份固件 |
| `libfprint-2-2` | **覆盖自编译 libfprint**（goodixqsee 驱动消失） | 🟢 | 🔴 指纹设备消失 | ✅ 重新编译安装；建议 hold/blacklist |
| `fprintd` / `libpam-fprintd` | 可能改变 fprintd 沙箱/PAM 行为；升级后需重启 fprintd | 🟢 | 🟠 需检查 drop-in/PAM | ✅ 恢复 drop-in、重测 PAM |
| `sudo` 包 | `/etc/pam.d/sudo` 是 conffile；升级可能提示保留本地版本（**必须保留我们的指纹栈**） | 🟢 | 🟠 处理不当会回退密码 sudo | ✅ 检查/恢复 PAM |
| `iio-sensor-proxy`（发行版包） | 当前**未安装**（自编译）；误装会覆盖 | 🔴 若覆盖 | 🟢 | ✅ 重编 |
| `gnome-shell`/`gdm` | 指纹 UI/claim 行为可能变化 | 🟠 低（iio 补丁兜底） | 🟠 低（GDM 指纹入口可能变） | ⚠️ 无源码补丁，通常可重启恢复 |
| `hexagonrpc`/`libssc`/qsee-supplicant | 非 dpkg，apt 不动 | 🟢 | 🟢 | — |

## 2. 升级频率与自动化（实测/推断）

- **Armbian 源**（内核三件套、armbian-firmware-full）：`unattended-upgrades` 的 `Allowed-Origins`
  只含 Ubuntu 的 `noble`/`noble-security`，**不会自动升级 Armbian 包**；实际频率 = 你手动
  `apt upgrade`/`armbian-update` 的频率。Armbian 稳定版约**每季度**（2/5/8/11 月），
  周期内可能有 point release；每次内核/DTB 升级都需重做全部内核补丁（预计约 4 次/年，若 hold 则 0）。
- **Ubuntu 源**（`libfprint-2-2`、`fprintd`、`libpam-fprintd`、`sudo`）：会随 Ubuntu 安全/稳定更新
  进入 `noble-updates`/`noble-security`，`unattended-upgrades` **可能自动安装**（尤其安全更新）。
  频率不固定（通常每次更新周期 1–3 次/年，取决于上游）。其中 **libfprint-2-2 被自动升级会直接让指纹失效**。
- 依赖库（glib/gudev/qmi/qrtr/polkit）升级一般保持 SONAME；若变更需重编 iio/libssc/libfprint。

## 3. 建议：hold / 黑名单

不想被自动升级破坏：

```bash
# Armbian 内核/固件（可选；冻结内核安全更新）
sudo apt-mark hold linux-image-current-sm8250 linux-dtb-current-sm8250 \
  linux-headers-current-sm8250 armbian-firmware-full
# 自编译 libfprint 必须防 apt 覆盖（强烈建议）
sudo apt-mark hold libfprint-2-2
# 或让 unattended-upgrades 跳过（等价效果）：
# /etc/apt/apt.conf.d/51unattended-upgrades-fp：
#   Unattended-Upgrade::Package-Blacklist { "libfprint-2-2"; };
```

`fprintd`/`libpam-fprintd` 通常可升级，但升级后要重测；若遇到 PAM 行为变化再 hold。

## 4. 升级前后检查清单

**升级前**
```bash
sudo dd if=/dev/disk/by-partlabel/boot_b of=/root/boot_b-backup-$(date +%Y%m%d).img bs=1M status=progress
sudo cp -a /boot/armbian-kernel-csot.img /root/
sudo cp -a /usr/lib/linux-image-$(uname -r)/qcom/sm8250-xiaomi-elish-csot.dtb /root/
sudo cp -a /etc/pam.d/sudo /root/sudo.pam.bak-$(date +%Y%m%d)
sudo tar -czf /root/sensor-data-backup-$(date +%Y%m%d).tar.gz /usr/share/qcom-ssc-elish 2>/dev/null || true
sensor-status > /tmp/sensor-baseline.txt
./install/check-all.sh > /tmp/all-baseline.txt
```

**升级后（按顺序）**
```bash
# 1) 内核是否还是带补丁版本 + DTB 是否仍含两项目节点
./kernel/dtb/verify-combined-dtb.sh
modinfo -F vermagic /lib/modules/$(uname -r)/kernel/drivers/misc/fastrpc.ko
strings /lib/modules/$(uname -r)/kernel/drivers/misc/fastrpc.ko | grep 'SDSP assign try'
ls -l /lib/firmware/qcom/sm8250/xiaomi/elish/slpi.mbn   # 必须是悬空符号链接
# 2) 指纹 libfprint/TA/服务/drop-in
strings /usr/lib/aarch64-linux-gnu/libfprint-2.so.2.0.0 | grep -q 'Goodix QSEE bring-up command' && echo libfprint-OK
systemctl status qsee-supplicant 'qsee-app-loader@gfenu' fprintd fp-ta-watchdog --no-pager
# 3) initramfs 不得含 SLPI 固件（hook 被升级覆盖时要重装修复版）
lsinitramfs /boot/initrd.img-$(uname -r) | grep 'elish/slpi' && echo 'ERROR: 需要 kernel/initramfs/install-hook.sh'
# 4) 合并自检
./install/check-all.sh
```

## 5. 回退

| 场景 | 回退 |
|---|---|
| 内核/启动镜像 | `sudo dd if=/root/boot_b-backup-*.img of=/dev/disk/by-partlabel/boot_b`（只写 boot_b） |
| 指纹 libfprint | 重编安装，或 `sudo apt install --reinstall libfprint-2-2` 后再重编 |
| PAM | 恢复 `/root/sudo.pam.bak-*` 或 `/root/fp-pam-backup-*/sudo` |
| 传感器固件链接 | 移走普通文件、重建悬空符号链接（见 `../NOTES.md`） |
| 整体禁用 | 传感器 `touch /etc/disable-sensors`；指纹 `fingerprint/install/uninstall-fingerprint.sh` |

## 6. 合并包的额外注意

- 内核/DTB 重建后**必须同时验证各套功能**（`kernel/dtb/verify-combined-dtb.sh` + `sensors/verify.sh`
  + `fingerprint/verify-fingerprint.sh`），不要只验证一边。
- 升级后 boot 镜像必须包含**联合 DTB**；如果只保留指纹节点或只保留 vmids，另一套功能会失效。
- `zz-update-abl-kernel` 会自动重建并写 boot_b（危险）；升级内核前先备份，升级后立即检查。
- 内核升级后 Armbian 补丁集可能随版本变化，`kernel/ARMBIAN-BASELINE.md` 说明如何获取 6.18.44 基线；
  新内核需要对应版本补丁（可能需 re-port）。

## 7. GNSS（Airoha AG3335，2026-11-12 新增）

| 项 | 影响 |
|---|---|
| `linux-image-current-sm8250` / `linux-dtb-current-sm8250` | 🔴 覆盖 `/usr/lib/linux-image/*/qcom/*.dtb`，丢失 **QUP5 UART（`ioc@994000`→`qcom,geni-uart`）与 `/bcm4775`** 节点 → `/dev/ttyHS1`、`/dev/airoha_gps` 消失，GNSS 失效。必须用本包 DTB 重建（`kernel/update-boot-image.sh` + `kernel/dtb/verify-combined-dtb.sh`）。 |
| `linux-image-current-sm8250` | 🟠 `airoha_gps.ko` 与内核 ABI 绑定，升级后需按 `gnss/kernel/README.md` 重编（无内核源码补丁，只依赖公共 API）。 |
| `airoha-gnssd` / `gpsd` / gpsd 配置 | 🟢 用户态，apt 升级内核不影响；gpsd 升级一般兼容（读取 `/dev/ttyGNSS0`）。 |
| `geoclue-2.0` | 🟠 升级可能重置 `/etc/geoclue/geoclue.conf` 的 `nmea-socket`（conffile）；升级后重跑 `gnss/install/install.sh` 或手动恢复，并检查 `gps-nmea-share`/`geoclue` 服务。 |
| `iio-sensor-proxy` | 🟠 若重装/升级覆盖为发行版包，会丢失自定义补丁且缺少本包 polkit 规则；GeoClue heading 失效。重跑 `gnss/install/install.sh` 恢复 polkit 规则，并参考 `sensors/` 重打 iio 补丁。 |
| GNSS 厂商固件（`ag3335_bootloader.bin` 等） | 🟢 在设备 GNSS flash 内，不随 apt 升级、不需要重刷；**不要为了定位重刷 flash**。 |
| 检查 | `gnss/install/verify.sh`、`kernel/dtb/verify-combined-dtb.sh`、`gpspipe -w -n 5`；升级后先跑 `install/check-all.sh`。 |

