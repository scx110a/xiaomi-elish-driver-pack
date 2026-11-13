#!/bin/bash
# backup-boot.sh — 备份 Linux 启动槽 / boot 镜像 / 联合 DTB（可选 PAM）
#
# 用法（在包根目录）：
#   sudo ./install/backup-boot.sh [--pam] [--dry-run]
#
# 说明：
#   - 启动槽从 /boot/armbianEnv.txt 的 abl_boot_partition_label 自动读取，不写死 boot_b；
#   - --pam 额外备份 /etc/pam.d/sudo（改动指纹 PAM 前建议加）；
#   - 只读当前 Linux 启动槽，不做任何写入（备份文件写到 /root/）。
set -euo pipefail

WANT_PAM=0
DRY_RUN=0
for arg in "$@"; do
    case "$arg" in
        --pam) WANT_PAM=1 ;;
        --dry-run) DRY_RUN=1 ;;
        -h|--help)
            echo "用法: sudo $0 [--pam] [--dry-run]"
            echo "  --pam      额外备份 /etc/pam.d/sudo"
            echo "  --dry-run  只打印将执行的操作，不写 /root"
            exit 0 ;;
        *) echo "未知参数: $arg（见 --help）" >&2; exit 2 ;;
    esac
done

if [ "$(id -u)" -ne 0 ] && [ "$DRY_RUN" -ne 1 ]; then
    echo "需要 root：sudo $0 [--pam]" >&2
    exit 1
fi

ENV=/boot/armbianEnv.txt
[ -r "$ENV" ] || { echo "错误：读不到 $ENV" >&2; exit 1; }

LABEL=$(sed -n 's/^[[:space:]]*abl_boot_partition_label[[:space:]]*=[[:space:]]*//p' \
        "$ENV" 2>/dev/null | tail -1 | tr -d '\r')
[ -n "$LABEL" ] || { echo "错误：$ENV 未设置 abl_boot_partition_label" >&2; exit 1; }

PART=/dev/disk/by-partlabel/$LABEL
[ -e "$PART" ] || { echo "错误：找不到启动分区 $PART" >&2; exit 1; }

STAMP=$(date +%Y%m%d-%H%M%S)
IMG=/root/${LABEL}-backup-${STAMP}.img

echo "Linux 启动槽: $LABEL ($PART)"
if [ "$DRY_RUN" = 1 ]; then
    echo "[dry-run] dd if=$PART of=$IMG bs=1M status=progress"
    exit 0
fi
echo "备份槽镜像 -> $IMG"
dd if="$PART" of="$IMG" bs=1M status=progress

KIMG=/boot/armbian-kernel-csot.img
if [ -f "$KIMG" ]; then
    cp -a "$KIMG" "/root/$(basename "$KIMG").bak-$STAMP"
    echo "备份 boot 镜像 -> /root/$(basename "$KIMG").bak-$STAMP"
fi

DTB="/usr/lib/linux-image-$(uname -r)/qcom/sm8250-xiaomi-elish-csot.dtb"
if [ -f "$DTB" ]; then
    cp -a "$DTB" "/root/$(basename "$DTB").bak-$STAMP"
    echo "备份 DTB -> /root/$(basename "$DTB").bak-$STAMP"
fi

if [ "$WANT_PAM" = 1 ] && [ -f /etc/pam.d/sudo ]; then
    cp -a /etc/pam.d/sudo "/root/sudo.pam.bak-$STAMP"
    echo "备份 PAM -> /root/sudo.pam.bak-$STAMP"
fi

echo
echo "完成。回滚 Linux 启动槽："
echo "  sudo dd if=$IMG of=$PART bs=1M status=progress"
