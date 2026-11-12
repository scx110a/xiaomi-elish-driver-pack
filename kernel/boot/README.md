# boot 镜像构建参考

- `zz-update-abl-kernel`：本机 Armbian BSP 提供的系统 hook 副本（1360B），用于参考/回退；
  实际统一构建请调用 `../update-boot-image.sh`（预检 + 备份 boot_b + 调用系统 hook + 回读校验）。
- 若 Armbian BSP 升级导致 hook 变化，以 `/etc/kernel/postinst.d/zz-update-abl-kernel` 为准。
- 写 boot_b 前务必 `dd` 备份；绝不写 boot_a。
