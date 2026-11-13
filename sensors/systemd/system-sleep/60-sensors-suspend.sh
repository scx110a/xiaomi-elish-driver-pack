#!/bin/bash
# 休眠/唤醒维护：只重置 iio 与 attach 会话，不动固件
#
# 2026-11-12 加速改造：post 不再无条件另起 listener。常驻的 hexrpc-sdsp-service.sh
# 本身有 while 循环，会在 suspend 断开 fastrpc 会话后自动重启 hexagonrpcd；若 post
# 钩子此时再起一个 listener，会出现多个 hexagonrpcd 争抢 /dev/fastrpc-sdsp，
# 唤醒后 attach 抖动（实测 2026-11-12 23:27 一次唤醒出现数十次 attach/exited）。
# 因此仅在 "没有 listener 脚本进程" 时兜底拉起；真正确保 listener 存活由
# elish-sensors-resume-worker（独立 cgroup）负责。
case "$1" in
  pre)
    systemctl stop iio-sensor-proxy.service 2>/dev/null
    ;;
  post)
    if ! pgrep -f '/usr/local/sbin/hexrpc-sdsp-service.sh' >/dev/null 2>&1; then
      setsid /usr/local/sbin/hexrpc-sdsp-service.sh >> /var/log/hexrpc-boot.log 2>&1 &
    fi
    # pre 已经 stop 掉 iio；这里用 start，避免与 elish-sensors-resume worker
    # 随后的“等加速计就绪再 restart”叠成每次都重启两次。
    systemctl start iio-sensor-proxy.service 2>/dev/null
    ;;
esac
exit 0
