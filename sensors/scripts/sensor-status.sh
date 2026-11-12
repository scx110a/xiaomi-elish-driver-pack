#!/bin/bash
# sensor-status.sh — 传感器栈快速诊断（只读）
# 用法：sensor-status.sh [--brief]
# 需要能读 dmesg / journal（本机普通用户可读）；不需要 sudo。
set -u

FW=/lib/firmware/qcom/sm8250/xiaomi/elish/slpi.mbn
LOG=/var/log/hexrpc-boot.log
BRIEF=0
[ "${1:-}" = "--brief" ] && BRIEF=1

echo "== sensor status $(date -Is) =="
echo "boot=$(uptime -s 2>/dev/null) up=$(uptime -p 2>/dev/null)"
echo "config=$(tr '\n' ' ' < /etc/sensor-tuning.conf 2>/dev/null)"
if [ -e /etc/disable-sensors ]; then echo "disable-flag=SET"; else echo "disable-flag=absent"; fi

# 内核固件路径状态（关键不变量）
if [ -L "$FW" ]; then
  tgt=$(readlink "$FW")
  if [ -e "$FW" ]; then state="symlink -> $tgt (resolves)"; else state="symlink -> $tgt (dangling: boot-safe)"; fi
elif [ -e "$FW" ]; then state="REGULAR FILE (boot leak risk!)"
else state="absent"; fi
echo "kernel-fw-path=$state"

# remoteproc
echo -n "remoteproc="
for r in /sys/class/remoteproc/*/; do
  n=$(cat "$r/name" 2>/dev/null); s=$(cat "$r/state" 2>/dev/null)
  [ -n "$n" ] && echo -n "$n=$s "
done
echo
echo "fastrpc-device=$([ -e /dev/fastrpc-sdsp ] && echo present || echo absent)"
echo "services: slpi-sensors=$(systemctl is-active slpi-sensors 2>/dev/null) iio=$(systemctl is-active iio-sensor-proxy 2>/dev/null)"
echo "listeners=$(pgrep -c hexagonrpcd 2>/dev/null || echo 0)"
echo "bad-marker=$([ -e /run/sensor-boot-bad ] && cat /run/sensor-boot-bad 2>/dev/null || echo none)"

# auto_boot 判定（2026-11-13 修正）：不再用"服务 boot+60s 才起、<30s 即内核 auto_boot"的
# 旧时间启发式（加速改造后服务 up=12s 就启动，旧判据必然误报）。现用两个权威信号：
#  1) fw-guard 在服务启动前发现内核已加载固件 -> 写 /run/sensor-boot-bad；
#  2) 否则核对"固件加载时刻 vs slpi-sensors 单调启动时刻"：加载发生在服务启动之后 = 服务触发。
bline=$(dmesg 2>/dev/null | grep -E 'Booting fw image .*elish/slpi\.mbn' | head -1)
bsec=$(printf '%s' "$bline" | sed -n 's/^\[ *\([0-9.]*\)\].*/\1/p')
svc_us=$(systemctl show -p ExecMainStartTimestampMonotonic --value slpi-sensors 2>/dev/null || true)
if [ -e /run/sensor-boot-bad ]; then
  echo "AUTO_BOOT=yes (bad marker: $(cat /run/sensor-boot-bad 2>/dev/null))"
elif [ -n "$bline" ]; then
  if [ -n "$svc_us" ] && [ "$svc_us" -gt 0 ] 2>/dev/null \
     && awk "BEGIN{exit !($bsec * 1000000 < $svc_us)}" 2>/dev/null; then
    echo "AUTO_BOOT=yes (fw loaded at ${bsec}s BEFORE slpi-sensors start; kernel auto_boot)"
  else
    echo "AUTO_BOOT=no (fw loaded by service at ${bsec:-?}s; kernel early attempt blocked by dangling symlink)"
  fi
else
  echo "AUTO_BOOT=no (no fw load line)"
fi
echo "PDM-crash-count=$(dmesg 2>/dev/null | grep -c "PDM: service 'sensor_process' crash" || true)"

# 最近一次 attach 段
[ "$BRIEF" = 1 ] || {
  echo "--- boot kernel events ---"
  dmesg 2>/dev/null | grep -E 'powering up slpi|Booting fw image .*slpi|all SDSP assign|sensor_process' | tail -6
}

line=$(grep -n '=== attach' "$LOG" 2>/dev/null | tail -1 | cut -d: -f1)
if [ -n "${line:-}" ]; then
  total=$(wc -l < "$LOG" 2>/dev/null || echo 0)
  echo "last-attach-line=$line (lines-in-segment=$((total-line+1)))"
  echo "  registry-refs=$(tail -n "+$line" "$LOG" 2>/dev/null | grep -c 'sensors/registry')"
  echo "  sns_reg_config-refs=$(tail -n "+$line" "$LOG" 2>/dev/null | grep -c 'sns_reg_config')"
  echo "  hw_platform-refs=$(tail -n "+$line" "$LOG" 2>/dev/null | grep -c 'hw_platform')"
else
  echo "last-attach=none"
fi

[ "$BRIEF" = 1 ] || {
  echo "--- iio (last 3) ---"
  journalctl -b -u iio-sensor-proxy --no-pager 2>/dev/null | tail -3
}

echo "--- sensor values ---"
for p in LightLevel AccelerometerOrientation HasAmbientLight; do
  echo -n "$p="
  timeout 4 gdbus call --system --dest net.hadess.SensorProxy --object-path /net/hadess/SensorProxy \
    --method org.freedesktop.DBus.Properties.Get net.hadess.SensorProxy "$p" 2>&1 | head -1
done
