# 合并包注意事项（传感器 + 指纹 + GNSS 共同必读）

> 子包细节：`sensors/NOTES.md`（传感器专属）、`fingerprint/NOTES.md`（指纹专属）、`gnss/NOTES.md`（GNSS 专属）。
> 升级影响：`docs/UPGRADE-IMPACT-ALL.md` 与 `sensors/docs/UPGRADE-IMPACT.md`。
> **硬件驱动状态以 `docs/HARDWARE-STATUS.md` 为权威基线**（摄像头仍未驱动；GNSS 已于 2026-11-12 打通）。

## 0. 红线

1. **boot_a 是 Android 启动分区，绝对不要写**。只操作 rootfs 和 boot_b（Linux 侧）。
2. 改内核/启动镜像/DTB 前必须备份 Linux 启动槽（本机 `boot_b`，按 armbianEnv）、`/boot/armbian-kernel-csot.img`、DTB。
3. **不要用 `dtc` 反编译-重编译改 DTB**：会破坏 `regulators-1/ldo10`（L10C 3.3V）的 4 字节调压属性；
   L10A 是 `regulators-0/ldo10`（3.008V，指纹供电），不在该问题范围。改 DTB 请用等长二进制替换或完整源码重编。
4. **不要用当前 `/var/tmp/kbuild-src` 的 DTS 重建 DTB**：该树不完整（会丢 17 节点/109 属性）；
   直接用包内已验证 DTB（`kernel/dtb/`）。
5. 不要用 edge 内核；不要关 `CONFIG_DEBUG_INFO_BTF`；不要改 `CONFIG_LOCALVERSION`；
   不要用实验性的 `qcom_q6v5_pas auto_boot=false`（必须 upstream `true`）。
6. 指纹 PAM 改动前确保还有密码/TTY 登录通道；**不要重新启用 NOPASSWD sudo**。
7. 不要用 `fingerpr` TA（错误），必须 `gfenu`。
8. 本机若存在 `/etc/sudoers.d/*nopasswd*`，sudo 指纹会被绕过；要真正启用指纹 sudo，请先停用 NOPASSWD（本包不附带该文件）。
9. **GNSS：不要为了定位重刷 GNSS flash**。芯片从 flash 自行启动；串口必须 **921600**，
   上电后 **~1 秒内**必须发 `$PAIR002`。刷写流程仅用于修复 flash（风险高，见 `gnss/extract/README.md`）。

## 1. 统一内核/DTB/boot 是各功能的共同基础

- 内核需同时满足：传感器 fastrpc 两处补丁（SDSP VMID `{3,5}` + sensors-PD remote heap）；
  指纹 QSEECOM/mdt_loader(ELF64)/qcom_scm + goodix_fp_spi；GNSS 的 `airoha_gps.ko`
  （独立模块，不涉及内核源码补丁）。
- 联合 DTB 需同时含：`/soc@0/remoteproc@5c00000/glink-edge/fastrpc` 的 `qcom,vmids=<3 15 5 6>`；
  根 `/fingerprint` 节点（`compatible="goodix,gf3626"`、`firmware-name="gfenu"`、GPIO 23/125、L10A）；
  GNSS 的 QUP5 UART（`i2c@994000` → `qcom,geni-uart`，`status=okay`）与根 `/bcm4775` 节点。
- 一次构建、一次写 Linux 启动槽（自动检测）。校验：`kernel/dtb/verify-combined-dtb.sh`。
- **内核升级会覆盖全部改动**（模块、DTB、boot 镜像），传感器/指纹/GNSS 会同时失效；详见升级文档。
- `zz-update-abl-kernel` 会用**包内 DTB**（不是你的修改版）重建 boot_b；内核升级后必须手动重做联合 DTB。

## 1b. LPASS pinctrl（pogo 键盘/USB，独立补丁）

- 统一内核还应用 `kernel/patches/0002-pinctrl-lpass-lpi-defer-on-clock-timeout.patch`：
  ADSP 未就绪时 pinctrl 返回 `-EPROBE_DEFER` 而非硬失败，避免开机随机丢 pogo 键盘。
- 该补丁的模块在 **initramfs** 里：重建内核后必须 `sudo update-initramfs -u -k $(uname -r)`，
  再用系统 hook（`zz-update-abl-kernel`）重建 boot 镜像；`install-new-kernel.sh` 复用旧 ramdisk，
  不适用于 initramfs 已更新的场景。详见 `kernel/PINCTRL-LPASS.md`。
- 本机发行版 26.8.3 模块已含同样修复；但本机源码树不含，统一重建会丢，所以补丁随包。

## 1c. GNSS（Airoha AG3335，独立模块 + 用户态）

- DTB 只需保留 QUP5 UART 与 `/bcm4775` 节点（本包 `kernel/dtb/` 已包含，校验：
  `kernel/dtb/verify-combined-dtb.sh`）。
