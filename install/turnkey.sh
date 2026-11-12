#!/bin/bash
# install/turnkey.sh — 合并包一键安装器（尽可能自动；无法自动的部分会给出明确指令）
#
# 用法（推荐从包根目录执行）：
#   sudo ./install.sh --android /path/android-vendor.img --pmos /path/firmware-xiaomi-elish-slpi-*.apk
#   sudo ./install.sh --android ... --pmos ... --fingerprint-adb --with-gnss --with-camera
#   sudo ./install.sh --dry-run            # 只打印每一步将做什么（不写系统）
#
# 常用参数：
#   --android <img|dir>    Android vendor 镜像/目录（传感器 proto/hals 等）
#   --pmos <apk|dir>       pMOS SLPI 固件包（内含 slpi.mbn）
#   --fingerprint <dir|img> 指纹 TA 来源；--fingerprint-adb 从 adb 提取；都不给则自动找/跳过
#   --with-gnss / --no-gnss        GNSS 子系统（默认装）
#   --with-camera / --no-camera    相机子系统（默认装）
#   --no-fingerprint               跳过指纹
#   --with-kernel --kernel-src DIR [--write-boot]   内核构建+安装（写 boot_b，显式 opt-in）
#   --with-regver          额外构建 regver-v1 回退版 hexagonrpcd
#   --force                已安装的组件也重编
#   --deps / --no-deps     自动安装 apt 依赖（默认装）
#   --skip-extract / --skip-build / --skip-install / --skip-check
#   --pam                  安装 sudo 指纹 PAM（安全敏感，需显式；脚本会备份）
#   --yes                  全部默认确认；--dry-run 只演示
#
# 分阶段执行示例：
#   sudo ./install.sh --skip-extract --skip-build          # 只装系统集成文件
#   sudo ./install.sh --skip-build --skip-install --skip-check   # 只提取数据
set -euo pipefail
TAG=turnkey
. "$(dirname "$0")/lib/common.sh"

ANDROID=""; PMOS=""; FP=""; FP_ADB=0
WITH_GNSS=1; WITH_CAMERA=1; WITH_FP=1; WITH_KERNEL=0; WITH_REGVER=0
DO_DEPS=1; SKIP_EXTRACT=0; SKIP_BUILD=0; SKIP_INSTALL=0; SKIP_CHECK=0
PAM=0; FORCE=0; KSRC=""; WRITE_BOOT=0

while [ $# -gt 0 ]; do
    case "$1" in
        --android) ANDROID=${2:-}; shift 2 ;;
        --pmos) PMOS=${2:-}; shift 2 ;;
        --fingerprint) FP=${2:-}; shift 2 ;;
        --fingerprint-adb) FP_ADB=1; shift ;;
        --with-gnss) WITH_GNSS=1; shift ;;
        --no-gnss) WITH_GNSS=0; shift ;;
        --with-camera) WITH_CAMERA=1; shift ;;
        --no-camera) WITH_CAMERA=0; shift ;;
        --with-fingerprint) WITH_FP=1; shift ;;
        --no-fingerprint) WITH_FP=0; shift ;;
        --with-kernel) WITH_KERNEL=1; shift ;;
        --kernel-src) KSRC=${2:-}; shift 2 ;;
        --write-boot) WRITE_BOOT=1; shift ;;
        --with-regver) WITH_REGVER=1; shift ;;
        --no-deps) DO_DEPS=0; shift ;;
        --deps) DO_DEPS=1; shift ;;
        --skip-extract) SKIP_EXTRACT=1; shift ;;
        --skip-build) SKIP_BUILD=1; shift ;;
        --skip-install) SKIP_INSTALL=1; shift ;;
        --skip-check) SKIP_CHECK=1; shift ;;
        --pam) PAM=1; shift ;;
        --force) FORCE=1; shift ;;
        --yes) ASSUME_YES=1; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) awk 'NR>1 { if ($0 ~ /^#/) { sub(/^# ?/, ""); print } else exit }' "$0"; exit 0 ;;
        *) die "未知参数: $1（-h 查看用法）" ;;
    esac
done
export DRY_RUN ASSUME_YES

need_root
[ "$DRY_RUN" = 1 ] || mkdir -p "$BUILD_DIR" "$EXTRACT_DIR"

