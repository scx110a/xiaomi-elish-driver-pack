#!/bin/bash
# extract-pmos-slpi-mbn.sh
# 从 postmarketOS 的 firmware-xiaomi-elish-slpi-*.apk 中提取单文件 SLPI 固件 slpi.mbn。
#
# 这是本项目验证过的固件形式：单文件 7,633,828 字节，md5
#   51c3b3bef20a76d386a516dfbcc72b68
# 对应 pMOS 包：firmware-xiaomi-elish-slpi-20240917-r3.apk
# （获取方式：pMOS 的 device-xiaomi-elish 固件子包；.apk 是 gzip tar，可直接解包）
#
# 备选：如果拿不到 pMOS 包，可以用 armbian-firmware-full 自带的拆份固件（slpi.mbn + slpi.b00..b20）。
#       内容同源（12 段 md5 相同），但本项目**只验证过**单文件形式，拆份形式未验证。
#
# 用法： ./extract-pmos-slpi-mbn.sh <firmware-xiaomi-elish-slpi-*.apk> [输出目录]
set -euo pipefail
APK=${1:-}; DEST=${2:-./extracted/pmos}
[ -n "$APK" ] && [ -f "$APK" ] || { echo "用法: $0 <slpi.apk> [输出目录]"; exit 1; }
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/x" "$DEST/firmware"
if ! tar -xzf "$APK" -C "$WORK/x" 2>/dev/null; then
  gzip -dc "$APK" 2>/dev/null | tar -x -C "$WORK/x" || { echo "无法解包 $APK"; exit 1; }
fi
F=$(find "$WORK" -type f -name 'slpi.mbn' | head -1 || true)
[ -n "$F" ] || { echo "包内没找到 slpi.mbn；内容示例："; tar -tzf "$APK" 2>/dev/null | grep -i slpi | head -20; exit 1; }
cp -a "$F" "$DEST/firmware/slpi.mbn"
echo "已提取: $DEST/firmware/slpi.mbn"
ls -l "$DEST/firmware/slpi.mbn"
md5sum "$DEST/firmware/slpi.mbn"
echo "期望 md5: 51c3b3bef20a76d386a516dfbcc72b68（若不同，请确认 pMOS 包版本）"
