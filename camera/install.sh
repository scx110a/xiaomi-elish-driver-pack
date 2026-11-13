#!/bin/bash
# camera/install.sh — 相机子系统一键安装（编译 + 二进制/脚本/规则/虚拟相机/自启 + 休眠恢复）
#
# 用法（root）：
#   sudo ./camera/install.sh [--dry-run] [--skip-apt] [--skip-make]
#
# 安装内容：
#   /usr/local/bin/elish-camera（GPU 全分辨率管线后端）
#   /usr/local/bin/elish-camera-ctl 等辅助脚本
#   udev 规则（VCM 对焦权限、DMA heap）、v4l2loopback 模块配置
#   /etc/xdg/autostart 后端自启、桌面文件
#   camera/system 的休眠唤醒恢复（worker / sleep hook / oneshot service）
set -euo pipefail
TAG=camera
. "$(dirname "$0")/../install/lib/common.sh"

SKIP_APT=0; SKIP_MAKE=0
while [ $# -gt 0 ]; do
    case "$1" in
        --skip-apt) SKIP_APT=1; shift ;;
        --skip-make) SKIP_MAKE=1; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        --yes) ASSUME_YES=1; shift ;;
        -h|--help) awk 'NR>1 { if ($0 ~ /^#/) { sub(/^# ?/, ""); print } else exit }' "$0"; exit 0 ;;
        *) die "未知参数: $1" ;;
    esac
done
need_root
C="$PKG_ROOT/camera"

step "1) 依赖（可选）"
if [ "$SKIP_APT" = 0 ]; then
    run apt-get install -y python3-zxing-cpp zbar-tools || warn "相机可选依赖安装失败（扫码功能可能不可用）"
fi

step "2) 编译 elish-camera（GPU debayer + Venus 编码）"
if [ "$SKIP_MAKE" = 0 ]; then
    run make -C "$C/elish-camera" -j"$(nproc)"
fi
[ -x "$C/elish-camera/build/elish-camera" ] || [ "$DRY_RUN" = 1 ] || die "编译产物不存在: camera/elish-camera/build/elish-camera"

step "3) 安装二进制与辅助脚本"
run install -m 0755 "$C/elish-camera/build/elish-camera" /usr/local/bin/elish-camera
for f in elish-camera-ctl elish-camera-init elish-camera-scan elish-camera-color \
         elish-camera-rear-color elish-camera-rear-init elish-camera-rear-autofocus \
         elish-camera-vcam elish-camera-switch elish-camera-gui elish-check-usb \
         elish-camera-autostart; do
    [ -f "$C/$f" ] && run install -m 0755 "$C/$f" /usr/local/bin/
done

step "4) udev / 内核模块配置"
for r in 99-elish-vcm.rules 99-elish-dma-heap.rules; do
    [ -f "$C/$r" ] && run install -m 0644 "$C/$r" /etc/udev/rules.d/
done
[ -f "$C/elish-vcam.modprobe.conf" ] && run install -m 0644 "$C/elish-vcam.modprobe.conf" /etc/modprobe.d/elish-vcam.conf
[ -f "$C/elish-vcam.modules-load.conf" ] && run install -m 0644 "$C/elish-vcam.modules-load.conf" /etc/modules-load.d/elish-vcam.conf

step "4b) orientation 配置权限（删除 sudo 免密后仍可由用户写入）"
ORIENT_CONF=/etc/elish-orientation.conf
U=${SUDO_USER:-}
if [ -e "$ORIENT_CONF" ]; then
    log "保留已有配置: $ORIENT_CONF"
else
    printf '# elish orientation settings (edit with: elish-camera-ctl orientation)\ntablet_mode=auto\nrebuild_mode=idle\n' > "$ORIENT_CONF"
    log "已创建默认配置: $ORIENT_CONF"
fi
if [ -n "$U" ] && [ "$U" != root ]; then
    chgrp "$(id -gn "$U")" "$ORIENT_CONF"
    chmod 0664 "$ORIENT_CONF"
    log "已设组可写: $(ls -l "$ORIENT_CONF")"
fi
# VCM focus sysfs 的 video 组可写由 udev 规则 99-elish-vcm.rules 保证

step "5) 桌面集成与后端自启"
[ -f "$C/elish-camera-autoserve.desktop" ] && run install -m 0644 "$C/elish-camera-autoserve.desktop" /etc/xdg/autostart/
for d in elish-camera-panel.desktop; do
    [ -f "$C/$d" ] && run install -m 0644 "$C/$d" /usr/share/applications/
done

step "6) 休眠唤醒后传感器/自动旋转恢复"
run install -m 0755 "$C/system/elish-sensors-resume-worker" /usr/local/sbin/
run install -d /usr/lib/systemd/system-sleep
run install -m 0755 "$C/system/elish-sensors-resume" /usr/lib/systemd/system-sleep/
run install -m 0644 "$C/system/elish-sensors-resume.service" /etc/systemd/system/
run systemctl daemon-reload

step "7) 重载 udev 规则"
run udevadm control --reload-rules 2>/dev/null || true

step "完成"
cat <<EOF
相机子系统已安装。
- 当前桌面会话如需立即生效：注销重登录（autostart 会拉起 elish-camera autoserve），
  或手动：elish-camera-ctl start
- 验证：elish-camera-ctl panel / elish-camera-scan；ls -l /dev/video2 /dev/video20
- 休眠唤醒恢复：合盖再唤醒后自动旋转应 ~10s 恢复（journalctl -t elish-sensors-resume -b）
  连续睡眠加固：pre 会取消未完成的恢复 worker + flock 单实例 + sleep delay inhibitor；
  第二次合盖会等本轮恢复结束（本机系统 InhibitDelayMaxSec=30s，足够 ~10s 恢复）再 suspend。
EOF