step "0) 环境检查"
MODEL=$(tr -d '\0' < /sys/firmware/devicetree/base/model 2>/dev/null || echo '?')
KREL=$(uname -r)
log "设备: $MODEL"
log "内核: $KREL"
case "$MODEL" in
    *elish*|*"Mi Pad 5 Pro"*) log "机型匹配 Xiaomi elish ✅" ;;
    *) warn "机型看起来不是 Xiaomi Pad 5 Pro (elish)：$MODEL —— 继续有风险，请确认后重试。" ;;
esac
[ "$(id -u)" -eq 0 ] || [ "$DRY_RUN" = 1 ] || die "请用 sudo 运行"

# ---------------- 依赖 ----------------
if [ "$DO_DEPS" = 1 ]; then
    step "1) apt 依赖"
    REQ="build-essential gcc make git meson ninja-build pkg-config e2fsprogs python3 \
         libglib2.0-dev libgudev-1.0-dev libpolkit-gobject-1-dev libqmi-glib-dev libqrtr-glib-dev \
         libprotobuf-c-dev protobuf-c-compiler libgusb-dev libusb-1.0-0-dev libpixman-1-dev \
         libcairo2-dev libgirepository1.0-dev libssl-dev pahole dtc mkbootimg fdtget gpsd gpsd-clients \
         libegl-dev libgles-dev libdrm-dev libgbm-dev libpipewire-0.3-dev zlib1g-dev"
    if [ "$DRY_RUN" = 1 ]; then
        echo "    DRY: apt-get update && apt-get install -y $REQ"
    else
        apt-get update -qq || warn "apt update 失败（继续，可能已有缓存）"
        # shellcheck disable=SC2086
        apt-get install -y $REQ || die "依赖安装失败；可手动安装后重试，或 --no-deps 跳过"
        for opt in python3-zxing-cpp zbar-tools android-sdk-libsparse-utils; do
            apt-get install -y "$opt" >/dev/null 2>&1 || warn "可选依赖未装: $opt"
        done
    fi
fi

# ---------------- 提取 ----------------
if [ "$SKIP_EXTRACT" = 0 ]; then
    step "2) 提取设备数据/固件（本包不附带）"
    EXARGS=(--out "$EXTRACT_DIR")
    [ -n "$ANDROID" ] && EXARGS+=(--android "$ANDROID")
    [ -n "$PMOS" ] && EXARGS+=(--pmos "$PMOS")
    [ -n "$FP" ] && EXARGS+=(--fingerprint "$FP")
    [ "$FP_ADB" = 1 ] && EXARGS+=(--fingerprint-adb)
    [ "$WITH_FP" = 0 ] && EXARGS+=(--skip-fingerprint)
    run bash "$PKG_ROOT/install/extract-all.sh" "${EXARGS[@]}"
else
    log "跳过提取（使用已有: $EXTRACT_DIR）"
fi

# ---------------- 组装传感器数据母本 ----------------
step "3) 组装传感器数据母本 + 固件路径不变量"
if [ -d "$EXTRACT_DIR/android/sensors" ] && [ -f "$EXTRACT_DIR/pmos/firmware/slpi.mbn" ]; then
    run bash "$PKG_ROOT/sensors/install/install-data.sh" \
        --android "$EXTRACT_DIR/android" \
        --armbian "$EXTRACT_DIR/armbian" \
        --firmware "$EXTRACT_DIR/pmos"
else
    if [ "$DRY_RUN" = 1 ]; then
        echo "    DRY: install-data.sh（提取结果尚不存在，真实运行时需先提取）"
    elif [ "$SKIP_INSTALL" = 1 ]; then
        warn "跳过数据组装（--skip-install；提取结果不存在）"
    else
        die "缺少传感器提取结果：$EXTRACT_DIR/android/sensors 或 $EXTRACT_DIR/pmos/firmware/slpi.mbn
  先运行: sudo ./install.sh --android <vendor.img> --pmos <slpi.apk>"
    fi
fi

# ---------------- 构建用户态 ----------------
if [ "$SKIP_BUILD" = 0 ]; then
    step "4) 自动打补丁 + 编译用户态（hexrpc/libssc/iio/qsee/libfprint/fprintd）"
    BARGS=()
    [ "$FORCE" = 1 ] && BARGS+=(--force)
    [ "$WITH_REGVER" = 1 ] && BARGS+=(--with-regver)
    [ "$DRY_RUN" = 1 ] && BARGS+=(--dry-run)
    run bash "$PKG_ROOT/install/build-userspace.sh" "${BARGS[@]}"
