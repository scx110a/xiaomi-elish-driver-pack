#!/bin/bash
# install/extract-all.sh — 自动提取设备数据/固件（本包不附带这些文件）
#
# 用法：
#   sudo ./install/extract-all.sh --android <vendor.img|目录> --pmos <slpi.apk|目录> [选项]
# 依赖：debugfs(e2fsprogs)、tar/gzip；指纹 --adb 需要 adb。
#
# 可选参数：
#   --armbian-root <目录>    armbian-firmware 根目录（内含 lib/firmware/...；默认自动找）
#   --fingerprint <目录|firmware_mnt.img>   指纹 TA 来源
#   --fingerprint-adb        从 adb（需 root）提取指纹 TA
#   --out <目录>             输出目录（默认 /var/tmp/elish-build/extracted）
#   --skip-sensors / --skip-fingerprint
#   --dry-run                只打印将执行的命令
#   --yes                    不交互
#
# 数据来源不在包里；脚本会自动在常见位置查找，找不到会打印精确获取方式后退出。
set -euo pipefail
TAG=extract
. "$(dirname "$0")/lib/common.sh"

ANDROID=""; PMOS=""; ARMB=""; FP=""; FP_ADB=0; SKIP_SENSORS=0; SKIP_FP=0
while [ $# -gt 0 ]; do
    case "$1" in
        --android) ANDROID=${2:-}; shift 2 ;;
        --pmos) PMOS=${2:-}; shift 2 ;;
        --armbian-root) ARMB=${2:-}; shift 2 ;;
        --fingerprint) FP=${2:-}; shift 2 ;;
        --fingerprint-adb) FP_ADB=1; shift ;;
        --out) EXTRACT_DIR=${2:-}; shift 2 ;;
        --skip-sensors) SKIP_SENSORS=1; shift ;;
        --skip-fingerprint) SKIP_FP=1; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        --yes) ASSUME_YES=1; shift ;;
        -h|--help) awk 'NR>1 { if ($0 ~ /^#/) { sub(/^# ?/, ""); print } else exit }' "$0"; exit 0 ;;
        *) die "未知参数: $1（-h 查看用法）" ;;
    esac
done

mkdir_build
log "输出目录: $EXTRACT_DIR"

# ---------- 自动探测输入 ----------
find_android() {
    local f
    [ -n "$ANDROID" ] && { printf '%s\n' "$ANDROID"; return 0; }
    f=$(detect_path \
        "${ELISH_ANDROID_VENDOR:-}" \
        "$PWD/vendor_a.img" "$PWD/android-vendor.img" \
        "$HOME/vendor_a.img" "$HOME/android-vendor.img" \
        "$HOME/Downloads/vendor_a.img" "$HOME/Downloads/android-vendor.img" \
        "/root/vendor_a.img" "/var/tmp/vendor_a.img") && { printf '%s\n' "$f"; return 0; }
    return 1
}
find_pmos() {
    local f
    if [ -n "$PMOS" ]; then printf '%s\n' "$PMOS"; return 0; fi
    for f in "$PWD"/firmware-xiaomi-elish-slpi-*.apk \
             "$HOME"/Downloads/firmware-xiaomi-elish-slpi-*.apk \
             "$HOME"/firmware-xiaomi-elish-slpi-*.apk \
             /root/firmware-xiaomi-elish-slpi-*.apk; do
        [ -e "$f" ] && { printf '%s\n' "$f"; return 0; }
    done
    f=$(detect_path "$PWD/slpi.mbn" "$HOME/Downloads/slpi.mbn") && { printf '%s\n' "$f"; return 0; }
    return 1
}

