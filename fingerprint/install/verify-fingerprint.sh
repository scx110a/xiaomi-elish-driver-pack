#!/bin/bash
# verify-fingerprint.sh — 指纹栈自检（只读）
set -u
pass=0; fail=0; warn=0
ok(){ echo "  OK    $1"; pass=$((pass+1)); }
bad(){ echo "  FAIL  $1"; fail=$((fail+1)); }
wk(){ echo "  WARN  $1"; warn=$((warn+1)); }
U=${1:-${SUDO_USER:-${USER:-}}}
echo "== 指纹栈自检 $(date -Is)  user=$U =="

echo "-- 服务"
for u in qsee-supplicant 'qsee-app-loader@gfenu' fp-ta-watchdog; do
  case "$(systemctl is-active "$u" 2>/dev/null)" in active) ok "unit $u active";; *) bad "unit $u 未运行";; esac
done
# fprintd 是 D-Bus 按需激活：稳态 inactive 属正常
case "$(systemctl is-active fprintd 2>/dev/null)" in
  active) ok "fprintd active";;
  *) if systemctl cat fprintd.service >/dev/null 2>&1 && [ -e /usr/share/dbus-1/system-services/net.reactivated.Fprint.service ]; then
       ok "fprintd 按需激活（稳态 inactive 正常）"
     else bad "fprintd 未安装/不可激活"; fi;;
esac
if grep -RIsq 'NOPASSWD' /etc/sudoers /etc/sudoers.d 2>/dev/null; then
  wk "检测到 NOPASSWD sudo 规则：sudo 指纹会被绕过（如要指纹 sudo 请停用，见 fingerprint/INSTALL.md §9）"
fi
if [ -n "$U" ] && [ "$(systemctl --user is-active fp-sudo-prompt-watch.service 2>/dev/null || true)" = active ]; then
  ok "用户 watcher active"
else
  wk "用户 watcher 未运行（sudo 弹窗不可用；锁屏/登录指纹不依赖它）"
fi

echo "-- 设备/固件"
for d in /dev/goodix_fp /dev/tee0 /dev/teepriv0; do [ -e "$d" ] && ok "$d 存在" || bad "缺少 $d"; done
sysctl_fw=/sys/devices/platform/fingerprint/firmware_name
v=$(cat "$sysctl_fw" 2>/dev/null || true)
[ "$v" = gfenu ] && ok "firmware_name=gfenu" || bad "firmware_name=${v:-缺失}（期望 gfenu）"
n=$(ls /lib/firmware/gfenu.* 2>/dev/null | wc -l)
[ "$n" -eq 9 ] && ok "TA 文件 9 个" || bad "TA 文件数=$n（期望 9）"
lsmod 2>/dev/null | grep -q '^goodix_fp_spi' && ok "goodix_fp_spi 已加载" || bad "goodix_fp_spi 未加载"
dmesg 2>/dev/null | grep -q "loaded 'gfenu'" && ok "dmesg: loaded 'gfenu'" || wk "dmesg 未见 loaded 'gfenu'（可能已加载过或需要 root）"
grep -q 'DeviceAllow=/dev/goodix_fp' /etc/systemd/system/fprintd.service.d/60-goodix-qsee.conf 2>/dev/null && ok "fprintd DeviceAllow drop-in" || bad "fprintd drop-in 缺失"
ls -l /usr/sbin/qsee-supplicant /usr/sbin/qsee-app-loader >/dev/null 2>&1 && ok "qsee-supplicant/app-loader 已安装" || bad "缺少 /usr/sbin/qsee-*"
[ -e /usr/lib/aarch64-linux-gnu/libfprint-2.so.2.0.0 ] && ok "自定义 libfprint 已安装" || wk "libfprint 未找到"
strings /usr/lib/aarch64-linux-gnu/libfprint-2.so.2.0.0 2>/dev/null | grep -q 'Goodix QSEE bring-up command' && ok "libfprint 含 goodixqsee 日志补丁（看门狗可用）" || wk "libfprint 日志补丁未确认"
grep -q 'pam_fprintd.so' /etc/pam.d/sudo 2>/dev/null && ok "sudo PAM 含 pam_fprintd" || wk "sudo PAM 未启用指纹（--pam 可选）"
grep -q CmaFree <(cat /proc/meminfo) && ok "CMA 可用: $(awk '/CmaFree/{print $2" kB"}' /proc/meminfo)" || wk "CMA 状态未知"

echo "-- fprintd 设备/模板"
if [ -n "$U" ] && command -v fprintd-list >/dev/null; then
  out=$(fprintd-list "$U" 2>&1 || true)
  echo "$out" | grep -q 'Goodix' && ok "fprintd 识别到 Goodix 设备" || wk "fprintd 未识别设备: $(echo "$out" | head -1)"
  echo "$out" | grep -q 'left\|finger' && ok "已有指纹模板" || wk "尚未录入指纹（fprintd-enroll）"
fi

echo
echo "== 结果: OK=$pass WARN=$warn FAIL=$fail =="
[ "$fail" -eq 0 ] || { echo "建议看 fingerprint/NOTES.md 的故障排查"; exit 1; }
