#!/bin/bash
# install-system-files.sh — 安装脚本/服务/udev/休眠钩子/D-Bus 配置（root）
# 用法： sudo ./install-system-files.sh
set -euo pipefail
[ "$(id -u)" -eq 0 ] || { echo "请用 root 运行: sudo $0"; exit 1; }
HERE=$(cd "$(dirname "$0")/.." && pwd)
P="$HERE"

echo "== 1) 运行时脚本 -> /usr/local/sbin"
install -d /usr/local/sbin
for f in slpi-sensors-start.sh slpi-sensors-stop.sh hexrpc-sdsp-service.sh \
         slpi-fw-guard.sh sensor-status.sh sensor-boot-report.sh sensor-timing-diag.sh; do
  install -m 0755 "$P/scripts/$f" "/usr/local/sbin/$f"
done
ln -sfn /usr/local/sbin/sensor-status.sh /usr/local/bin/sensor-status

# 1b) 本包不附带 hexrpc 预编译二进制（只含补丁/源码/文档）；
#     按 sensors/userspace/hexrpc/README.md 自行编译并安装到 /opt/hexrpc-032b/bin/。

echo "== 2) systemd 单元"
install -m 0644 "$P/systemd/slpi-sensors.service" /etc/systemd/system/
install -m 0644 "$P/systemd/slpi-fw-guard.service" /etc/systemd/system/
install -m 0644 "$P/systemd/sensor-boot-report.service" /etc/systemd/system/
install -m 0644 "$P/systemd/sensor-boot-report.timer" /etc/systemd/system/
install -d /etc/systemd/system/iio-sensor-proxy.service.d
install -m 0644 "$P/systemd/iio-dropin/after-ssc.conf" /etc/systemd/system/iio-sensor-proxy.service.d/after-ssc.conf

echo "== 3) udev 规则"
install -m 0644 "$P/systemd/udev/80-iio-sensor-proxy.rules" /etc/udev/rules.d/ 2>/dev/null || true
install -m 0644 "$P/systemd/udev/81-libssc-xiaomi-elish.rules" /etc/udev/rules.d/
install -m 0644 "$P/systemd/udev/99-ssc-accel.rules" /etc/udev/rules.d/

echo "== 4) 休眠钩子"
install -d /usr/lib/systemd/system-sleep
install -m 0755 "$P/systemd/system-sleep/60-sensors-suspend.sh" /usr/lib/systemd/system-sleep/
install -m 0755 "$P/systemd/system-sleep/40-cancel-scheduled-shutdown.sh" /usr/lib/systemd/system-sleep/

echo "== 5) 调参配置（会备份已有文件）"
if [ -e /etc/sensor-tuning.conf ]; then cp -a /etc/sensor-tuning.conf "/etc/sensor-tuning.conf.bak-$(date +%s)"; fi
install -m 0644 "$P/systemd/sensor-tuning.conf" /etc/sensor-tuning.conf

echo "== 6) D-Bus / polkit"
install -d /usr/share/dbus-1/system-services /usr/share/dbus-1/system.d /usr/share/polkit-1/actions
install -m 0644 "$P/systemd/dbus/net.hadess.SensorProxy.service" /usr/share/dbus-1/system-services/
install -m 0644 "$P/systemd/dbus/net.hadess.SensorProxy.conf" /usr/share/dbus-1/system.d/ 2>/dev/null || true
install -m 0644 "$P/systemd/dbus/net.hadess.SensorProxy.policy" /usr/share/polkit-1/actions/ 2>/dev/null || true

echo "== 7) 停用历史/实验单元（如果存在）"
for u in hexagonrpcd-sdsp.service slpi-start.service sensors-matrix.service \
         sensor-validate.service sensor-orch.service sensor-delay-exp.timer; do
  systemctl disable --now "$u" >/dev/null 2>&1 || true
done

echo "== 8) 重新加载并启用"
systemctl daemon-reload
systemctl enable slpi-fw-guard.service
systemctl enable slpi-sensors.service
systemctl enable sensor-boot-report.timer
udevadm control --reload-rules || true
udevadm trigger --subsystem-match=misc || true
systemctl reload dbus >/dev/null 2>&1 || systemctl restart dbus >/dev/null 2>&1 || true
ldconfig 2>/dev/null || true

echo
echo "完成。下一步："
echo "  1) 按 INSTALL.md 完成内核 fastrpc 模块 + hexagonrpcd + libssc + iio-sensor-proxy 的编译安装；"
echo "  2) 用 install/install-data.sh 组装数据；"
echo "  3) sudo reboot，然后运行 verify.sh 或 sensor-status 验证。"