# ---------- 传感器数据 ----------
if [ "$SKIP_SENSORS" = 0 ]; then
    step "传感器数据 / SLPI 固件"
    command -v debugfs >/dev/null || warn "缺少 debugfs（sudo apt install e2fsprogs），Android 镜像提取会失败"

    step "  1/3 armbian-firmware 数据"
    if [ -n "$ARMB" ]; then
        run env ARMBIAN_FW_ROOT="$ARMB" bash "$PKG_ROOT/sensors/extract/extract-from-armbian-firmware.sh" "$EXTRACT_DIR/armbian"
    else
        run bash "$PKG_ROOT/sensors/extract/extract-from-armbian-firmware.sh" "$EXTRACT_DIR/armbian"
    fi

    step "  2/3 Android vendor 数据"
    AV=""
    AV=$(find_android) || true
    if [ -z "$AV" ]; then
        if [ "$DRY_RUN" = 1 ]; then
            warn "DRY: 未找到 Android vendor 镜像（真实运行需 --android 指定）"
        else
            die "找不到 Android vendor 镜像。请显式指定：
    sudo ./install/extract-all.sh --android /path/to/android-vendor.img --pmos /path/to/firmware-xiaomi-elish-slpi-*.apk
  （vendor 镜像可从本机 Android vendor 分区只读 dump；本包不附带）"
        fi
    fi
    if [ -n "$AV" ]; then
        if [ ! -e "$AV" ]; then
            if [ "$DRY_RUN" = 1 ]; then warn "DRY: 输入不存在: $AV（演示跳过）"; AV=""
            else die "Android vendor 输入不存在: $AV"; fi
        fi
    fi
    if [ -n "$AV" ]; then
        if [ -d "$AV" ]; then
            [ -d "$AV/sensors" ] || die "目录 $AV 下没有 sensors/（应含 config/、proto/ 等）"
            log "Android 数据目录: $AV"
            run mkdir -p "$EXTRACT_DIR/android"
            run cp -a "$AV/sensors" "$EXTRACT_DIR/android/"
            [ -d "$AV/acdb" ] && run cp -a "$AV/acdb" "$EXTRACT_DIR/android/"
        else
            log "Android vendor 镜像: $AV"
            run bash "$PKG_ROOT/sensors/extract/extract-from-android-vendor-img.sh" "$AV" "$EXTRACT_DIR/android"
        fi
    fi

    step "  3/3 SLPI 固件（pMOS APK）"
    PM=""
    PM=$(find_pmos) || true
    if [ -z "$PM" ]; then
        if [ "$DRY_RUN" = 1 ]; then
            warn "DRY: 未找到 pMOS 固件包（真实运行需 --pmos 指定）"
        else
            die "找不到 pMOS 固件包。请显式指定：
    sudo ./install/extract-all.sh ... --pmos /path/to/firmware-xiaomi-elish-slpi-*.apk
  （pMOS 包名 firmware-xiaomi-elish-slpi-20240917-r3.apk；解包后内含 slpi.mbn）"
        fi
    fi
    if [ -n "$PM" ]; then
        if [ ! -e "$PM" ]; then
            if [ "$DRY_RUN" = 1 ]; then warn "DRY: 输入不存在: $PM（演示跳过）"; PM=""
            else die "pMOS 输入不存在: $PM"; fi
        fi
    fi
    if [ -n "$PM" ]; then
        if [ -d "$PM" ]; then
            if [ -f "$PM/slpi.mbn" ]; then
                run mkdir -p "$EXTRACT_DIR/pmos/firmware"
                run cp -a "$PM/slpi.mbn" "$EXTRACT_DIR/pmos/firmware/slpi.mbn"
            elif [ -f "$PM/firmware/slpi.mbn" ]; then
                run mkdir -p "$EXTRACT_DIR/pmos"
                run cp -a "$PM/firmware/slpi.mbn" "$EXTRACT_DIR/pmos/firmware/slpi.mbn"
            else
                die "目录 $PM 里没有 slpi.mbn"
            fi
        else
            log "pMOS APK: $PM"
            run bash "$PKG_ROOT/sensors/extract/extract-pmos-slpi-mbn.sh" "$PM" "$EXTRACT_DIR/pmos"
        fi
    fi

    # 结果校验（dry-run 跳过）
    if [ "$DRY_RUN" = 0 ]; then
        for d in "$EXTRACT_DIR/armbian/sensors/registry" "$EXTRACT_DIR/armbian/socinfo" \
                 "$EXTRACT_DIR/android/sensors" "$EXTRACT_DIR/pmos/firmware/slpi.mbn"; do
            [ -e "$d" ] || die "提取结果缺失: $d"
        done
        nreg=$(find "$EXTRACT_DIR/armbian/sensors/registry" -type f 2>/dev/null | wc -l)
        ncfg=$(find "$EXTRACT_DIR/armbian/sensors/config" -type f 2>/dev/null | wc -l)
        nproto=$(find "$EXTRACT_DIR/android/sensors/proto" -type f 2>/dev/null | wc -l)
        log "校验: registry=$nreg（期望155） config=$ncfg（期望40） proto=$nproto（期望79）"
        [ "$nreg" -eq 155 ] || die "registry 数量不是 155，数据不完整"
        [ "$ncfg" -eq 40 ]  || die "config 数量不是 40，数据不完整"
        [ "$nproto" -eq 79 ] || warn "proto 数量 $nproto ≠ 79（可接受？）"
        log "SLPI 固件: $(md5sum "$EXTRACT_DIR/pmos/firmware/slpi.mbn" | cut -d' ' -f1)（期望 51c3b3bef20a76d386a516dfbcc72b68）"
    fi
fi

# ---------- 指纹 TA（可选） ----------
if [ "$SKIP_FP" = 0 ]; then
    step "指纹 TA（gfenu.*，可选）"
    FP_OUT="$EXTRACT_DIR/fingerprint-ta"
    if [ "$FP_ADB" = 1 ]; then
        run bash "$PKG_ROOT/fingerprint/extract/extract-gfenu-ta.sh" --adb "$FP_OUT"
    elif [ -n "$FP" ]; then
        run bash "$PKG_ROOT/fingerprint/extract/extract-gfenu-ta.sh" "$FP" "$FP_OUT"
    elif [ -f /lib/firmware/gfenu.mdt ]; then
        log "系统已装 TA（/lib/firmware/gfenu.*），复制一份到提取目录"
        run mkdir -p "$FP_OUT"
        run cp -a /lib/firmware/gfenu.* "$FP_OUT/"
    else
        FPI=$(detect_path "$HOME/firmware_mnt.img" "$PWD/firmware_mnt.img" "/root/firmware_mnt.img" || true)
        if [ -n "${FPI:-}" ]; then
            run bash "$PKG_ROOT/fingerprint/extract/extract-gfenu-ta.sh" "$FPI" "$FP_OUT"
        else
            warn "未找到指纹 TA 来源（可选）：用 --fingerprint <目录|firmware_mnt.img> 或 --fingerprint-adb 指定；本次跳过。"
            SKIP_FP=1
        fi
    fi
fi

step "提取完成"
cat <<EOF
提取目录: $EXTRACT_DIR
下一步（安装器会自动执行）:
  sudo $PKG_ROOT/sensors/install/install-data.sh \\
      --android "$EXTRACT_DIR/android" \\
      --armbian  "$EXTRACT_DIR/armbian" \\
      --firmware "$EXTRACT_DIR/pmos"
EOF
