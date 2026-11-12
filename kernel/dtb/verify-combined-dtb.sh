#!/bin/bash
# verify-combined-dtb.sh — 检查 DTB 是否同时含传感器+指纹改动（只读）
set -u
DTB=${1:-/usr/lib/linux-image-$(uname -r)/qcom/sm8250-xiaomi-elish-csot.dtb}
fail=0
ok(){ echo "  OK    $1"; }
bad(){ echo "  FAIL  $1"; fail=$((fail+1)); }
wk(){ echo "  WARN  $1"; }
echo "== 联合 DTB 检查: $DTB =="
[ -f "$DTB" ] || { echo "找不到 DTB"; exit 1; }
sha256sum "$DTB" 2>/dev/null
echo "-- 传感器：SLPI fastrpc qcom,vmids"
v=$(fdtget -t x "$DTB" /soc@0/remoteproc@5c00000/glink-edge/fastrpc qcom,vmids 2>/dev/null || true)
case "$v" in "3 f 5 6"|"3 f 5 6 "*) ok "qcom,vmids = <3 15 5 6>";; "") bad "缺少 qcom,vmids";; *) bad "qcom,vmids=$v（期望 3 f 5 6）";; esac
echo "-- 传感器：fastrpc reserved-memory / memory-region（remote heap 必需）"
mem=$(fdtget -t x "$DTB" /soc@0/remoteproc@5c00000/glink-edge/fastrpc memory-region 2>/dev/null || true)
[ -n "$mem" ] && ok "fastrpc memory-region = <$mem>" || bad "fastrpc 缺少 memory-region（remote heap 不可用）"
rsize=$(fdtget -t x "$DTB" /reserved-memory/fastrpc size 2>/dev/null || true)
[ -n "$rsize" ] && ok "/reserved-memory/fastrpc size = $rsize" || bad "缺少 /reserved-memory/fastrpc（32MB shared-dma-pool）"
echo "-- 指纹节点 /fingerprint"
comp=$(fdtget -t s "$DTB" /fingerprint compatible 2>/dev/null || true)
fw=$(fdtget -t s "$DTB" /fingerprint firmware-name 2>/dev/null | tr -d '\0' | sed -e 's/[[:space:]]*$//' || true)
intr=$(fdtget -t x "$DTB" /fingerprint interrupts 2>/dev/null || true)
[ "$comp" = "goodix,gf3626" ] && ok "compatible=goodix,gf3626" || bad "compatible=${comp:-缺失}"
[ "$fw" = "gfenu" ] && ok "firmware-name=gfenu" || bad "firmware-name=${fw:-缺失}（期望 gfenu）"
case "$intr" in "17 1"*) ok "interrupts=<23 1>";; *) wk "interrupts=$intr（期望 17 1）";; esac
echo "-- regulators（dtc 往返破坏检查：L10A 3.008V / L10C 3.3V 必须仍是 4 字节原值）"
l10a=$(fdtget -t x "$DTB" /soc@0/rsc@18200000/regulators-0/ldo10 regulator-min-microvolt 2>/dev/null | tr -d ' ' || true)
l10c=$(fdtget -t x "$DTB" /soc@0/rsc@18200000/regulators-1/ldo10 regulator-min-microvolt 2>/dev/null | tr -d ' ' || true)
[ "$l10a" = "2de600" ] && ok "L10A min=0x$l10a（3.008V）" || bad "L10A min=${l10a:-缺失}（期望 2de600，4 字节）"
[ "$l10c" = "324b00" ] && ok "L10C min=0x$l10c（4 字节）" || bad "L10C min=${l10c:-缺失}（期望 324b00；若为 24b00 说明被 dtc 往返破坏）"
echo "-- GNSS：QUP5 UART（0x994000）+ bcm4775 电源节点"
gcomp=$(fdtget -t s "$DTB" /soc@0/geniqup@9c0000/i2c@994000 compatible 2>/dev/null || true)
gstat=$(fdtget -t s "$DTB" /soc@0/geniqup@9c0000/i2c@994000 status 2>/dev/null || true)
[ "$gcomp" = "qcom,geni-uart" ] && ok "QUP5 UART compatible=qcom,geni-uart" || bad "QUP5 UART compatible=${gcomp:-缺失}（期望 qcom,geni-uart）"
[ "$gstat" = "okay" ] && ok "QUP5 UART status=okay" || bad "QUP5 UART status=${gstat:-缺失}（期望 okay）"
[ -n "$(fdtget -p "$DTB" /bcm4775 gps-ldo 2>/dev/null)" ] && ok "/bcm4775 gps-ldo 存在" || bad "缺少 /bcm4775 gps-ldo"
[ -n "$(fdtget -p "$DTB" /bcm4775 pinctrl-0 2>/dev/null)" ] && ok "/bcm4775 pinctrl-0/1/2 存在" || bad "缺少 /bcm4775 pinctrl"
for n in qup-uart5-default-state gps-enable-active gps-enable-suspend; do
  fdtget -p "$DTB" /soc@0/pinctrl@f100000/$n >/dev/null 2>&1 && ok "pinctrl $n 存在" || bad "缺少 pinctrl $n"
done
fdtget -p "$DTB" /soc@0/spmi@c440000/pmic@0/gpio@c000/gps-clk-default >/dev/null 2>&1 && ok "PMIC gps-clk-default 存在" || bad "缺少 gps-clk-default"

echo
[ "$fail" -eq 0 ] && echo "结论: DTB 同时满足传感器、指纹与 GNSS 需求。" || { echo "结论: DTB 不完整。"; exit 1; }
