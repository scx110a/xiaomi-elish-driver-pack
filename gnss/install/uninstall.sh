#!/bin/bash
# 卸载 airoha-gnssd 与 gpsd 集成（保留内核模块与 GNSS flash）
set -u
[ "$(id -u)" -eq 0 ] || { echo "需要 root"; exit 1; }
GPSD_DEFAULT=/etc/default/gpsd

echo "== 停止并禁用服务"
systemctl disable --now airoha-gnssd.service 2>/dev/null || true
systemctl daemon-reload

echo "== 停止并禁用 GeoClue 桥"
systemctl disable --now gps-nmea-share.service 2>/dev/null || true

echo "== 删除文件"
rm -f /usr/local/sbin/airoha-gnssd
rm -f /usr/local/sbin/gps-nmea-share
rm -f /etc/systemd/system/airoha-gnssd.service
rm -f /etc/systemd/system/gps-nmea-share.service
rm -f /etc/systemd/system/gpsd.service.d/airoha-gnssd.conf
rm -f /etc/systemd/system/geoclue.service.d/airoha-gnssd.conf
rm -f /etc/polkit-1/rules.d/49-iio-sensor-proxy-geoclue.rules
rm -f /run/gps-share.sock
rm -rf /usr/local/share/doc/airoha-gnssd
if [ -f /etc/geoclue/geoclue.conf.bak-airoha ]; then
    cp -a /etc/geoclue/geoclue.conf.bak-airoha /etc/geoclue/geoclue.conf
    echo "== 已恢复原 geoclue.conf（备份保留）"
fi

if [ -f "$GPSD_DEFAULT.bak-airoha" ]; then
    cp -a "$GPSD_DEFAULT.bak-airoha" "$GPSD_DEFAULT"
    echo "== 已恢复原 gpsd 配置（备份保留：$GPSD_DEFAULT.bak-airoha）"
fi
systemctl restart polkit 2>/dev/null || true
systemctl restart gpsd.service 2>/dev/null || true
echo "== 完成。GNSS LDO 已由服务停止流程关闭；内核模块与 flash 未改动。"
