#!/bin/bash
# uninstall.sh — 卸载传感器栈（root）
# 用法： sudo ./uninstall.sh [--purge-data]
set -u
[ "$(id -u)" -eq 0 ] || { echo "请用 root 运行: sudo $0"; exit 1; }
PURGE=0; [ "${1:-}" = "--purge-data" ] && PURGE=1
echo "== 停止并禁用服务 =="
for u in slpi-sensors.service slpi-fw-guard.service sensor-boot-report.timer; do
  systemctl disable --now "$u" >/dev/null 2>&1 || true
done
echo "== 删除单元/脚本/配置 =="
rm -f /etc/systemd/system/slpi-sensors.service /etc/systemd/system/slpi-fw-guard.service \
      /etc/systemd/system/sensor-boot-report.service /etc/systemd/system/sensor-boot-report.timer
rm -f /etc/systemd/system/iio-sensor-proxy.service.d/after-ssc.conf
rmdir /etc/systemd/system/iio-sensor-proxy.service.d 2>/dev/null || true
rm -f /usr/local/sbin/slpi-sensors-start.sh /usr/local/sbin/slpi-sensors-stop.sh \
      /usr/local/sbin/hexrpc-sdsp-service.sh /usr/local/sbin/slpi-fw-guard.sh \
      /usr/local/sbin/sensor-status.sh /usr/local/sbin/sensor-boot-report.sh
rm -f /usr/local/bin/sensor-status
rm -f /etc/udev/rules.d/81-libssc-xiaomi-elish.rules /etc/udev/rules.d/99-ssc-accel.rules
rm -f /usr/lib/systemd/system-sleep/60-sensors-suspend.sh /usr/lib/systemd/system-sleep/40-cancel-scheduled-shutdown.sh
rm -f /usr/share/dbus-1/system-services/net.hadess.SensorProxy.service
E=/lib/firmware/qcom/sm8250/xiaomi/elish/slpi.mbn
[ -L "$E" ] && rm -f "$E"
if [ "$PURGE" = 1 ]; then rm -rf /usr/share/qcom-ssc-elish; fi
systemctl daemon-reload
udevadm control --reload-rules 2>/dev/null || true
echo "完成。注意：内核模块 fastrpc.ko、libssc、iio-sensor-proxy、hexrpc 0.3.2 未自动删除，"
echo "      如需彻底清理请按 INSTALL.md 反向操作；数据目录$( [ "$PURGE" = 1 ] && echo 已删除 || echo 保留)。"
