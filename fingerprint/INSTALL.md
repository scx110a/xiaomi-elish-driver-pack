# 指纹子包安装手册

> 本子包与传感器子包共用同一个内核/DTB/boot 镜像。**先完成 `../kernel/README.md` 的统一下载/编译/安装**，
> 再按本文件安装指纹用户态。合并包总览见 `../README.md`。
>
> **一键安装器已自动完成 TA 提取、qsee-supplicant/libfprint/fprintd 的编译安装与系统集成**
> （`../install.sh` / `../install/build-userspace.sh`）；下面是手动等价步骤。
> 危险点先读 `NOTES.md`：内核 ABI 三件套、CMA、TA 拒服务、libfprint 被 apt 覆盖、PAM。

## 0. 前置与备份

```bash
uname -r                                    # 6.18.44-current-sm8250
cat /sys/firmware/devicetree/base/model     # Xiaomi Mi Pad 5 Pro (CSOT)
LABEL=$(sed -n "s/^abl_boot_partition_label=//p" /boot/armbianEnv.txt)
ls -l /dev/disk/by-partlabel/$LABEL         # Linux 启动分区（本机 boot_b；按 armbianEnv 自动）
# 备份 Linux 启动槽 / boot 镜像 / DTB
sudo dd if=/dev/disk/by-partlabel/$LABEL of=/root/${LABEL}-backup-$(date +%Y%m%d).img bs=1M status=progress
sudo cp -a /boot/armbian-kernel-csot.img /root/
sudo cp -a /usr/lib/linux-image-$(uname -r)/qcom/sm8250-xiaomi-elish-csot.dtb /root/
```

要求：kernel 已按 `../kernel/README.md` 构建并重启，以下设备存在：
`/dev/tee0`、`/dev/teepriv0`、`/dev/goodix_fp`、`/sys/devices/platform/fingerprint/firmware_name`。

## 1. 内核层（由 `../kernel/` 统一完成）

所需内容（已在 `../kernel/` 提供）：

| 内容 | 说明 |
|---|---|
| `qseecom/` | QSEECOM TEE 驱动（`/dev/tee0`、`/dev/teepriv0`） |
| `mdt_loader.c` | 带 ELF64 支持的 mdt_loader |
| `qcom_scm.c` | 含 `qcom_scm_qseecom_*` |
| `goodix_fp_spi/` | 指纹传感器模块源码 |
| `config-used-6.18.44` | 实际编译配置（TEE/QSEECOM/BTF 等） |
| `ARMBIAN-BASELINE.md` | Armbian sm8250 基线获取说明（见 `../kernel/`） |
| `dtb/` | 指纹 DT 片段 + 联合 DTB 校验说明 |

本步只确认结果：

```bash
sudo dmesg | grep -i qseecom        # 期望 found qseecom with version 0x1402000
ls -l /dev/tee0 /dev/teepriv0 /dev/goodix_fp
cat /sys/devices/platform/fingerprint/firmware_name   # 期望 gfenu（TA 文件尚未放也行）
lsmod | grep goodix_fp_spi
```

若 `goodix_fp_spi` 未加载：`sudo echo goodix_fp_spi | sudo tee /etc/modules-load.d/goodix-fp.conf`
并在 `../kernel/README.md` 的模块安装步骤确认模块位于 `/lib/modules/$(uname -r)/extra/`。

## 2. 提取并安装 TA（gfenu）

TA 是厂商签名固件，**本包不附带**，从你的设备提取：

```bash
./extract/extract-gfenu-ta.sh --adb ./extracted/fingerprint-ta
# 或从 firmware_mnt.img： ./extract/extract-gfenu-ta.sh /path/firmware_mnt.img ./extracted/fingerprint-ta
# 校验 9 个 sha256 全 OK 后：
sudo cp ./extracted/fingerprint-ta/gfenu.* /lib/firmware/
sudo chmod 0644 /lib/firmware/gfenu.*
```

**不要用 `fingerpr.*`**（错误 TA）。TA 名字必须与 DTB `/fingerprint` 的 `firmware-name` 一致（gfenu）。

## 3. qsee-supplicant / qsee-app-loader

```bash
cd src/qsee-supplicant
make
make check
sudo make install-bin install-systemd install-doc
sudo systemctl daemon-reload
sudo systemctl enable --now qsee-supplicant.service
systemctl status qsee-supplicant --no-pager     # active，Listener services registered
```

> 不要用 `make install`（会装 openrc 脚本，Ubuntu 上 `systemctl enable` 可能走错路径）。

## 4. udev 自动拉起 TA loader

由 `install/install-fingerprint-files.sh` 安装 `60-goodix-qsee.rules`，或手动：

```bash
sudo install -m 0644 system/udev/60-goodix-qsee.rules /etc/udev/rules.d/
sudo udevadm control --reload
sudo udevadm trigger --action=add --subsystem-match=misc --sysname-match=goodix_fp
systemctl status 'qsee-app-loader@gfenu' --no-pager   # active，application_loaded app=gfenu
sudo dmesg | grep "loaded 'gfenu'"                     # loaded 'gfenu' as app N
```

## 5. 编译安装 libfprint（goodixqsee）

依赖：`build-essential meson ninja-build pkg-config libglib2.0-dev libgusb-dev
libusb-1.0-0-dev libgudev-1.0-dev libpixman-1-dev libcairo2-dev
libgirepository1.0-dev libssl-dev`。

