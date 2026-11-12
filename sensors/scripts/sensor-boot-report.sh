#!/bin/bash
# sensor-boot-report.sh — 开机后综合验证报告（由 sensor-boot-report.service 调用）
# 只读，不改系统；结果写到 /var/log/sensor-boot-report.txt（覆盖，最近一次），
# 并在 /var/log/sensor-boot-report-history.txt 追加一行摘要。
set -u

OUT=/var/log/sensor-boot-report.txt
HIST=/var/log/sensor-boot-report-history.txt
FW=/lib/firmware/qcom/sm8250/xiaomi/elish/slpi.mbn
LOG=/var/log/hexrpc-boot.log

verdict="UNKNOWN"; detail=""

{
  echo "===== sensor boot report $(date -Is) ====="
  echo "boot=$(uptime -s 2>/dev/null) up=$(uptime -p 2>/dev/null) kernel=$(uname -r)"
  echo

  echo "--- services ---"
  echo "slpi-sensors=$(systemctl is-active slpi-sensors 2>/dev/null) iio=$(systemctl is-active iio-sensor-proxy 2>/dev/null) guard=$(systemctl is-active slpi-fw-guard 2>/dev/null)"
  systemctl show iio-sensor-proxy -p NRestarts -p MainPID -p ActiveState -p Result 2>/dev/null
  echo "iio instances=$(pgrep -c iio-sensor-prox 2>/dev/null || echo 0)"
  for p in $(pgrep -x iio-sensor-prox 2>/dev/null); do echo "  iio pid $p cgroup=$(cat /proc/$p/cgroup 2>/dev/null)"; done
  echo "hexagonrpcd instances=$(pgrep -c hexagonrpcd 2>/dev/null || echo 0)"
  echo

  echo "--- firmware invariant ---"
  if [ -L "$FW" ]; then echo "kernel-fw-path=symlink -> $(readlink "$FW") resolves=$([ -e "$FW" ] && echo yes || echo no)"; else echo "kernel-fw-path=$(stat -c '%F' "$FW" 2>/dev/null || echo absent) !!"; fi
  ls -la /run/sensor-fw/ 2>/dev/null
  echo

  echo "--- kernel boot events ---"
  dmesg 2>/dev/null | grep -E 'Direct firmware load.*slpi|Booting fw image.*elish/slpi|PDM: service|SDSP assign|USER-PD DOG' | tail -12
  echo

  echo "--- hexrpc last attach ---"
  /usr/local/sbin/sensor-status.sh --brief 2>&1 | sed -n '1,40p'
  echo

  echo "--- claim failures since boot ---"
  journalctl -b --no-pager 2>/dev/null | grep -aE 'Claiming (light|accelerometer) sensor failed|Claim.*AccessDenied' | tail -6
  echo

  echo "--- sensor values (3 samples, 3s apart) ---"
  vals_ok=0
  for n in 1 2 3; do
    o=$(timeout 5 gdbus call --system --dest net.hadess.SensorProxy --object-path /net/hadess/SensorProxy --method org.freedesktop.DBus.Properties.Get net.hadess.SensorProxy AccelerometerOrientation 2>/dev/null | head -1)
    l=$(timeout 5 gdbus call --system --dest net.hadess.SensorProxy --object-path /net/hadess/SensorProxy --method org.freedesktop.DBus.Properties.Get net.hadess.SensorProxy LightLevel 2>/dev/null | head -1)
    h=$(timeout 5 gdbus call --system --dest net.hadess.SensorProxy --object-path /net/hadess/SensorProxy --method org.freedesktop.DBus.Properties.Get net.hadess.SensorProxy HasAmbientLight 2>/dev/null | head -1)
    echo "sample$n: orientation=$o light=$l has_light=$h"
    case "$o" in *undefined*|'') ;; *) vals_ok=$((vals_ok+1));; esac
    sleep 3
  done
  echo

  # 判定
  iio_ok=$(systemctl is-active iio-sensor-proxy 2>/dev/null)
  nr=$(systemctl show -p NRestarts --value iio-sensor-proxy 2>/dev/null)
  inst=$(pgrep -c iio-sensor-prox 2>/dev/null || echo 0)
  if [ "$iio_ok" = active ] && [ "$inst" -eq 1 ] && [ "${nr:-99}" -le 1 ] && [ "$vals_ok" -ge 2 ]; then
    verdict="OK"; detail="iio active, 1 instance, NRestarts=$nr, orientation samples=$vals_ok/3"
  else
    verdict="FAILED"; detail="iio=$iio_ok instances=$inst NRestarts=$nr orientation_samples=$vals_ok/3"
  fi
  echo "--- VERDICT: $verdict ($detail) ---"
} > "$OUT" 2>&1

echo "$(date -Is) boot=$(uptime -s) $verdict $detail" >> "$HIST" 2>/dev/null || true
exit 0