- 内核侧只有 `airoha_gps.ko`（电源/时钟/pinctrl，无内核源码补丁）；用户态 `airoha-gnssd`
  负责上电后 1 秒窗口内的 `$PAIR002` 与 NMEA→pty 转发。
- **不要重刷 GNSS flash**；串口 921600；室内无星正常。细节与参考实现见 `gnss/README.md`、
  `gnss/NOTES.md`、`gnss/docs/PROTOCOL.md`。
- 内核升级后需按 `gnss/kernel/README.md` 重编 `airoha_gps.ko`（ABI 绑定）。

## 2. 传感器核心不变量（沿用原传感器包）

- `/lib/firmware/qcom/sm8250/xiaomi/elish/slpi.mbn` 必须是**悬空符号链接** →
  `/run/sensor-fw/slpi.mbn`（`/run` 是 tmpfs，开机清空）。若变成普通文件，内核 boot+5s 会抢先
  auto_boot SLPI → 监听器来不及 → PD 看门狗崩溃 → 本次开机传感器永久失效。
- `slpi-fw-guard.service` 必须启用；`armbian-firmware-full` 升级后立即检查/恢复该链接。
- `PRE_DELAY=0`（经 8/8 验证）、`IIO_DELAY=8`；异常时可回退 10/45。
- 每 boot 出现 `iio has no SSC sensors after device appeared ... warm restart` 是**已知兜底**，正常。
- 偶发 PD 静默卡死（约 1/14 干净开机）：干净重启恢复；同 boot 内重启服务无效。
- suspend/resume 后 PD 可能崩溃；已部署休眠钩子，少数情况需重启。

## 3. 指纹核心坑（沿用原指纹包）

- **ABI 三件套**：vermagic=`6.18.44-current-sm8250 SMP mod_unload aarch64`、
  `pahole -C module vmlinux` size=1280、BTF 两项 =y；不一致会黑屏只剩背光。
- **CMA**：加载 TA 约需 9MB 连续 CMA；不要并发/反复加载；ENOMEM 时重启或重启 loader。
- **TA 拒服务（1062 / scm -22）**：`sudo systemctl restart qsee-app-loader@gfenu`；
  watchdog 自动处理；不要并发 verify。
- **libfprint 被 apt 覆盖**：升级 `libfprint-2-2` 后重编安装；否则指纹设备消失。
- **fprintd 沙箱**：保留 `/etc/systemd/system/fprintd.service.d/60-goodix-qsee.conf`。
- **PAM**：`success=done`（不是 end）；触发文件在 `/run/user/<uid>`；弹窗只能由用户会话 watcher 做；
  zenity 要 `--no-markup`；`sudo -v` 不建 session（弹窗靠 12s 超时）。
- 指纹模板在 `/var/lib/fprint/...`，用户私有，不要复制/进包。

## 4. 数据与固件（本包不附带）

| 内容 | 获取方式 |
|---|---|
| SLPI 单文件固件 + sensors registry/config/socinfo + vendor proto/hals/acdb | 见 `sensors/extract/README.md` |
| 指纹 TA `gfenu.*`（9 文件） | 见 `fingerprint/extract/README.md`（从自己设备 Android 固件提取） |
| 内核 Armbian 基线 | 不随包；获取方式见 `kernel/ARMBIAN-BASELINE.md` |
| libfprint / qsee-supplicant 源码 | 随包；libfprint 上游分支+补丁说明见 `fingerprint/src/libfprint/` |

## 5. 升级/回滚速查

| 场景 | 操作 |
|---|---|
| 内核升级 | 重做统一内核 + 联合 DTB + boot 镜像；备份 Linux 启动槽；参考 `kernel/README.md` |
| `armbian-firmware-full` 升级 | 检查/恢复 SLPI 符号链接（或让启动脚本自愈后重启一次） |
| `libfprint-2-2` 升级 | 重编安装 libfprint + `ldconfig` + 重启 fprintd |
| 指纹 PAM 出问题 | 恢复 `/root/fp-pam-backup-*/sudo`；确保有密码登录 |
| 传感器长期不稳 | `sudo touch /etc/disable-sensors` 保系统正常，再排查 |
| 完全卸载 | `sensors/install/uninstall.sh --purge-data` + `fingerprint/install/uninstall-fingerprint.sh` |

## 6. 日志/诊断

```bash
# 传感器
sensor-status
sudo journalctl -b -u slpi-sensors -u iio-sensor-proxy --no-pager
sudo tail -50 /var/log/hexrpc-boot.log
# 指纹
sudo dmesg | grep -iE 'qseecom|goodix|loaded .gfenu'
journalctl -b -u qsee-supplicant -u fprintd -u fp-ta-watchdog --no-pager
fprintd-list "$USER"
cat /sys/devices/platform/fingerprint/firmware_name
# 合并自检
./install/check-all.sh
```