else
    log "跳过用户态构建"
fi

# ---------------- 安装系统集成 ----------------
if [ "$SKIP_INSTALL" = 0 ]; then
    step "5) 安装系统集成文件"
    run bash "$PKG_ROOT/sensors/install/install-system-files.sh"

    if [ "$WITH_FP" = 1 ]; then
        step "5a) 指纹 TA + 系统集成"
        if run test -f "$EXTRACT_DIR/fingerprint-ta/gfenu.mdt"; then
            run install -m 0644 "$EXTRACT_DIR"/fingerprint-ta/gfenu.* /lib/firmware/
            log "TA 已安装到 /lib/firmware/gfenu.*"
        else
            warn "未提取到指纹 TA，跳过 TA 安装（可 --fingerprint-adb 重跑）"
        fi
        FPARGS=()
        [ "$PAM" = 1 ] && FPARGS+=(--pam)
        run bash "$PKG_ROOT/fingerprint/install/install-fingerprint-files.sh" "${FPARGS[@]}"
        run systemctl daemon-reload
        run systemctl enable --now qsee-supplicant.service || warn "qsee-supplicant 启动失败（TA/内核未就绪？）"
        run systemctl enable --now fp-ta-watchdog.service || warn "fp-ta-watchdog 启动失败（TA 未装？）"
        run systemctl restart fprintd 2>/dev/null || warn "fprintd 重启失败（可能尚未安装）"
    else
        log "跳过指纹（--no-fingerprint）"
    fi

    if [ "$WITH_GNSS" = 1 ]; then
        step "5b) GNSS 用户态（gpsd 集成）"
        run bash "$PKG_ROOT/gnss/install/install.sh"
    else
        log "跳过 GNSS（--no-gnss）"
    fi

    if [ "$WITH_CAMERA" = 1 ]; then
        step "5c) 相机（GPU 管线 + 虚拟摄像头 + 休眠唤醒恢复）"
        run bash "$PKG_ROOT/camera/install.sh"
    else
        log "跳过相机（--no-camera）"
    fi
else
    log "跳过系统集成安装"
fi

# ---------------- 内核（可选，显式 opt-in） ----------------
if [ "$WITH_KERNEL" = 1 ]; then
    step "6) 内核构建 + 安装（写 boot_b，不碰 boot_a）"
    [ -n "$KSRC" ] || die "--with-kernel 需要 --kernel-src <同版本内核源码树>（如 /var/tmp/kbuild-src）"
    KARGS=(--kernel-src "$KSRC")
    [ "$WRITE_BOOT" = 1 ] && KARGS+=(--write-boot)
    [ "$DRY_RUN" = 1 ] && KARGS+=(--dry-run)
    [ "$ASSUME_YES" = 1 ] && KARGS+=(--yes)
    run bash "$PKG_ROOT/kernel/build-and-install.sh" "${KARGS[@]}"
else
    log "跳过内核（未加 --with-kernel）。单独执行：sudo ./kernel/build-and-install.sh --kernel-src <内核源码> --write-boot"
fi

# ---------------- 自检 ----------------
if [ "$SKIP_CHECK" = 0 ]; then
    step "7) 合并自检（重启后运行更准确）"
    run bash "$PKG_ROOT/install/check-all.sh" || warn "自检有失败项（若尚未重启，属正常；重启后再跑一次）"
fi

step "完成 —— 还需要的手动步骤（很少）"
cat <<EOF
1) 重启使内核/服务/固件路径生效：sudo reboot
2) 重启后验证：sudo $PKG_ROOT/install/check-all.sh
   或 sudo $PKG_ROOT/sensors/install/verify.sh && sensor-status
3) 指纹录入（如装了指纹）：fprintd-enroll -f left-index-finger "\$USER"
4) 可选 sudo 指纹 PAM：sudo ./install.sh --no-deps --skip-extract --skip-build --pam --yes
   或：sudo $PKG_ROOT/fingerprint/install/install-fingerprint-files.sh --pam --user
5) 内核若未自动装：sudo $PKG_ROOT/kernel/build-and-install.sh --kernel-src <源码> --write-boot
6) 解压/移植时若换了目录：所有路径均由脚本自身定位，无需改文件。
EOF
log "日志/提取目录保留在 $BUILD_DIR（可删）"
