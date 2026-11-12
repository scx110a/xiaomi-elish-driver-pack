#!/bin/bash
# extract-gfenu-ta.sh — 从你自己的 Android 设备/固件镜像提取 Goodix gfenu TA（不随包提供）
#
# 用法：
#   ./extract-gfenu-ta.sh --adb                     # 从 adb（需 root）提取
#   ./extract-gfenu-ta.sh <目录> <输出目录>          # 目录里应含 gfenu.*（9 个文件）
#   ./extract-gfenu-ta.sh <firmware_mnt.img> <输出目录>  # 从 Android firmware 分区镜像提取
#
# 输出：<输出目录>/gfenu.mdt + gfenu.b00..b07
# 校验：脚本会核对 md5/sha256 列表（gfenu 9 个文件，共约 4.8MB）
# 注意：Android 固件里还有 fingerpr.*（FPC/旧方案，错误 TA），不要用。
set -euo pipefail
DEST=${2:-./extracted/fingerprint-ta}
EXPECT=(
"gfenu.mdt 8339a3475b3897a8fcce2ab9626a9239b52f2b234782b98b8945588db4640dab"
"gfenu.b00 d983c8496f741a798d9a9895ea5860f712199f23764dd536944cdf8539250b61"
"gfenu.b01 ed70572089e239656009828558132ad55dc30567cd7592a726a261ecec3154f4"
"gfenu.b02 9033afda9aa483aac6926f9fd6d94e9b35cc32b9ca411973f1df88d4e0c9f43f"
"gfenu.b03 09c1a80b0478d226f00d8c171a5d954429150e568eaec3ccc5d43d96e926a9ae"
"gfenu.b04 4668a9259b02f284bb010787fa6b701a2f4fd4e3bd4147395f03977830686a35"
"gfenu.b05 06babd5d76f8f678a550e493f616832ec09e1362b135b8278e4113aaae41b5f7"
"gfenu.b06 e4a21e79da3a5e3000008691c1ad77910d0f699ca08235c886833864342eed26"
"gfenu.b07 0a78d00cc7aab1b43b56f0c16c07ea8a209219ea623195d59235779ba168d8a0"
)
WORK=$(mktemp -d); trap 'rm -rf "$WORK"' EXIT
SRC=${1:-}
[ -n "$SRC" ] || { echo "用法: $0 --adb | <目录> | <firmware_mnt.img> [输出目录]"; exit 1; }

if [ "$SRC" = "--adb" ]; then
  command -v adb >/dev/null || { echo "缺少 adb"; exit 1; }
  adb devices | grep -q 'device$' || { echo "没有 adb 设备（先开启 USB 调试并授权）"; exit 1; }
  adb root >/dev/null 2>&1 || true
  for p in /vendor/firmware_mnt/image /firmware/image /vendor/etc/firmware; do
    if adb shell "ls $p/gfenu.mdt" >/dev/null 2>&1; then
      echo "从设备提取 $p/gfenu.* ..."
      adb pull "$p" "$WORK/dump" >/dev/null || true
      mkdir -p "$WORK/found"; cp -f "$WORK"/dump/gfenu.* "$WORK/found/" 2>/dev/null || true
      break
    fi
  done
elif [ -d "$SRC" ]; then
  mkdir -p "$WORK/found"
  cp -f "$SRC"/gfenu.* "$WORK/found/" 2>/dev/null || { echo "目录里没有 gfenu.*：$SRC"; exit 1; }
elif [ -b "$SRC" ]; then
  # 固件分区（modem_a，FAT）：需要 root 只读挂载
  [ "$(id -u)" -eq 0 ] || { echo "读取分区需要 root：sudo $0 $SRC ..."; exit 1; }
  M=$(mktemp -d); trap 'umount "$M" 2>/dev/null || true; rm -rf "$M"' EXIT
  mount -o ro "$SRC" "$M" || { echo "挂载失败: $SRC"; exit 1; }
  mkdir -p "$WORK/found"
  for p in "$M/image" "$M"; do cp -f "$p"/gfenu.* "$WORK/found/" 2>/dev/null && break; done
elif [ -f "$SRC" ] && file -b "$SRC" 2>/dev/null | grep -qi 'fat'; then
  # FAT 镜像：优先用 mcopy/7z，否则 root loop 挂载
  mkdir -p "$WORK/found"
  if command -v mcopy >/dev/null; then
    for p in ::/image ::/; do mcopy -n -i "$SRC" "$p/gfenu.*" "$WORK/found/" 2>/dev/null && break; done
  elif command -v 7z >/dev/null; then
    7z e -y -o"$WORK/found" "$SRC" 'image/gfenu.*' 'gfenu.*' >/dev/null 2>&1 || true
  else
    [ "$(id -u)" -eq 0 ] || { echo "需要 root 或安装 mtools/7z"; exit 1; }
    M=$(mktemp -d); trap 'umount "$M" 2>/dev/null || true; rm -rf "$M"' EXIT
    mount -o loop,ro "$SRC" "$M" || { echo "loop 挂载失败"; exit 1; }
    for p in "$M/image" "$M"; do cp -f "$p"/gfenu.* "$WORK/found/" 2>/dev/null && break; done
  fi
elif [ -f "$SRC" ]; then
  command -v debugfs >/dev/null || { echo "缺少 debugfs（apt install e2fsprogs）"; exit 1; }
  echo "从 ext4 镜像提取（尝试 /image 与根目录）..."
  mkdir -p "$WORK/root"
  for p in /image /; do
    debugfs -R "rdump $p $WORK/root" "$SRC" >/dev/null 2>&1 || true
  done
  mkdir -p "$WORK/found"
  find "$WORK/root" -name 'gfenu.*' -exec cp -f {} "$WORK/found/" \; 2>/dev/null || true
else
  echo "找不到来源: $SRC"; exit 1
fi

mkdir -p "$DEST"
cp -f "$WORK"/found/gfenu.* "$DEST"/ 2>/dev/null || { echo "没有提取到 gfenu.*；请确认 Android 固件分区路径"; exit 1; }
echo "== 校验 sha256 =="
fail=0
for row in "${EXPECT[@]}"; do
  f=${row%% *}; want=${row#* }
  if [ ! -f "$DEST/$f" ]; then echo "  MISSING $f"; fail=1; continue; fi
  got=$(sha256sum "$DEST/$f" | awk '{print $1}')
  if [ "$got" = "$want" ]; then echo "  OK   $f"; else echo "  BAD  $f ($got != $want)"; fail=1; fi
done
echo
if [ "$fail" = 0 ]; then
  echo "提取完成：$DEST（9 个文件，约 4.8MB）"
  echo "下一步：sudo cp $DEST/gfenu.* /lib/firmware/ && sudo chmod 0644 /lib/firmware/gfenu.*"
else
  echo "校验失败：TA 版本可能不同（或用了 fingerpr.*）。请确认来源是设备 Android 固件。"; exit 1
fi
