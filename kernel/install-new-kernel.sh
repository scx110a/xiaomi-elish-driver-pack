#!/bin/bash
# ⚠️ 历史脚本/已不推荐：它从旧启动镜像提取 ramdisk（不会使用更新后的 /boot/initrd），
# 且硬编码 slot_suffix/splash。统一构建请使用 kernel/update-boot-image.sh（预检+备份+回读校验）。
# 仅保留作历史参考。
# 安装新内核（含 QSEECOM TEE 驱动）+ 重建启动镜像
# 用法: sudo ./install-new-kernel.sh
#
# 安全措施：
#   - 备份当前 /boot 镜像和 vmlinuz
#   - 只替换内核镜像，不动模块（配置只新增了内置项，模块 ABI 不变）
#   - 写入 Linux 启动槽（自动读 armbianEnv 的 abl_boot_partition_label）前后校验
set -e

KVER=$(uname -r)
SRC=/var/tmp/kbuild-src
DTB=/usr/lib/linux-image-$KVER/qcom/sm8250-xiaomi-elish-csot.dtb
STAMP=$(date +%Y%m%d-%H%M%S)

# Linux boot 槽位（不写死 boot_b）
LABEL=$(sed -n 's/^[[:space:]]*abl_boot_partition_label[[:space:]]*=[[:space:]]*//p' /boot/armbianEnv.txt 2>/dev/null | tail -1 | tr -d '\r')
[ -n "$LABEL" ] || { echo "错误：/boot/armbianEnv.txt 未设置 abl_boot_partition_label"; exit 1; }
PART=/dev/disk/by-partlabel/$LABEL
[ -e "$PART" ] || { echo "错误：找不到启动分区 $PART"; exit 1; }
echo "  Linux boot 槽: $LABEL"

echo "=== 1. 检查构建产物 ==="
[ -f "$SRC/arch/arm64/boot/Image" ] || { echo "错误：内核镜像不存在，编译未完成"; exit 1; }
ls -lh "$SRC/arch/arm64/boot/Image" | awk '{print "  新内核: "$5}'
[ -f "$DTB" ] || { echo "错误：DTB 不存在"; exit 1; }
fdtget -t s "$DTB" /fingerprint compatible >/dev/null 2>&1 || {
  echo "警告：DTB 里没有 fingerprint 节点！继续将丢失指纹支持"; read -p "继续? [y/N] " a; [ "$a" = "y" ] || exit 1
}

echo "=== 2. 备份当前状态 ==="
cp -v /boot/vmlinuz-$KVER /boot/vmlinuz-$KVER.pre-tee-$STAMP
cp -v /boot/armbian-kernel-csot.img /boot/armbian-kernel-csot.img.pre-tee-$STAMP
echo "  备份完成"

echo "=== 3. 安装新内核 ==="
cp -v "$SRC/arch/arm64/boot/Image" /boot/vmlinuz-$KVER

echo "=== 4. 重建启动镜像 ==="
# 从当前启动镜像里提取 ramdisk（与可正常启动的镜像保持一致）
python3 - <<'PYX'
import struct
img = open('/boot/armbian-kernel-csot.img','rb').read()
hdr = img[:4096]
ks, _, rs, _ = struct.unpack('<4I', hdr[8:24])
page = struct.unpack('<I', hdr[36:40])[0]
ro = page + ((ks + page - 1)//page)*page
open('/tmp/current-initrd.img','wb').write(img[ro:ro+rs])
print(f"  已提取 ramdisk: {rs} 字节")
PYX

gzip -c /boot/vmlinuz-$KVER > /tmp/Image.gz
cat /tmp/Image.gz "$DTB" > /tmp/Image.gz-dtb
UUID=$(sed -e 's/^.*root=UUID=//' -e 's/ .*$//' < /proc/cmdline)
echo "  root UUID: $UUID"
mkbootimg \
  --kernel /tmp/Image.gz-dtb \
  --ramdisk /tmp/current-initrd.img \
  --base 0x0 --second_offset 0x00f00000 \
  --cmdline "root=UUID=${UUID} slot_suffix=${LABEL#boot} splash quiet" \
  --kernel_offset 0x8000 --ramdisk_offset 0x1000000 --tags_offset 0x100 \
  --pagesize 4096 \
  -o /tmp/new-boot-tee.img
ls -lh /tmp/new-boot-tee.img | awk '{print "  新启动镜像: "$5}'

echo "=== 5. 写入 Linux 启动槽 ($LABEL) ==="
dd if=/tmp/new-boot-tee.img of="$PART" conv=fsync
SIZE=$(stat -c%s /tmp/new-boot-tee.img)
dd if="$PART" bs=1M count=$((SIZE/1048576+1)) 2>/dev/null | head -c "$SIZE" | cmp - /tmp/new-boot-tee.img
echo "  ✓ 回读校验通过"
cp /tmp/new-boot-tee.img /boot/armbian-kernel-csot.img
umount /mnt/cust 2>/dev/null || true

echo
echo "=== 安装完成 ==="
echo "  新内核: /boot/vmlinuz-$KVER"
echo "  启动镜像: $LABEL 已更新"
echo "  备份:"
echo "    /boot/vmlinuz-$KVER.pre-tee-$STAMP"
echo "    /boot/armbian-kernel-csot.img.pre-tee-$STAMP"
echo
echo "现在可以重启了。重启后验证："
echo "  dmesg | grep -i qseecom"
echo "  ls /dev/tee*"
echo "  ls /sys/class/tee/"
