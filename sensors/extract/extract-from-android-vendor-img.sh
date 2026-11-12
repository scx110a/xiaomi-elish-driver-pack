#!/bin/bash
# extract-from-android-vendor-img.sh
# 从你自己设备的 Android vendor 镜像中提取传感器所需数据（只读，不需要 root）。
#
# 用法：
#   ./extract-from-android-vendor-img.sh <android-vendor.img> [输出目录]
#
# 提取内容：
#   <输出目录>/sensors/    ← /etc/sensors（sns_reg_config、config/、proto/、scripts/、
#                              hals.conf、calibratedCheck.json、judge_calibrated.json、sensor_diag.cfg）
#   <输出目录>/acdb/       ← /etc/acdbdata（Forte/、adsp_avs_config.acdb）
#
# 说明：
# - 该镜像可以是你从设备 dump 出来的 vendor 分区镜像（ext4 或稀疏镜像）。
# - 本脚本不会修改镜像；debugfs 只读挂载。
# - registry（155 个文件）不在 vendor 镜像里，需从 pMOS hexagonfs 包提取（见 extract-from-armbian-firmware.sh）。
set -euo pipefail

IMG=${1:-}
DEST=${2:-./extracted/android}
[ -n "$IMG" ] || { echo "用法: $0 <android-vendor.img> [输出目录]"; exit 1; }
[ -f "$IMG" ] || { echo "找不到镜像: $IMG"; exit 1; }
command -v debugfs >/dev/null || { echo "缺少 debugfs（apt install e2fsprogs）"; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
REAL="$IMG"

# 稀疏镜像处理
if command -v file >/dev/null && file -b "$IMG" | grep -qi 'sparse'; then
  if command -v simg2img >/dev/null; then
    echo "检测到 sparse 镜像，转换为 raw..."
    simg2img "$IMG" "$WORK/vendor.raw"
    REAL="$WORK/vendor.raw"
  else
    echo "镜像为 sparse 格式，但缺少 simg2img（apt install android-sdk-libsparse-utils）"; exit 1
  fi
fi

mkdir -p "$DEST"
echo "== 从 $REAL 提取 =="

extract_dir() { # <img 内路径> <目标目录（直接放内容，不再套一层）>
  local src="$1" out="$2" base tmpd
  echo "-- $src -> $out"
  tmpd=$(mktemp -d)
  # debugfs rdump 要求目标目录存在，且会在目标下创建源目录的同名子目录；先导到临时目录再摊平。
  debugfs -R "rdump $src $tmpd" "$REAL" >/dev/null 2>&1 || true
  base=$(basename "$src")
  mkdir -p "$out"
  if [ -d "$tmpd/$base" ]; then
    cp -a "$tmpd/$base/." "$out"/ 2>/dev/null || true
  elif [ -n "$(ls -A "$tmpd" 2>/dev/null)" ]; then
    cp -a "$tmpd/." "$out"/ 2>/dev/null || true
  fi
  rm -rf "$tmpd"
}

extract_dir /etc/sensors  "$DEST/sensors"
extract_dir /etc/acdbdata "$DEST/acdb"

echo
echo "== 检查结果 =="
n_sensors=$(find "$DEST/sensors" -type f 2>/dev/null | wc -l)
n_acdb=$(find "$DEST/acdb" -type f 2>/dev/null | wc -l)
echo "sensors 文件数: $n_sensors （Android /etc/sensors 约 124 个）"
echo "acdb 文件数:    $n_acdb （应含 Forte/Forte_*.acdb 与 adsp_avs_config.acdb）"

if [ "$n_sensors" -gt 0 ]; then
  for f in sns_reg_config hals.conf calibratedCheck.json; do
    [ -e "$DEST/sensors/$f" ] && echo "  OK: sensors/$f" || echo "  !! 缺少 sensors/$f"
  done
  [ -d "$DEST/sensors/config" ]  && echo "  OK: sensors/config/"  || echo "  !! 缺少 sensors/config/"
  [ -d "$DEST/sensors/proto" ]   && echo "  OK: sensors/proto/"   || echo "  !! 缺少 sensors/proto/"
  [ -d "$DEST/sensors/scripts" ] && echo "  OK: sensors/scripts/" || echo "  !! 缺少 sensors/scripts/"
fi
if [ "$n_acdb" -eq 0 ]; then
  echo "!! 没有提取到 acdb（如果镜像里路径是 /vendor/etc/acdbdata，请手动尝试）"
fi

cat <<EOF

完成：$DEST
下一步：
  1) registry/config/socinfo 用 extract-from-armbian-firmware.sh 提取；
  2) SLPI 单文件固件用 extract-pmos-slpi-mbn.sh 提取；
  3) 用 install/install-data.sh 组装到 /usr/share/qcom-ssc-elish。
注意：registry 目录（155 个文件）不在此镜像中。
EOF
