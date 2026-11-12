#!/bin/bash
# camera/elish-camera/verify.sh — 相机栈只读自检（普通用户可运行；root 更完整）
set -u
pass=0; warn=0; fail=0
ok()  { echo "  OK   $1"; pass=$((pass+1)); }
wk()  { echo "  WARN $1"; warn=$((warn+1)); }
bad() { echo "  FAIL $1"; fail=$((fail+1)); }

echo "== 相机栈自检 $(date -Is) =="
echo "内核: $(uname -r)"

echo "-- 二进制/脚本"
for f in /usr/local/bin/elish-camera /usr/local/bin/elish-camera-ctl /usr/local/bin/elish-camera-scan; do
    [ -x "$f" ] && ok "$f" || bad "缺少 $f（运行 sudo ./camera/install.sh）"
done
for f in elish-camera-init elish-camera-color elish-camera-rear-color elish-camera-rear-init \
         elish-camera-rear-autofocus elish-camera-vcam elish-camera-autostart elish-check-usb; do
    [ -x "/usr/local/bin/$f" ] && ok "/usr/local/bin/$f" || wk "缺少 /usr/local/bin/$f"
done

echo "-- udev / 内核模块"
for r in 99-elish-vcm.rules 99-elish-dma-heap.rules; do
    [ -e "/etc/udev/rules.d/$r" ] && ok "udev $r" || bad "缺少 udev 规则 $r"
done
lsmod 2>/dev/null | grep -q '^v4l2loopback' && ok "v4l2loopback 已加载" || wk "v4l2loopback 未加载（虚拟相机不可用；检查 modules-load.d）"
[ -e /dev/dma_heap/system ] && ok "/dev/dma_heap/system 存在" || bad "缺少 /dev/dma_heap/system（DMABUF heaps 未启用）"

echo "-- 虚拟相机"
for d in /dev/video2 /dev/video20; do
    [ -c "$d" ] && ok "$d 存在" || bad "缺少 $d（后端未启动或 vcam 未创建；见 camera/README.md）"
done

echo "-- 配置 / 自启"
[ -e /etc/elish-orientation.conf ] && ok "orientation 配置存在" || wk "缺 /etc/elish-orientation.conf（运行 sudo ./camera/install.sh 创建）"
if [ -e /etc/elish-orientation.conf ]; then
    if [ -w /etc/elish-orientation.conf ]; then
        ok "orientation 配置当前用户可写（orientation set 无需 sudo）"
    else
        wk "orientation 配置当前用户不可写（orientation set 需要权限；运行 camera/install.sh 修正组权限）"
    fi
fi
[ -e /etc/xdg/autostart/elish-camera-autoserve.desktop ] && ok "后端登录自启已安装" || wk "缺少 /etc/xdg/autostart/elish-camera-autoserve.desktop"
[ -x /usr/local/bin/elish-camera-autostart ] && ok "elish-camera-autostart 存在" || wk "缺少 elish-camera-autostart"

echo "-- 运行时状态"
pgrep -f 'elish-camera (autoserve|serve|stream)' >/dev/null 2>&1 \
    && ok "相机后端进程运行中" \
    || wk "未检测到后端进程（需登录桌面/autostart，或手动 elish-camera-ctl start）"
F=$(ls /sys/bus/i2c/devices/*/focus 2>/dev/null | head -1)
if [ -n "$F" ]; then
    [ -w "$F" ] && ok "VCM focus sysfs 可写（$F）" || wk "VCM focus sysfs 存在但当前用户不可写（检查 99-elish-vcm.rules）"
else
    wk "未找到 CN3927V focus sysfs（后摄 AF 不可用）"
fi

echo
echo "== 结果: OK=$pass WARN=$warn FAIL=$fail =="
if [ "$fail" -eq 0 ]; then
    echo "结论: 相机栈基本正常（WARN 项视使用场景）"
    exit 0
else
    echo "结论: 有失败项，见上"
    exit 1
fi
