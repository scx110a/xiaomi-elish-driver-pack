# boot 镜像构建参考

- `zz-update-abl-kernel`：本机 Armbian BSP 提供的系统 hook 副本（1360B），用于参考/回退；
  实际统一构建请调用 `../update-boot-image.sh`（预检 + 备份目标 Linux 启动槽 + 调用系统 hook + 回读校验；槽位自动检测）。
- 若 Armbian BSP 升级导致 hook 变化，以 `/etc/kernel/postinst.d/zz-update-abl-kernel` 为准。
- 写目标 Linux 启动槽前务必 `dd` 备份；绝不写 Android 槽（本机 Android 在 `boot_a`，以你的实际布局为准）。
