#!/bin/bash
# extract-from-armbian-firmware.sh
# 从已安装（或下载的）armbian-firmware-full 包中提取传感器配置数据：
#   - sensors/registry/ (155 个)  - sensors/config/ (40 个)  - sensors/ 根文件（sns_reg.conf 等）
#   - socinfo/ (6 个：hw_platform / platform_subtype / ... )
#
# 这些数据来自 Armbian 从 Xiaomi vendor 镜像抽取的固件包，属于设备型号级默认值（非本机唯一），
# 与 Android vendor 镜像中的 config 逐字节一致。本包不附带这些文件。
#
# 用法：
#   ./extract-from-armbian-firmware.sh [输出目录]
#   ARMBIAN_FW_ROOT=<解包后的 armbian-firmware-full 根> ./extract-from-armbian-firmware.sh [输出目录]
set -euo pipefail
DEST=${1:-./extracted/armbian}
# 候选路径：已安装系统 / 解包的包 / 下载的 deb 解包
CANDIDATES=(
  "${ARMBIAN_FW_ROOT:-}/lib/firmware/qcom/sm8250/xiaomi/elish"
  "/lib/firmware/qcom/sm8250/xiaomi/elish"
  "/usr/lib/firmware/qcom/sm8250/xiaomi/elish"
)
SRC=""
for c in "${CANDIDATES[@]}"; do
  [ -n "$c" ] && [ -d "$c/sensors" ] && { SRC="$c"; break; }
done
[ -n "$SRC" ] || { echo "找不到 armbian-firmware 的 elish 数据目录。"; echo "可先安装: sudo apt-get install armbian-firmware-full"; echo "或下载解包: apt-get download armbian-firmware-full && dpkg-deb -x *.deb ./fw"; echo "然后: ARMBIAN_FW_ROOT=./fw $0 $DEST"; exit 1; }
echo "数据源: $SRC"
mkdir -p "$DEST"
for d in sensors socinfo; do
  [ -d "$SRC/$d" ] && { echo "-- $SRC/$d"; cp -a "$SRC/$d" "$DEST/"; }
done
# 拆份固件仅作参考/备份，不用于运行时（运行时用 pMOS 单文件固件）
if [ -e "$SRC/slpi.mbn" ]; then
  echo "-- 检测到拆份 SLPI 固件（slpi.mbn + slpi.b*），仅供了解；本包运行时推荐 pMOS 单文件固件。"
  echo "   注意：armbian-firmware 默认提供拆份固件，内核开机可能 auto_boot SLPI；安装时会移到 slpi-disabled/。"
fi
echo
echo "== 统计 =="
echo "registry 文件数: $(find "$DEST/sensors/registry" -type f 2>/dev/null | wc -l) （应为 155）"
echo "config   文件数: $(find "$DEST/sensors/config" -type f 2>/dev/null | wc -l) （应为 40）"
echo "socinfo  文件数: $(find "$DEST/socinfo" -type f 2>/dev/null | wc -l) （应为 6）"
echo "完成: $DEST"
