#!/bin/bash
# slpi-fw-guard.sh — 开机早期"固件路径守卫"（稳定化版 2026-11-11）
#
# 在 multi-user 之前、slpi-sensors.service 之前运行（见 slpi-fw-guard.service）：
#  1) 若内核固件路径被上一轮残留成了普通文件：立刻移出固件路径（放到 /run 留证），
#     并标记 LEAKED_FW_AT_BOOT —— 这次 boot 可能已被内核 auto_boot，但至少不会继续泄漏。
#  2) 恢复"内核固件路径 = 指向 /run/sensor-fw/slpi.mbn 的符号链接"这一不变量。
#     /run 是 tmpfs，开机为空，所以下次开机内核仍看不到固件。
#  3) 若 dmesg 表明内核在本 boot 已经加载过 SLPI 固件（auto_boot），写
#     /run/sensor-boot-bad 标记，供启动脚本快速失败并给出明确提示。
# 注意：守卫无法阻止"固件在开机时存在"导致的本次 auto_boot（内核 probe 早于用户态），
#       真正的预防手段是上面的符号链接不变量；守卫是自愈/取证/快速诊断。
set -u

FW=/lib/firmware/qcom/sm8250/xiaomi/elish/slpi.mbn
FWROOT=/run/sensor-fw
LOG=/var/log/hexrpc-boot.log
BAD=/run/sensor-boot-bad

touch "$LOG" 2>/dev/null || true
log() { echo "$(date -Is) [guard] $*" >> "$LOG" 2>/dev/null; }

mkdir -p "$FWROOT" 2>/dev/null || true
status="ok"

# 1) 普通文件 = 上一轮泄漏；移出固件路径（保留在 /run 供查看，重启自动清空）
if [ -e "$FW" ] && [ ! -L "$FW" ]; then
  leaked="$FWROOT/leaked-slpi-$(date +%s).mbn"
  log "LEAKED_FW_AT_BOOT kernel fw path was a regular file (size=$(stat -c %s "$FW" 2>/dev/null || echo '?')); moving to $leaked"
  mv -f "$FW" "$leaked" 2>/dev/null || rm -f "$FW"
  status="leaked"
fi

# 2) 恢复符号链接不变量
ln -sfn "$FWROOT/slpi.mbn" "$FW" 2>/dev/null || true

# 3) 本 boot 是否已被内核 auto_boot（只有内核会在服务启动前加载该固件）
if dmesg 2>/dev/null | grep -qE 'Booting fw image .*elish/slpi\.mbn'; then
  log "AUTO_BOOT_AT_BOOT kernel already loaded slpi.mbn before userspace; this boot is damaged (marker set)"
  echo "auto_boot $(date -Is)" > "$BAD" 2>/dev/null || true
  [ "$status" = ok ] && status="auto_boot"
fi

log "guard done status=$status kernel_path=$(readlink "$FW" 2>/dev/null || echo '?')"
exit 0
