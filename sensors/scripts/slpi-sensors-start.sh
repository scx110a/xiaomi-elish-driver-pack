#!/bin/bash
# slpi-sensors-start.sh — 传感器栈启动（2026-11-12 加速版 v3）
#
# 加速改造（相对 2026-11-11 稳定版）：
#  1) 服务不再 After=multi-user.target（见 slpi-sensors.service），在 basic.target 之后
#     就开始，与桌面启动并行；本脚本因此会先等内核里的 slpi remoteproc 设备出现
#     （早期启动时 qcom_q6v5_pas 模块可能还没加载完），再走原有"监听器先行"时序。
#  2) iio 不再"启动后 8s 盲重启"：改为启动后轮询传感器数据，最长 150s。
#     （原"≥45s 无数据就 SIGKILL 硬重启一次"兜底已于 2026-11-13 删除：M2 可写 registry 后
#      PD 初始化只需 ~8s、传感器 ~21s 就绪，iio 自动热插拔已实测可靠，该兜底永不触发且曾因
#      wall-clock/NTP 步进误触发——留着只有风险没有收益。）
#  3) hexrpc-sdsp-service.sh 优先使用 M2 可写 registry 的 hexagonrpcd.writable-v1
#     （回退 regver-v1 → 原厂），让 PD 的 temp.json/registry 写回成功（PD 初始化 35s→8s）。
#
# 安全不变量（保持不变）：
#  - /lib/firmware/.../slpi.mbn 永远是 -> /run/sensor-fw/slpi.mbn 的符号链接；
#    /run 是 tmpfs，开机为空 -> 内核不可能在服务之前 auto_boot SLPI。
#  - 监听器先起，再把固件复制到 /run，最后 start SLPI（保留已验证成功的时序）。
#  - 若发现本 boot 已被内核 auto_boot（同 boot SCM assign 不可重复），快速失败并写清原因。
#  - 健康检查用真实数据（gdbus 属性 / hexrpc registry 读取），失败只记日志不自动重启。
set -u

FW=/lib/firmware/qcom/sm8250/xiaomi/elish/slpi.mbn
FWROOT=/run/sensor-fw
SRC=/usr/share/qcom-ssc-elish/slpi.mbn
ROOT=/run/sensors-rootsvc
LOG=/var/log/hexrpc-boot.log
BAD=/run/sensor-boot-bad
STATUS=/var/log/sensor-last-boot.txt

up() { cut -d' ' -f1 /proc/uptime 2>/dev/null || echo '?'; }
touch "$LOG" 2>/dev/null || true
log() { echo "$(date -Is) [start] up=$(up) $*" >> "$LOG" 2>/dev/null; }

publish_status() { # $1 = text
  {
    echo "=== sensor last boot report $(date -Is) ==="
    echo "$1"
    /usr/local/sbin/sensor-status.sh --brief 2>&1 || true
  } > "$STATUS" 2>/dev/null
}

finish() { # $1 = yes|no, $2 = reason
  local iio_restarts iio_instances
  iio_restarts=$(systemctl show -p NRestarts --value iio-sensor-proxy 2>/dev/null || echo '?')
  iio_instances=$(pgrep -c iio-sensor-prox 2>/dev/null || echo 0)
  if [ "$1" = yes ]; then
    log "sensors OK ($2); iio_restarts=$iio_restarts iio_instances=$iio_instances; fw kept in /run only (kernel path stays a symlink)"
    rm -f "$BAD"
    publish_status "OK: $2"
  else
    log "sensors FAILED ($2); iio_restarts=$iio_restarts iio_instances=$iio_instances; this boot cannot recover, reboot required"
    publish_status "FAILED: $2
同 boot 内 SCM assign 不可重复，传感器要重启才能恢复。"
  fi
  exit 0
}

# ---- 0. 开关与前置 ----
if [ -e /etc/disable-sensors ]; then log "disabled by /etc/disable-sensors"; exit 0; fi
if [ ! -e "$SRC" ]; then log "ERROR missing mother copy $SRC"; exit 1; fi
# shellcheck disable=SC1091
. /etc/sensor-tuning.conf 2>/dev/null || true
: "${PRE_DELAY:=5}"
: "${IIO_DELAY:=8}"

# ---- 1. 维护"开机看不到固件"的不变量（符号链接 -> /run，重启后悬空）----
mkdir -p "$(dirname "$FW")" "$FWROOT"
if [ -e "$FW" ] && [ ! -L "$FW" ]; then
  log "WARN kernel fw path was a regular file; moving to /run and replacing with symlink"
  mv -f "$FW" "$FWROOT/leaked-slpi-$(date +%s).mbn" 2>/dev/null || rm -f "$FW"
fi
ln -sfn "$FWROOT/slpi.mbn" "$FW"

# ---- 2. 等服务前提就绪：内核里出现 slpi remoteproc（早期启动时模块可能还没加载）----
t_wait0=$(date +%s%3N)
found=no
for _ in $(seq 1200); do              # 最多 60s
  for r in /sys/class/remoteproc/*/; do
    [ "$(cat "$r/name" 2>/dev/null)" = slpi ] && { found=yes; break; }
  done
  [ "$found" = yes ] && break
  sleep 0.05
done
t_wait1=$(date +%s%3N)
if [ "$found" != yes ]; then
  log "FATAL kernel slpi remoteproc device not found after $((t_wait1-t_wait0))ms from script start"
  publish_status "FAILED: 内核 slpi remoteproc 设备未出现（qcom_q6v5_pas 模块加载失败？）"
  exit 0
fi
log "kernel slpi remoteproc ready (waited $((t_wait1-t_wait0))ms from script start)"

# ---- 3. 本 boot 是否已经 auto_boot？（同 boot 不可恢复）----
slpi_state=""
for r in /sys/class/remoteproc/*/; do
  [ "$(cat "$r/name" 2>/dev/null)" = slpi ] && slpi_state="$(cat "$r/state" 2>/dev/null)"
