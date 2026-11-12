#!/bin/bash
# slpi-sensors-stop.sh — 传感器栈停止（稳定化版 2026-11-11）
#
# 只删运行时 /run 里的固件副本；内核固件路径保持"悬空符号链接"，
# 这样即使强制断电/不干净关机，下一次开机内核也永远看不到固件。
set -u

FW=/lib/firmware/qcom/sm8250/xiaomi/elish/slpi.mbn
FWROOT=/run/sensor-fw
LOG=/var/log/hexrpc-boot.log
log() { echo "$(date -Is) [stop] $*" >> "$LOG" 2>/dev/null; }

# 停掉监听器/attach 进程（避免残留占着设备）
pkill -f '/usr/local/sbin/hexrpc-sdsp-service.sh' 2>/dev/null || true
pkill -x hexagonrpcd 2>/dev/null || true

# 只删 /run 副本；/run 本身开机清空，无需其它清理
rm -f "$FWROOT/slpi.mbn" 2>/dev/null || true

# 保证内核路径仍然是符号链接（绝不留下普通文件 -> 绝不泄漏到下次开机）
if [ -e "$FW" ] && [ ! -L "$FW" ]; then rm -f "$FW" 2>/dev/null; fi
mkdir -p "$(dirname "$FW")" 2>/dev/null || true
ln -sfn "$FWROOT/slpi.mbn" "$FW" 2>/dev/null || true

log "runtime fw removed from /run; kernel path kept as dangling symlink (boot-safe)"
exit 0