```bash
git clone -b goodix-qsee https://github.com/wrobelda/libfprint.git
cd libfprint
git checkout 430e24e538ba137b6edcfe2e1b3b8cc29c66fa6a
patch -p1 < <本包>/src/libfprint/patches/libfprint-goodixqsee-logging.patch
meson setup build -Ddrivers=goodixqsee -Ddoc=false -Dinstalled-tests=false \
      -Dgtk-examples=false -Dintrospection=true --prefix=/usr
ninja -C build
sudo ninja -C build install
sudo ldconfig
```

验证：`strings /usr/lib/aarch64-linux-gnu/libfprint-2.so.2.0.0 | grep -m1 1.94`；
`/usr/lib/aarch64-linux-gnu/libfprint-2.so.2.0.0` 存在。日志补丁是看门狗依赖（可跳过，但看门狗失效）。
> `apt upgrade` 升级 `libfprint-2-2` 会覆盖自编译库 → 指纹消失；升级后重新 `ninja -C build install`。

## 6. fprintd 与设备白名单

```bash
sudo apt install -y fprintd libpam-fprintd
sudo install -m 0644 system/systemd/fprintd-60-goodix-qsee.conf \
     /etc/systemd/system/fprintd.service.d/60-goodix-qsee.conf
sudo systemctl daemon-reload && sudo systemctl restart fprintd
fprintd-list "$USER"     # 期望 Goodix QSEE match-on-chip sensor
```

## 7. GDM 锁屏指纹

Ubuntu 24.04 自带 `/etc/pam.d/gdm-fingerprint`；确认设置：

```bash
gsettings get org.gnome.login-screen enable-fingerprint-authentication   # true
```

`Super+L` 锁屏 → 密码框旁出现指纹图标 → 按压解锁。

## 8. 安装系统集成（udev/systemd/脚本）

```bash
sudo ./install/install-fingerprint-files.sh          # 基础：udev + 服务 + 脚本
# 可选：sudo ./install/install-fingerprint-files.sh --user   # 给 SUDO_USER 装 sudo 弹窗 watcher
```

## 9. sudo 指纹（可选，安全敏感）

```bash
# 1) 检查并停用免密 sudo（存在 NOPASSWD 时 sudo 指纹会被绕过；本包不附带该文件）：
#    sudo -n -l 2>/dev/null | grep -i nopasswd
#    ls /etc/sudoers.d/
sudo mv /etc/sudoers.d/020-nopasswd /etc/sudoers.d/020-nopasswd.disabled
sudo visudo -c
# 2) 安装指纹 PAM（脚本会自动备份原 /etc/pam.d/sudo 到 /root/fp-pam-backup-*）：
sudo ./install/install-fingerprint-files.sh --pam --user
# 3) 立即测试（另开一个 TTY 保持 root 会话以防锁死）：
sudo true
```

PAM 行为：指纹优先、`max-tries=1 timeout=8`，8 秒内未按/失败则回退密码；
`--user` watcher 负责弹窗提示（zenity），认证成功立即关闭。

## 10. TA 看门狗（自动恢复拒服务）

```bash
sudo systemctl enable --now fp-ta-watchdog.service
# 日志：/var/log/fp-ta-watchdog.log；命中特征后重启 qsee-app-loader@gfenu（冷却 45s）
```

## 11. 验收

```bash
./install/verify-fingerprint.sh "$USER"
# 逐项：服务 active、/dev/goodix_fp+/dev/tee*、firmware_name=gfenu、TA 9 文件、
# goodix 模块、fprintd drop-in、qsee 二进制、libfprint 补丁、CMA、模板
fprintd-enroll -f left-index-finger "$USER"     # 按提示 5–25 次
fprintd-verify "$USER"                          # verify-match
```

完整验收（含 sudo/锁屏）见 `README.md` 的验收节。

## 12. 回退

- **PAM**：恢复 `/root/fp-pam-backup-*/sudo` 到 `/etc/pam.d/sudo`；若曾停用 NOPASSWD，按需恢复。
- **内核/启动镜像**：`LABEL=$(sed -n "s/^abl_boot_partition_label=//p" /boot/armbianEnv.txt); sudo dd if=/root/${LABEL}-backup-*.img of=/dev/disk/by-partlabel/$LABEL`（旧备份名可能为 `boot_b-backup-*`），
  或 `/boot/armbian-kernel-csot.img` 备份。
- **libfprint**：`sudo apt install --reinstall libfprint-2-2 && sudo systemctl restart fprintd`。
- **服务/配置**：`sudo ./install/uninstall-fingerprint.sh`（保留模块/TA/libfprint）。

## 13. 升级注意

- **内核升级**：本包的内核改动（QSEECOM/mdt_loader/qcom_scm/goodix 模块 + 联合 DTB + fastrpc 补丁）
  会在 `apt upgrade` 内核时被覆盖；需按 `../kernel/README.md` 重做，且 boot 镜像会丢失指纹 DT 节点。
- **libfprint/fprintd/PAM 包升级**：libfprint 必须重编；fprintd 升级后检查 drop-in 是否保留。
- **TA 不需要升级**，但换 Android 固件版本后需重新提取并核对 sha256。
