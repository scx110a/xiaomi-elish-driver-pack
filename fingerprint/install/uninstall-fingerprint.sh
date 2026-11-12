#!/bin/bash
# uninstall-fingerprint.sh — 移除指纹系统集成（保留内核模块/TA/libfprint）
set -u
[ "$(id -u)" -eq 0 ] || { echo "请用 root 运行: sudo $0"; exit 1; }
echo "== 停止并禁用服务 =="
systemctl disable --now qsee-app-loader@gfenu qsee-supplicant fp-ta-watchdog 2>/dev/null || true
echo "== 删除单元/规则/脚本 =="
rm -f /usr/lib/systemd/system/qsee-supplicant.service /usr/lib/systemd/system/qsee-app-loader@.service
rm -f /etc/systemd/system/fp-ta-watchdog.service
rm -f /etc/systemd/system/fprintd.service.d/60-goodix-qsee.conf
rmdir /etc/systemd/system/fprintd.service.d 2>/dev/null || true
rm -f /etc/udev/rules.d/60-goodix-qsee.rules
rm -f /usr/local/bin/fp-sudo-prompt /usr/local/bin/fp-sudo-prompt-done /usr/local/bin/fp-ta-watchdog
if [ -n "${SUDO_USER:-}" ]; then
  H=$(getent passwd "$SUDO_USER" | cut -d: -f6)
  systemctl --user -M "$SUDO_USER@" disable --now fp-sudo-prompt-watch.service 2>/dev/null || true
  rm -f "$H/.local/bin/fp-sudo-prompt-watch" "$H/.config/systemd/user/fp-sudo-prompt-watch.service"
fi
systemctl daemon-reload; udevadm control --reload-rules 2>/dev/null || true
echo "完成。注意：内核模块 goodix_fp_spi.ko、/lib/firmware/gfenu.*、qsee-supplicant 二进制、"
echo "      自定义 libfprint 未删除；如需彻底移除请另行处理（或重装发行版 libfprint-2-2）。"
echo "PAM：若之前用 --pam，请从 /root/fp-pam-backup-*/sudo 恢复 /etc/pam.d/sudo。"
