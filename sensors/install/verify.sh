#!/bin/bash
# verify.sh — 安装后/开机后自检（只读）
set -u
pass=0; fail=0
ck(){ if eval "$2" >/dev/null 2>&1; then echo "  OK   $1"; pass=$((pass+1)); else echo "  FAIL $1"; fail=$((fail+1)); fi; }
echo "== 传感器栈自检 $(date -Is) =="
echo "内核: $(uname -r)  boot: $(uptime -s 2>/dev/null)"
echo "-- 固件不变量"
FW=/lib/firmware/qcom/sm8250/xiaomi/elish/slpi.mbn
[ -L "$FW" ] && echo "  OK   内核固件路径是符号链接 -> $(readlink "$FW")" && pass=$((pass+1)) || { echo "  FAIL 内核固件路径不是符号链接（有 auto_boot 风险！）"; fail=$((fail+1)); }
[ -e "$FW" ] && echo "  INFO 链接目标当前存在（运行时正常）" || echo "  INFO 链接目标当前悬空（开机时正常）"
echo "-- 服务"
for u in slpi-fw-guard slpi-sensors; do ck "unit $u active" "systemctl is-active $u"; done
ck "iio-sensor-proxy active" "systemctl is-active iio-sensor-proxy"
ck "iio 单实例" "test \"\$(pgrep -c iio-sensor-prox 2>/dev/null)\" -eq 1"
ck "iio NRestarts<=1" "test \$(systemctl show -p NRestarts --value iio-sensor-proxy 2>/dev/null) -le 1"
echo "-- 设备/数据"
ck "/dev/fastrpc-sdsp 存在" "test -e /dev/fastrpc-sdsp"
ck "母本数据 registry 155" "test \"\$(find /usr/share/qcom-ssc-elish/sensors/registry -type f 2>/dev/null | wc -l)\" -eq 155"
ck "母本数据 socinfo 6" "test \"\$(find /usr/share/qcom-ssc-elish/socinfo -type f 2>/dev/null | wc -l)\" -eq 6"
ck "SLPI 运行中" "test \"\$(for r in /sys/class/remoteproc/*/; do [ \"\$(cat \$r/name 2>/dev/null)\" = slpi ] && cat \$r/state; done)\" = running"
echo "-- 内核日志"
ck "SDSP assign try[0]=0" "dmesg 2>/dev/null | grep -q 'SDSP assign try \\[0\\]: 0'"
ck "无 PDM 崩溃" "! dmesg 2>/dev/null | grep -q \"PDM: service 'sensor_process' crash\""
echo "-- 传感器数据"
o=$(timeout 5 gdbus call --system --dest net.hadess.SensorProxy --object-path /net/hadess/SensorProxy --method org.freedesktop.DBus.Properties.Get net.hadess.SensorProxy AccelerometerOrientation 2>/dev/null)
l=$(timeout 5 gdbus call --system --dest net.hadess.SensorProxy --object-path /net/hadess/SensorProxy --method org.freedesktop.DBus.Properties.Get net.hadess.SensorProxy LightLevel 2>/dev/null)
h=$(timeout 5 gdbus call --system --dest net.hadess.SensorProxy --object-path /net/hadess/SensorProxy --method org.freedesktop.DBus.Properties.Get net.hadess.SensorProxy HasAmbientLight 2>/dev/null)
echo "  orientation=$o"; echo "  light=$l"; echo "  has_light=$h"
case "$o" in *undefined*|'') echo "  FAIL orientation 未定义"; fail=$((fail+1));; *) echo "  OK   orientation 有值"; pass=$((pass+1));; esac
case "$h" in *true*) echo "  OK   HasAmbientLight=true"; pass=$((pass+1));; *) echo "  FAIL HasAmbientLight=false"; fail=$((fail+1));; esac
echo
echo "== 结果: $pass 项 OK, $fail 项 FAIL =="
[ "$fail" -eq 0 ] && echo "结论: 传感器栈看起来正常。" || { echo "结论: 有失败项，见 docs/TROUBLESHOOTING.md"; exit 1; }
