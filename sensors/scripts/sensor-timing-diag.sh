#!/bin/bash
# sensor-timing-diag.sh — 一次性传感器启动时序诊断（2026-11-12 加速改造验证）
#
# 由 slpi-sensors-start.sh 在启动 SLPI 前以 setsid 拉起，运行 ${1:-420}s 后自动退出。
# 只读观测（不启动/停止任何服务；只在 iio 已 active 时查询其 D-Bus 属性）：
#   - /dev/fastrpc-sdsp 出现
#   - slpi remoteproc 状态
#   - iio-sensor-proxy active 状态
#   - /var/log/hexrpc-boot.log 行数增长（PD 文件读取进度）
#   - HasAccelerometer / HasAmbientLight（传感器真正可用时刻）
# 输出 /var/log/sensor-timing.log，便于重启后精确分析各阶段耗时。
set -u
LOG=/var/log/sensor-timing.log
DUR=${1:-420}

up() { cut -d' ' -f1 /proc/uptime 2>/dev/null || echo '?'; }
log() { echo "$(date -Is) up=$(up) $*" >> "$LOG" 2>/dev/null; }
lines_of() { wc -l < /var/log/hexrpc-boot.log 2>/dev/null | tr -d ' ' || echo 0; }

: > "$LOG" 2>/dev/null || true
log "diag start (duration=${DUR}s)"
last_dev=init; last_slpi=init; last_iio=init; last_lines=-1; last_acc=init; last_light=init
end=$(( $(date +%s) + DUR ))
while [ "$(date +%s)" -lt "$end" ]; do
  dev=no; [ -e /dev/fastrpc-sdsp ] && dev=yes
  slpi=""
  for r in /sys/class/remoteproc/*/; do
    [ "$(cat "$r/name" 2>/dev/null)" = slpi ] && slpi=$(cat "$r/state" 2>/dev/null)
  done
  iio=$(systemctl is-active iio-sensor-proxy 2>/dev/null || true)
  [ "$dev" != "$last_dev" ] && { log "fastrpc-sdsp=$dev"; last_dev=$dev; }
  [ "$slpi" != "$last_slpi" ] && { log "slpi-state=$slpi"; last_slpi=$slpi; }
  [ "$iio" != "$last_iio" ] && { log "iio=$iio"; last_iio=$iio; }
  lines=$(lines_of)
  if [ "$lines" != "$last_lines" ]; then
    log "hexrpc-log-lines=$lines"
    last_lines=$lines
  fi
  if [ "$iio" = active ]; then
    acc=$(timeout 2 gdbus call --system --dest net.hadess.SensorProxy --object-path /net/hadess/SensorProxy --method org.freedesktop.DBus.Properties.Get net.hadess.SensorProxy HasAccelerometer 2>/dev/null | head -1)
    case "$acc" in
      *true*) [ "$last_acc" != yes ] && { log "HasAccelerometer=yes"; last_acc=yes; } ;;
      *) [ "$last_acc" != no ] && { log "HasAccelerometer=no"; last_acc=no; } ;;
    esac
    light=$(timeout 2 gdbus call --system --dest net.hadess.SensorProxy --object-path /net/hadess/SensorProxy --method org.freedesktop.DBus.Properties.Get net.hadess.SensorProxy HasAmbientLight 2>/dev/null | head -1)
    case "$light" in
      *true*) [ "$last_light" != yes ] && { log "HasAmbientLight=yes"; last_light=yes; } ;;
      *) [ "$last_light" != no ] && { log "HasAmbientLight=no"; last_light=no; } ;;
    esac
  fi
  sleep 0.5
done
log "diag end"
