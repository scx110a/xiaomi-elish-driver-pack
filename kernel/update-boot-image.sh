#!/bin/bash
# update-boot-image.sh — 安全重建并写入 Linux 启动槽（槽位自动检测，不写死 boot_b）
#
# 槽位规则（重要）：
#   - Linux 启动槽以 /boot/armbianEnv.txt 的 abl_boot_partition_label 为准
#     （本机为 boot_b；别人的安装可能是 boot_a 或其他）。脚本自动跟随，不写死。
#   - 未设置该变量时**拒绝执行**（避免盲写）；请先确认实际 Linux 槽位并写入 armbianEnv。
#   - --slot 仅用于断言/自检：与 armbianEnv 不一致时直接报错（ABL 启动链只认 armbianEnv）。
#   - 只写 armbianEnv 指向的 Linux 槽，不碰 Android 槽（Android 具体在哪个槽取决于你的布局）。
#
# 功能：预检联合 DTB、initramfs 固件不变量、备份目标 boot 分区，调用系统 hook 重建镜像，
#       回读校验（cmp）；失败不写盘。
#
# 用法：
#   sudo ./update-boot-image.sh               # 自动检测槽位：预检+备份+重建+回读校验
#   sudo ./update-boot-image.sh --dry-run     # 只做预检/备份，不写盘
#   sudo ./update-boot-image.sh --slot boot_b # 断言槽位（与 armbianEnv 不一致则报错）
#   ./update-boot-image.sh -h                 # 本帮助（无需 root）
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)

usage() { awk 'NR>1 { if ($0 ~ /^#/) { sub(/^# ?/, ""); print } else exit }' "$0"; }

DRY=0; SLOT_ARG=""
while [ $# -gt 0 ]; do
    case "$1" in
        --dry-run) DRY=1; shift ;;
        --slot) SLOT_ARG=${2:-}; shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) echo "未知参数: $1（-h 查看用法）" >&2; exit 2 ;;
    esac
done
[ "$(id -u)" -eq 0 ] || { echo "请用 root: sudo $0"; exit 1; }

ENVFILE=/boot/armbianEnv.txt
[ -f "$ENVFILE" ] || { echo "缺少 $ENVFILE"; exit 1; }

# 用 sed 读取，避免 source 引入无关变量
DETECTED=$(sed -n 's/^[[:space:]]*abl_boot_partition_label[[:space:]]*=[[:space:]]*//p' "$ENVFILE" | tail -1 | tr -d '\r')
if [ -z "$DETECTED" ]; then
    cat >&2 <<EOF
FAIL: $ENVFILE 未设置 abl_boot_partition_label，无法确定 Linux 启动槽。
  请先确认实际 Linux boot 槽，然后在 $ENVFILE 中加入：
      abl_boot_partition_label=<boot_a|boot_b|...>
  （可用 `cat /proc/cmdline` 的 slot_suffix、或对比 /dev/disk/by-partlabel/ 判断）
  本脚本只写 armbianEnv 指向的槽，不会盲写。
EOF
    exit 1
fi
case "$DETECTED" in *[!A-Za-z0-9_.-]*) echo "FAIL: 非法槽位名: $DETECTED"; exit 1;; esac
if [ -n "$SLOT_ARG" ] && [ "$SLOT_ARG" != "$DETECTED" ]; then
    cat >&2 <<EOF
FAIL: --slot $SLOT_ARG 与 $ENVFILE 的 abl_boot_partition_label=$DETECTED 不一致。
      ABL 启动链以 armbianEnv 为准；请先正确设置该变量（或去掉 --slot 自动跟随）。
EOF
    exit 1
fi
LABEL=$DETECTED
PART=/dev/disk/by-partlabel/$LABEL
[ -e "$PART" ] || { echo "找不到启动分区 $PART"; exit 1; }
echo "启动槽: $LABEL（来自 $ENVFILE: abl_boot_partition_label）"

KVER=$(uname -r)
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
echo "== 3) 备份目标 Linux 启动槽（$LABEL）"
BAK=/root/${LABEL}-backup-$(date +%Y%m%d-%H%M%S).img
dd if="$PART" of="$BAK" bs=1M status=progress
echo "  备份: $BAK ($(stat -c%s "$BAK")B)"
if [ "$DRY" = 1 ]; then
  echo "== dry-run：预检/备份完成，不写盘（目标槽 $LABEL）"
  exit 0
fi
echo "== 4) 重建并写入 $LABEL（系统 hook: zz-update-abl-kernel）"
/etc/kernel/postinst.d/zz-update-abl-kernel
sync
echo "== 5) 回读校验（$LABEL）"
SIZE=$(stat -c%s "$IMG")
head -c "$SIZE" "$PART" | cmp - "$IMG"
echo "  OK: $LABEL 回读与 $IMG 一致 ($SIZE B)"
echo "完成。重启后运行 install/check-all.sh。"