done
if [ -e "$BAD" ] || [ -e /dev/fastrpc-sdsp ] || [ "$slpi_state" = running ]; then
  log "FATAL auto_boot/leftover detected (slpi=${slpi_state:-unknown}, marker=$([ -e "$BAD" ] && echo yes || echo no), device=$([ -e /dev/fastrpc-sdsp ] && echo yes || echo no))"
  log "FATAL this boot cannot recover (SCM assign is one-shot); sensor unavailable until reboot"
  echo "auto_boot $(date -Is)" > "$BAD"
  publish_status "FAILED: SLPI 在本 boot 被内核抢先 auto_boot（固件泄漏的历史后果）。
同 boot 无法恢复，请执行干净重启：sudo reboot"
  exit 0
fi

# ---- 4. 清理可能重复的旧监听器 ----
pkill -f '/usr/local/sbin/hexrpc-sdsp-service.sh' 2>/dev/null || true
pkill -x hexagonrpcd 2>/dev/null || true
sleep 0.3

# ---- 5. 等开机负载稳定（可配置 PRE_DELAY；默认沿用 0）----
sleep "$PRE_DELAY"
if [ -e /etc/disable-sensors ]; then log "disabled during pre-delay"; exit 0; fi

# ---- 6. 启动时序诊断（一次性，供改造验证；不影响功能）----
setsid /usr/local/sbin/sensor-timing-diag.sh 420 >> /var/log/sensor-timing.log 2>&1 &

# ---- 7. 监听器先行 ----
rm -rf "$ROOT"
export HEXAGONRPC_WARM_ATTACH=1
log0=$(wc -l < "$LOG" 2>/dev/null || echo 0)
pdm0=$(dmesg 2>/dev/null | grep -c "PDM: service 'sensor_process' crash" || true)
asg0=$(dmesg 2>/dev/null | grep -c "all SDSP assign attempts failed" || true)
setsid /usr/local/sbin/hexrpc-sdsp-service.sh >> "$LOG" 2>&1 &
sleep 0.2

# ---- 8. 运行时固件放 /run（符号链接目标），然后启动 SLPI ----
cp -f "$SRC" "$FWROOT/slpi.mbn"
t0=$(date +%s%3N)
for r in /sys/class/remoteproc/*/; do
  [ "$(cat "$r/name" 2>/dev/null)" = slpi ] && echo start > "$r/state" 2>/dev/null
done
dev=no
for _ in $(seq 300); do [ -e /dev/fastrpc-sdsp ] && { dev=yes; break; }; sleep 0.1; done
t1=$(date +%s%3N)
log "SLPI start issued; device=$dev after $((t1-t0))ms"

# ---- 9. iio：尽早启动，轮询传感器数据（不再盲重启）----
sleep "$IIO_DELAY"
systemctl --no-block start iio-sensor-proxy.service >/dev/null 2>&1
ok=no
why="timeout"
if [ "$dev" = no ]; then
  why="no /dev/fastrpc-sdsp"
else
  deadline=$(( $(date +%s) + 150 ))   # 最长等 150s
  while [ "$(date +%s)" -lt "$deadline" ]; do
    pdm=$(dmesg 2>/dev/null | grep -c "PDM: service 'sensor_process' crash" || true)
    asg=$(dmesg 2>/dev/null | grep -c "all SDSP assign attempts failed" || true)
    if [ "$pdm" -gt "$pdm0" ]; then why="PDM crash"; break; fi
    if [ "$asg" -gt "$asg0" ]; then why="SDSP assign failed"; break; fi
    if systemctl is-active --quiet iio-sensor-proxy; then
      o=$(timeout 4 gdbus call --system --dest net.hadess.SensorProxy --object-path /net/hadess/SensorProxy --method org.freedesktop.DBus.Properties.Get net.hadess.SensorProxy AccelerometerOrientation 2>/dev/null | head -1)
      case "$o" in
        *right-up*|*left-up*|*bottom-up*|*top-up*|*normal*|*tilted*) ok=yes; why="orientation=$o"; break ;;
      esac
      l=$(timeout 4 gdbus call --system --dest net.hadess.SensorProxy --object-path /net/hadess/SensorProxy --method org.freedesktop.DBus.Properties.Get net.hadess.SensorProxy LightLevel 2>/dev/null | head -1)
      lv=$(printf '%s' "$l" | grep -oE '<[0-9]+\.[0-9]+>' | tr -d '<>')
      if [ -n "$lv" ] && awk "BEGIN{exit !($lv > 0)}" 2>/dev/null; then ok=yes; why="light=$lv"; break; fi
    fi
    sleep 1
  done
  # 兜底：本 boot 的 attach 是否真的读到了 registry（文件服务 + PD 正常）
  if [ "$ok" = no ] && tail -n "+$((log0+1))" "$LOG" 2>/dev/null | grep -q 'sensors/registry'; then
    ok=yes
    why="hexrpc registry-read"
  fi
fi

finish "$ok" "$why"
