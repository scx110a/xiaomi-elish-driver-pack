#!/bin/bash
# check-upgrade.sh — 升级后风险检查（只读）
# 用法： ./check-upgrade.sh   （root 更完整；普通用户可读大部分）
set -u
WARN=0; FAIL=0
ok(){ echo "  OK    $1"; }
warn(){ echo "  WARN  $1"; WARN=$((WARN+1)); }
bad(){ echo "  FAIL  $1"; FAIL=$((FAIL+1)); }
KVER=$(uname -r)
FW=/lib/firmware/qcom/sm8250/xiaomi/elish/slpi.mbn
DTB=/usr/lib/linux-image-$KVER/qcom/sm8250-xiaomi-elish-csot.dtb

echo "== 升级后检查 $(date -Is) =="
echo "内核: $KVER"

echo "-- 1. 内核模块（必须是我们打补丁后的 fastrpc.ko）"
KO=/lib/modules/$KVER/kernel/drivers/misc/fastrpc.ko
if [ -f "$KO" ]; then
  vermagic=$(modinfo -F vermagic "$KO" 2>/dev/null)
  [ "$vermagic" = "$KVER SMP mod_unload aarch64" ] && ok "vermagic 匹配 ($vermagic)" || bad "vermagic 不匹配: '$vermagic'（模块不会加载）"
  if strings "$KO" 2>/dev/null | grep -q 'SDSP assign try'; then ok "包含 SDSP VMID 候选补丁"; else bad "缺少 VMID 补丁（被发行版模块覆盖？）"; fi
  if strings "$KO" 2>/dev/null | grep -qi 'sensors_pd\|SENSORS_PD'; then :; else warn "未确认 SMMU sensors-PD 补丁（可看源码 diff）"; fi
else
  bad "找不到 $KO"
fi
lsmod 2>/dev/null | grep -q '^fastrpc' && ok "fastrpc 模块已加载" || warn "fastrpc 模块未加载"

echo "-- 2. DTB qcom,vmids（被内核/DTB 升级覆盖？）"
if [ -f "$DTB" ]; then
  if command -v fdtget >/dev/null; then
    v=$(fdtget -t x "$DTB" /soc@0/remoteproc@5c00000/glink-edge/fastrpc qcom,vmids 2>/dev/null || true)
    case "$v" in *"3 f 5 6"*) ok "vmids=$v";; "") warn "DTB 无 qcom,vmids（驱动候选表可兜底，建议补）";; *) warn "vmids=$v（期望 3 f 5 6）";; esac
  else warn "缺 fdtget，跳过"; fi
else
  warn "找不到 $DTB"
fi

echo "-- 3. 固件路径不变量（必须是悬空符号链接）"
if [ -L "$FW" ]; then
  ok "符号链接 -> $(readlink "$FW")"
  [ -e "$FW" ] && ok "链接目标运行时存在" || ok "链接目标悬空（开机时正常）"
else
  bad "不是符号链接（有 auto_boot 风险！）：$(stat -c '%F' "$FW" 2>/dev/null || echo 缺失)"
fi
if ls /lib/firmware/qcom/sm8250/xiaomi/elish/slpi.b* >/dev/null 2>&1; then warn "发现拆份固件 slpi.b*（建议移到 slpi-disabled/）"; else ok "无拆份段文件暴露"; fi
if command -v lsinitramfs >/dev/null && [ -f /boot/initrd.img-$KVER ]; then
  if lsinitramfs /boot/initrd.img-$KVER 2>/dev/null | grep -q 'elish/slpi.mbn'; then
    warn "initramfs 内含 slpi.mbn（内核升级时可能被用于早期加载）"
  else ok "initramfs 不含 SLPI 固件"; fi
fi

echo "-- 4. iio-sensor-proxy（必须是补丁版 + 正确配置）"
if [ -x /usr/libexec/iio-sensor-proxy ]; then
  strings /usr/libexec/iio-sensor-proxy 2>/dev/null | grep -q 'staying alive waiting for hotplug' && ok "iio 是补丁版（stayalive/hotplug）" || bad "iio 不是补丁版（被发行版包覆盖？）"
else bad "缺少 /usr/libexec/iio-sensor-proxy"; fi
if grep -q 'SystemdService=iio-sensor-proxy.service' /usr/share/dbus-1/system-services/net.hadess.SensorProxy.service 2>/dev/null; then ok "D-Bus service 含 SystemdService"; else bad "D-Bus service 缺 SystemdService（会重启风暴）"; fi
grep -q 'Restart=on-failure' /etc/systemd/system/iio-sensor-proxy.service.d/after-ssc.conf 2>/dev/null && ok "drop-in Restart=on-failure" || bad "drop-in 缺失或 Restart 不对"

echo "-- 5. 其他组件"
[ -x /opt/hexrpc-032b/bin/hexagonrpcd ] && ok "hexrpc 0.3.2 在 /opt/hexrpc-032b" || bad "缺少 /opt/hexrpc-032b/bin/hexagonrpcd"
v=$(pkg-config --modversion libssc 2>/dev/null || true)
case "$v" in 0.4.*) ok "libssc $v";; "") warn "libssc 未找到（iio 会加载失败）";; *) warn "libssc 版本 $v（验证过的为 0.4.4）";; esac
[ -d /usr/share/qcom-ssc-elish/sensors/registry ] && ok "数据母本存在" || bad "缺少 /usr/share/qcom-ssc-elish"
for u in slpi-fw-guard.service slpi-sensors.service; do
  systemctl is-enabled "$u" >/dev/null 2>&1 && ok "$u enabled" || bad "$u 未 enable"
done

echo "-- 6. 可用的内核/固件升级（apt 管理的 4 个包）"
for p in linux-image-current-sm8250 linux-dtb-current-sm8250 linux-headers-current-sm8250 armbian-firmware-full; do
  inst=$(dpkg-query -W -f='${Version}' "$p" 2>/dev/null || echo '-')
  cand=$(apt-cache policy "$p" 2>/dev/null | awk '/候选|Candidate/{print $2; exit}')
  [ -z "$cand" ] && cand=$(apt-cache madison "$p" 2>/dev/null | awk 'NR==1{print $3}')
  if [ -n "$cand" ] && [ "$cand" != "$inst" ]; then warn "$p: 已装 $inst，仓库 $cand（升级后需按 docs/UPGRADE-IMPACT.md 重做补丁）"
  else ok "$p: 已装 $inst（仓库无更新）"; fi
done

echo
echo "== 结果: FAIL=$FAIL WARN=$WARN =="
[ "$FAIL" -eq 0 ] || echo "建议按 docs/UPGRADE-IMPACT.md 修复后再重启。"
exit $([ "$FAIL" -eq 0 ] && echo 0 || echo 1)
