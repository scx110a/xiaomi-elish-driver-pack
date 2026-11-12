#!/bin/bash
# 40-cancel-scheduled-shutdown.sh — 防止"定时关机/重启"与 suspend 撞车
#
# 背景（2026-11-11 18:16 事故）：自动化安排的 `shutdown -r +1` 在 18:16:41 开始，
# 用户在 18:16:42 合盖 suspend，系统进入"半关机 + 休眠"的中间状态；18:32 唤醒后
# 认证栈（gdm/session/PAM/fprintd）处于损坏状态，密码和指纹解锁全部失败，最终只能
# 强制重启。这个 hook 在 suspend 前取消尚未执行的定时 shutdown/reboot，并记录诊断
# 信息到 /var/lib/suspend-watch.log，避免再次出现无法解锁的状态。
set -u

LOG=/var/lib/suspend-watch.log
STATE=/run/systemd/shutdown/scheduled

log() { echo "$(date -Is) [sleep-guard] $*" >> "$LOG" 2>/dev/null || true; }

diag() {
  log "$1 diag: systemd=$(systemctl is-system-running 2>/dev/null || echo '?') gdm=$(systemctl is-active gdm 2>/dev/null || echo '?') fprintd=$(systemctl is-active fprintd 2>/dev/null || echo '?') ta=$(systemctl is-active 'qsee-app-loader@gfenu' 2>/dev/null || echo '?') gnome-shell=$(pgrep -c gnome-shell 2>/dev/null || echo 0)"
}

case "${1:-}" in
  pre)
    if [ -e "$STATE" ]; then
      log "WARN pending scheduled shutdown detected before suspend; canceling it to avoid half-shutdown+suspend collision"
      command -v shutdown >/dev/null 2>&1 && shutdown -c >/dev/null 2>&1 || true
      sleep 1
      if [ -e "$STATE" ]; then
        log "WARN scheduled shutdown still present after cancel attempt"
      else
        log "pending scheduled shutdown canceled"
      fi
    fi
    diag pre
    ;;
  post)
    diag post
    ;;
esac
exit 0
