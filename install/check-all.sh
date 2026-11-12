#!/bin/bash
# check-all.sh — 合并包总自检（内核联合 DTB + 传感器 + GNSS + 指纹 + 关键的 apt 状态）
# 用法： ./install/check-all.sh    （root 更完整；建议 sudo）
set -u
HERE=$(cd "$(dirname "$0")/.." && pwd)
FAIL=0
step(){ echo; echo "================ $* ================"; }

step "1. 联合 DTB（传感器 vmids + 指纹节点）"
"$HERE/kernel/dtb/verify-combined-dtb.sh" || FAIL=1

step "2. 传感器栈"
"$HERE/sensors/install/verify.sh" || FAIL=1

step "3. 指纹栈"
"$HERE/fingerprint/install/verify-fingerprint.sh" || FAIL=1

step "2b. GNSS 栈（Airoha AG3335）"
if [ -x "$HERE/gnss/install/verify.sh" ]; then
  "$HERE/gnss/install/verify.sh" || FAIL=1
else
  echo "  WARN  缺少 gnss/install/verify.sh"
fi

step "3b. initramfs 固件不变量（不应含 elish/slpi）"
if lsinitramfs "/boot/initrd.img-$(uname -r)" 2>/dev/null | grep -Eq 'elish/slpi(\.mbn|\.b[0-9]+)$'; then
  echo "  WARN  initramfs 仍含 SLPI 固件（运行 kernel/initramfs/install-hook.sh 修复）"
else
  echo "  OK    initramfs 不含 SLPI 固件"
fi

step "4. 摄像头栈（elish-camera 全分辨率管线）"
if [ -x "$HERE/camera/elish-camera/verify.sh" ]; then
  "$HERE/camera/elish-camera/verify.sh" || FAIL=1
else
  echo "  WARN  缺少 camera/elish-camera/verify.sh"
fi

step "5. apt 状态（防止被升级破坏）"
for p in linux-image-current-sm8250 linux-dtb-current-sm8250 linux-headers-current-sm8250 \
         armbian-firmware-full libfprint-2-2; do
  st=$(apt-mark showhold 2>/dev/null | grep -x "$p" >/dev/null && echo hold || echo free)
  echo "  $p: $st"
done
echo "  （建议 at least hold libfprint-2-2；见 docs/UPGRADE-IMPACT-ALL.md）"

echo
[ "$FAIL" -eq 0 ] && echo "== 合并自检结论: 全部通过 ==" || { echo "== 合并自检结论: 有失败项，见上方 =="; exit 1; }
