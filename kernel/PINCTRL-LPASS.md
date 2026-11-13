# LPASS LPI pinctrl：时钟超时改为 -EPROBE_DEFER（pogo 键盘/USB 随机失效）

此补丁**与传感器/指纹无关**，修的是 elish 的另一个随机问题；为保证统一内核重建后行为一致，一并在
`kernel/README.md` 的构建流程中应用（已提供 `patches/0002-pinctrl-lpass-lpi-defer-on-clock-timeout.patch`）。

## 症状

开机偶发 pogo 键盘消失（与 ADSP 启动速度赛跑）：

```
qcom-sm8250-lpass-lpi-pinctrl 33c0000.pinctrl: error -ETIMEDOUT: Can't enable clocks
probe with driver qcom-sm8250-lpass-lpi-pinctrl failed
platform 88e4000.phy: deferred probe pending (supplier 33c0000.pinctrl)
platform a800000.usb: deferred probe pending (supplier 88e4000.phy)
```

链路：initramfs 早期加载 pinctrl 模块 → LPASS 时钟由 ADSP(q6prm) 提供，固件未就绪时
`clk_bulk_prepare_enable` 超时 → 驱动返回 `-ETIMEDOUT` 导致 probe 永久失败 → USB PHY 永远 defer →
键盘消失。

## 补丁

`drivers/pinctrl/qcom/pinctrl-lpass-lpi.c` 的 `lpi_pinctrl_probe()`：对
`-ETIMEDOUT/-EAGAIN/-EBUSY` 返回 `-EPROBE_DEFER`（而非硬失败），ADSP 就绪后内核自动重试。
无需用户态服务。

## 构建/部署（统一内核流程的一部分）

1. 按 `kernel/README.md` 应用本补丁并编译内核/模块；
2. 安装模块后**必须更新 initramfs**（此模块在 initramfs 里）：
   ```bash
   sudo update-initramfs -u -k $(uname -r)
   ```
3. 重建 boot 镜像并写 Linux 启动槽（本机 `boot_b`，按 armbianEnv 自动检测；推荐系统 hook）：
   ```bash
   sudo /etc/kernel/postinst.d/zz-update-abl-kernel
   ```
   （`kernel/install-new-kernel.sh` 会复用旧启动镜像里的 ramdisk，**不适用于** initramfs 已更新的场景，
   除非你手动用新 `/boot/initrd.img-*` 重建镜像。）
4. 重启验证：
   ```bash
   strings /lib/modules/$(uname -r)/kernel/drivers/pinctrl/qcom/pinctrl-lpass-lpi.ko | grep 'LPASS clocks not ready'
   sudo dmesg | grep -i 'lpass-lpi'        # 正常应看到 deferring 后成功，或不再报 ETIMEDOUT
   lsusb | grep -i xiaomi                   # 键盘/USB 正常
   ```

## 注意

- 本机 2026-11-12 已部署并重启验证（内核/initramfs/boot 镜像一致性与 `install/check-all.sh` 通过）；部署/回滚细节见工作区历史归档。
- **内核/initramfs 更新会覆盖**：升级内核后需重新应用本补丁并重建 initramfs + boot 镜像。
- 若不想每次重做，也可用用户态自愈（检测 ADSP 就绪后重新 bind/rebind pinctrl）；本包未包含该服务。
- 不要用 `echo 33c0000.pinctrl > .../bind` 手动救急（时钟未就绪时会卡死内核）。
