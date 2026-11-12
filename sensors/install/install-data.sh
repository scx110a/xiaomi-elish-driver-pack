#!/bin/bash
# install-data.sh — 组装 /usr/share/qcom-ssc-elish（传感器配置数据母本）并设置内核固件路径不变量
#
# 用法（root）：
#   sudo ./install-data.sh --android <extract-from-android-vendor-img 输出目录> \
#                          --firmware <extract-pmos-slpi-mbn 输出目录> \
#                          [--armbian <extract-from-armbian-firmware 输出目录>] [--no-acdb]
#
# 组装后:
#   /usr/share/qcom-ssc-elish/{sensors,socinfo,acdb?,slpi.mbn}  （只读母本）
#   /lib/firmware/qcom/sm8250/xiaomi/elish/slpi.mbn -> /run/sensor-fw/slpi.mbn （悬空符号链接，启动脚本运行时放固件）
#   拆份固件（如果存在）移动到 slpi-disabled/ 与 slpi.mbn.disabled，防止内核开机 auto_boot。
set -euo pipefail
[ "$(id -u)" -eq 0 ] || { echo "请用 root 运行: sudo $0 ..."; exit 1; }
ROOT=/usr/share/qcom-ssc-elish
FW_DIR=/lib/firmware/qcom/sm8250/xiaomi/elish
ANDROID=""; FIRMWARE=""; ARMB=""; NO_ACDB=0
while [ $# -gt 0 ]; do
  case "$1" in
    --android) ANDROID="$2"; shift 2;;
    --firmware) FIRMWARE="$2"; shift 2;;
    --armbian) ARMB="$2"; shift 2;;
    --no-acdb) NO_ACDB=1; shift;;
    *) echo "未知参数: $1"; exit 1;;
  esac
done
[ -n "$ANDROID" ] || { echo "缺少 --android（Android vendor 提取目录）"; exit 1; }
[ -n "$FIRMWARE" ] || { echo "缺少 --firmware（pMOS 单文件固件目录）"; exit 1; }
[ -d "$ANDROID/sensors" ] || { echo "找不到 $ANDROID/sensors"; exit 1; }
[ -f "$FIRMWARE/slpi.mbn" ] || [ -f "$FIRMWARE/firmware/slpi.mbn" ] || { echo "找不到 $FIRMWARE/slpi.mbn（或 $FIRMWARE/firmware/slpi.mbn）"; exit 1; }

WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/data"
if [ -n "$ARMB" ]; then
  [ -d "$ARMB/sensors" ] && cp -a "$ARMB/sensors/." "$WORK/data/sensors/" 2>/dev/null || true
  [ -d "$ARMB/socinfo" ] && cp -a "$ARMB/socinfo" "$WORK/data/" || true
else
  echo "WARN: 未提供 --armbian，将缺少 registry/config（必需！）。"
  echo "      请先运行 extract/extract-from-armbian-firmware.sh 并把结果用 --armbian 传入。"
  mkdir -p "$WORK/data/sensors"
fi
mkdir -p "$WORK/data/sensors"
cp -a "$ANDROID/sensors/." "$WORK/data/sensors/" 2>/dev/null || true
if [ "$NO_ACDB" = 0 ] && [ -d "$ANDROID/acdb" ]; then cp -a "$ANDROID/acdb" "$WORK/data/" || true; fi
if [ -f "$FIRMWARE/slpi.mbn" ]; then cp -a "$FIRMWARE/slpi.mbn" "$WORK/data/slpi.mbn"
elif [ -f "$FIRMWARE/firmware/slpi.mbn" ]; then cp -a "$FIRMWARE/firmware/slpi.mbn" "$WORK/data/slpi.mbn"; fi

# 备份旧母本
if [ -e "$ROOT" ]; then
  BAK="$ROOT.bak-$(date +%Y%m%d-%H%M%S)"
  echo "备份旧数据: $ROOT -> $BAK"
  mv "$ROOT" "$BAK"
fi
mv "$WORK/data" "$ROOT"
chown -R root:root "$ROOT"
find "$ROOT" -type d -exec chmod 0555 {} +
find "$ROOT" -type f -exec chmod 0444 {} +

# 内核固件路径不变量
mkdir -p "$FW_DIR"
shopt -s nullglob
mkdir -p "$FW_DIR/slpi-disabled"
for f in "$FW_DIR"/slpi.b*; do mv -f "$f" "$FW_DIR/slpi-disabled/" 2>/dev/null || true; done
if [ -e "$FW_DIR/slpi.mbn" ] && [ ! -L "$FW_DIR/slpi.mbn" ]; then
  mv -f "$FW_DIR/slpi.mbn" "$FW_DIR/slpi.mbn.disabled" 2>/dev/null || rm -f "$FW_DIR/slpi.mbn"
fi
mkdir -p /run/sensor-fw
ln -sfn /run/sensor-fw/slpi.mbn "$FW_DIR/slpi.mbn"

echo
echo "== 检查 =="
echo "registry: $(find "$ROOT/sensors/registry" -type f 2>/dev/null | wc -l)/155"
echo "config  : $(find "$ROOT/sensors/config" -type f 2>/dev/null | wc -l)/40"
echo "proto   : $(find "$ROOT/sensors/proto" -type f 2>/dev/null | wc -l)/79"
echo "socinfo : $(find "$ROOT/socinfo" -type f 2>/dev/null | wc -l)/6"
[ -d "$ROOT/acdb" ] && echo "acdb    : 已包含（可选，仅 hexagonfs 根会用）" || echo "acdb    : 未包含（可选）"
echo "slpi.mbn: $(ls -l "$ROOT/slpi.mbn" 2>/dev/null | awk '{print $5}') 字节（期望 7633828）"
md5sum "$ROOT/slpi.mbn" 2>/dev/null || true
echo "固件路径: $(ls -l "$FW_DIR/slpi.mbn" | sed 's/.*-> / -> /')"
echo
echo "完成。注意：升级 armbian-firmware-full 后可能重新出现拆份固件/普通文件，"
echo "      slpi-fw-guard 会在下次开机修正并告警；升级后建议立即检查该路径并重启。"
