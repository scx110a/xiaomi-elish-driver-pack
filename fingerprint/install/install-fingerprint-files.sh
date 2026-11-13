#!/bin/bash
# install-fingerprint-files.sh — 安装指纹项目的系统集成文件（root）
# 用法：
#   sudo ./install-fingerprint-files.sh            # udev/systemd/脚本（推荐先装这些）
#   sudo ./install-fingerprint-files.sh --pam      # 同时把 /etc/pam.d/sudo 换成指纹优先栈（会备份）
#   sudo ./install-fingerprint-files.sh --user     # 同时给 $SUDO_USER 安装用户会话弹窗 watcher
set -euo pipefail
[ "$(id -u)" -eq 0 ] || { echo "请用 root 运行: sudo $0"; exit 1; }
HERE=$(cd "$(dirname "$0")/.." && pwd)
P="$HERE"
PAM=0; USER_INSTALL=0
for a in "$@"; do case "$a" in --pam) PAM=1;; --user) USER_INSTALL=1;; *) echo "未知参数 $a"; exit 1;; esac; done

echo "== 1) udev 规则（goodix_fp 自动拉起 TA loader）"
install -m 0644 "$P/system/udev/60-goodix-qsee.rules" /etc/udev/rules.d/

echo "== 2) systemd 单元"
install -m 0644 "$P/system/systemd/qsee-supplicant.service" "$P/system/systemd/qsee-app-loader@.service" /usr/lib/systemd/system/
install -m 0644 "$P/system/systemd/fp-ta-watchdog.service" /etc/systemd/system/
# Limit loader restart rate on TA/CMA hard failures (see fingerprint/NOTES.md §2/§3)
install -d /etc/systemd/system/qsee-app-loader@.service.d
install -m 0644 "$P/system/systemd/qsee-app-loader@.service.d/50-restart-limit.conf" /etc/systemd/system/qsee-app-loader@.service.d/
install -d /etc/systemd/system/fprintd.service.d
install -m 0644 "$P/system/systemd/fprintd-60-goodix-qsee.conf" /etc/systemd/system/fprintd.service.d/60-goodix-qsee.conf

echo "== 2b) 模块自动加载"
install -d /etc/modules-load.d
printf 'goodix_fp_spi\n' > /etc/modules-load.d/goodix-fp.conf

echo "== 3) 脚本"
install -d /usr/local/bin
install -m 0755 "$P/system/bin/fp-sudo-prompt" "$P/system/bin/fp-sudo-prompt-done" "$P/system/bin/fp-ta-watchdog" /usr/local/bin/

echo "== 4) 可选：PAM（sudo 指纹优先、8s 超时回退密码）"
if [ "$PAM" = 1 ]; then
  [ -f /etc/pam.d/sudo ] || { echo "找不到 /etc/pam.d/sudo"; exit 1; }
  BAK=/root/fp-pam-backup-$(date +%Y%m%d-%H%M%S)
  mkdir -p "$BAK"; cp -a /etc/pam.d/sudo "$BAK/sudo"
  install -m 0644 "$P/system/pam.d/sudo" /etc/pam.d/sudo
  echo "  已备份原 sudo PAM 到 $BAK/sudo"
else
  echo "  跳过（如需 sudo 指纹：sudo $0 --pam；务必先确认能回退密码登录）"
fi

echo "== 5) 可选：给登录用户安装弹窗 watcher"
if [ "$USER_INSTALL" = 1 ]; then
  U=${SUDO_USER:-}
  [ -n "$U" ] || { echo "SUDO_USER 为空，跳过；请用 sudo 运行或手动安装"; U=""; }
  if [ -n "$U" ]; then
    H=$(getent passwd "$U" | cut -d: -f6)
    install -d -o "$U" -g "$U" "$H/.local/bin" "$H/.config/systemd/user"
    install -m 0755 -o "$U" -g "$U" "$P/system/user-bin/fp-sudo-prompt-watch" "$H/.local/bin/"
    install -m 0644 -o "$U" -g "$U" "$P/system/systemd/user/fp-sudo-prompt-watch.service" "$H/.config/systemd/user/"
    runuser -u "$U" -- systemctl --user daemon-reload || true
    runuser -u "$U" -- systemctl --user enable --now fp-sudo-prompt-watch.service || true
  fi
else
  echo "  跳过（手动：cp 到 ~/.local/bin 与 ~/.config/systemd/user 后 systemctl --user enable --now fp-sudo-prompt-watch）"
fi

echo "== 6) 重新加载并启用"
systemctl daemon-reload
udevadm control --reload-rules || true
udevadm trigger --action=add --subsystem-match=misc --sysname-match=goodix_fp || true
systemctl enable qsee-supplicant.service
systemctl enable fp-ta-watchdog.service
systemctl enable fprintd.service 2>/dev/null || true
systemctl restart qsee-supplicant.service || true
systemctl restart fp-ta-watchdog.service || true

cat <<'NEXT'

完成。还需要按顺序完成（见 fingerprint/INSTALL.md）：
  1) 内核：../kernel/ 下的 QSEECOM/mdt_loader/qcom_scm/goodix_fp_spi 编译安装 + 联合 DTB；
  2) TA：从你自己的 Android 固件提取 gfenu.* 并放入 /lib/firmware；
  3) qsee-supplicant：cd fingerprint/src/qsee-supplicant && make && install-bin/systemd；
  4) libfprint：按 INSTALL.md 编译 wrobelda/libfprint goodix-qsee + 补丁；
  5) 重启后运行 fingerprint/install/verify-fingerprint.sh。
NEXT
