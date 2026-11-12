#!/bin/bash
# update-boot-image.sh — 安全重建并写入 boot_b（推荐替代裸 zz-update-abl-kernel / install-new-kernel.sh）
#
# 功能：预检联合 DTB、initramfs 固件不变量、备份 boot_b，调用系统 hook 重建镜像，
#       回读校验（cmp）；失败不写盘。
# 用法：
#   sudo ./update-boot-image.sh            # 预检+备份+重建+回读校验
#   sudo ./update-boot-image.sh --dry-run  # 只做预检/备份，不写 boot_b
set -euo pipefail
[ "$(id -u)" -eq 0 ] || { echo "请用 root: sudo $0"; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
KVER=$(uname -r)
DRY=0; [ "${1:-}" = "--dry-run" ] && DRY=1
ENVFILE=/boot/armbianEnv.txt
[ -f "$ENVFILE" ] || { echo "缺少 $ENVFILE"; exit 1; }
# shellcheck disable=SC1090
. "$ENVFILE"
LABEL=${abl_boot_partition_label:-boot_b}
PART=/dev/disk/by-partlabel/$LABEL
[ -e "$PART" ] || { echo "找不到启动分区 $PART"; exit 1; }
MODEL=$(tr -d '\0' < /sys/firmware/devicetree/base/model)
case "$MODEL" in
  *BOE*) PANEL=boe;; *CSOT*) PANEL=csot;; *) echo "未知面板型号: $MODEL"; exit 1;;
esac
IMG=/boot/armbian-kernel-${PANEL}.img
DTB=/usr/lib/linux-image-${KVER}/qcom/sm8250-xiaomi-elish-${PANEL}.dtb
INITRD=/boot/initrd.img-${KVER}

echo "== 1) 联合 DTB 预检（vmids + /fingerprint + fastrpc reserved-memory）"
"$HERE/dtb/verify-combined-dtb.sh" "$DTB"
echo "== 2) initramfs 固件不变量"
if lsinitramfs "$INITRD" 2>/dev/null | grep -Eq 'elish/slpi(\.mbn|\.b[0-9]+)$'; then
  echo "FAIL: initramfs 含 SLPI 固件；先运行 kernel/initramfs/install-hook.sh 并 update-initramfs -u"; exit 1
fi
echo "  OK: initramfs 不含 SLPI 固件"
if [ ! -L /lib/firmware/qcom/sm8250/xiaomi/elish/slpi.mbn ]; then
  echo "FAIL: /lib/firmware/.../slpi.mbn 不是符号链接（有 auto_boot 风险）"; exit 1
fi
echo "  OK: 固件路径是符号链接 -> $(readlink /lib/firmware/qcom/sm8250/xiaomi/elish/slpi.mbn)"
echo "== 3) 备份 boot_b ($LABEL)"
BAK=/root/boot_b-backup-$(date +%Y%m%d-%H%M%S).img
dd if="$PART" of="$BAK" bs=1M status=progress
echo "  备份: $BAK ($(stat -c%s "$BAK")B)"
if [ "$DRY" = 1 ]; then echo "== dry-run：预检/备份完成，不写 boot_b =="; exit 0; fi
echo "== 4) 重建并写入 boot_b（系统 hook）"
/etc/kernel/postinst.d/zz-update-abl-kernel
sync
echo "== 5) 回读校验"
SIZE=$(stat -c%s "$IMG")
head -c "$SIZE" "$PART" | cmp - "$IMG"
echo "  OK: boot_b 回读与 $IMG 一致 ($SIZE B)"
echo "完成。重启后运行 install/check-all.sh。"
