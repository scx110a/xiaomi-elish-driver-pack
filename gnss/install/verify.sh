#!/bin/bash
# 验证 GNSS 栈（DTB / 设备节点 / 服务 / NMEA / gpsd）
set -u
FAIL=0
ok(){ echo "  OK    $1"; }
bad(){ echo "  FAIL  $1"; FAIL=$((FAIL+1)); }
wk(){ echo "  WARN  $1"; }

echo "== GNSS 栈检查 =="

# 1. DTB（运行时）
if [ -d /proc/device-tree ]; then
    comp=$(tr -d '\0' < /proc/device-tree/soc@0/geniqup@9c0000/i2c@994000/compatible 2>/dev/null || true)
    st=$(tr -d '\0' < /proc/device-tree/soc@0/geniqup@9c0000/i2c@994000/status 2>/dev/null || true)
    [ "$comp" = "qcom,geni-uart" ] && ok "QUP5 UART 节点兼容 qcom,geni-uart" || bad "QUP5 UART compatible=${comp:-缺失}（DTB 未含 GNSS 改动）"
    [ "$st" = "okay" ] && ok "QUP5 UART 状态 okay" || bad "QUP5 UART status=${st:-缺失}"
    [ -e /proc/device-tree/bcm4775 ] && ok "存在 /bcm4775 GNSS 电源节点" || bad "缺少 /bcm4775 节点"
else
    wk "无 /proc/device-tree，跳过 DTB 检查"
fi

# 2. 内核模块与节点
lsmod | grep -q '^airoha_gps' && ok "airoha_gps 模块已加载" || wk "airoha_gps 模块未加载（modprobe airoha_gps）"
[ -c /dev/airoha_gps ] && ok "/dev/airoha_gps 存在" || bad "/dev/airoha_gps 不存在"
[ -c /dev/ttyHS1 ] && ok "/dev/ttyHS1 存在" || bad "/dev/ttyHS1 不存在（检查 DTB）"

# 3. 服务
systemctl is-active --quiet airoha-gnssd && ok "airoha-gnssd active" || bad "airoha-gnssd 未运行"
if journalctl -u airoha-gnssd -b --no-pager 2>/dev/null | grep -q 'runtime init sent'; then
    ok "运行时初始化已发送（PAIR066/062/086/004）"
else
    wk "未看到 runtime init 日志（daemon 可能是旧版；见 gnss/README.md）"
fi
[ -L /dev/ttyGNSS0 ] && ok "/dev/ttyGNSS0 -> $(readlink /dev/ttyGNSS0)" || bad "/dev/ttyGNSS0 不存在"
if [ -r /run/airoha-gnssd.state ]; then
    n=$(sed -n 's/^nmea_total=//p' /run/airoha-gnssd.state)
    [ "${n:-0}" -gt 0 ] && ok "已转发 NMEA 句子数=$n" || bad "NMEA 数为 0（看 journalctl -u airoha-gnssd -b）"
fi

# 3b. GeoClue 标准 API 桥
if systemctl cat gps-nmea-share.service >/dev/null 2>&1; then
    systemctl is-active --quiet gps-nmea-share && ok "gps-nmea-share active" || bad "gps-nmea-share 未运行"
    [ -S /run/gps-share.sock ] && ok "NMEA socket /run/gps-share.sock 存在" || bad "缺少 /run/gps-share.sock"
    grep -q '^nmea-socket=/run/gps-share.sock' /etc/geoclue/geoclue.conf 2>/dev/null && ok "geoclue 已配置 network-nmea socket" || wk "geoclue 未配置 nmea-socket（地图应用拿不到位置）"
    systemctl is-active --quiet geoclue && ok "geoclue active" || wk "geoclue 未运行（装 geoclue-2.0 后重试）"
    if [ -r /etc/polkit-1/rules.d/49-iio-sensor-proxy-geoclue.rules ]; then
        ok "polkit 规则已安装（geoclue 可 claim 指南针）"
    elif [ "$(id -u)" -ne 0 ] && [ ! -x /etc/polkit-1/rules.d ]; then
        wk "polkit 规则目录仅 root 可读，当前用户无法确认（sudo 运行本检查更完整）"
    else
        bad "缺少 polkit 规则（GeoClue 无法 claim 指南针，Location.heading 会为空）"
    fi
else
    wk "未安装 gps-nmea-share（GeoClue 标准 API 不可用；不影响 gpsd 客户端）"
fi

# 4. gpsd
if command -v gpsd >/dev/null 2>&1; then
    systemctl is-active --quiet gpsd && ok "gpsd active" || bad "gpsd 未运行"
    if command -v gpspipe >/dev/null 2>&1; then
        out=$(timeout 8 gpspipe -w -n 12 2>/dev/null || true)
        echo "$out" | grep -q '"/dev/ttyGNSS0"' && ok "gpsd 已识别 /dev/ttyGNSS0" || wk "gpsd 暂未报告 /dev/ttyGNSS0"
        echo "$out" | grep -q '"class":"TPV"' && ok "gpsd TPV 输出正常" || wk "gpsd 暂无 TPV"
    fi
else
    wk "未安装 gpsd；NMEA 仍可直接读 /dev/ttyGNSS0"
fi

echo
[ "$FAIL" -eq 0 ] && echo "结论: GNSS 栈正常（室内无星时 TPV mode=1 属正常）" || { echo "结论: 有失败项（$FAIL）"; exit 1; }
