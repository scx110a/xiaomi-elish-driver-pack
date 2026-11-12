#!/bin/bash
# install-hook.sh — 安装修复版 initramfs hook 并重建 initramfs（不写 boot_b）
set -euo pipefail
[ "$(id -u)" -eq 0 ] || { echo "请用 root: sudo $0"; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
DST=/etc/initramfs-tools/hooks/xiaomi-elish-firmware
TS=$(date +%Y%m%d-%H%M%S)
# 备份必须放到 /root：initramfs-tools 会执行 hooks/ 下所有可执行文件，
# 把备份留在 hooks/ 会让旧 hook 继续生效（本项目实际踩过）。
[ -f "$DST" ] && cp -a "$DST" "/root/xiaomi-elish-firmware.hook.bak-$TS"
# 清理历史错误备份（放在 hooks/ 且可执行）
for old in "$DST".bak-*; do [ -e "$old" ] && mv -f "$old" "/root/$(basename "$old")"; done
install -m 0755 "$HERE/hooks/xiaomi-elish-firmware" "$DST"
update-initramfs -u -k "$(uname -r)"
echo
echo "== 检查 initramfs 不应含 elish/slpi.mbn =="
if lsinitramfs "/boot/initrd.img-$(uname -r)" | grep -Eq 'elish/slpi(\.mbn|\.b[0-9]+)$'; then
  echo "仍含 SLPI 固件：请检查 /lib/firmware/.../elish 下是否仍有普通 slpi 文件"
  exit 1
else
  echo "OK：initramfs 已不含 SLPI 固件。"
  echo "注意：当前 boot 镜像里的旧 ramdisk 仍含旧内容，重建 boot 镜像（zz-update-abl-kernel）后才完全生效。"
fi
