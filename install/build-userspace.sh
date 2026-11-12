#!/bin/bash
# install/build-userspace.sh — 自动打补丁 + 编译 + 安装用户态组件
#
# 覆盖：hexagonrpcd（含 M1/M2 补丁）、libssc、iio-sensor-proxy、qsee-supplicant、
#       libfprint(goodixqsee)、fprintd 依赖；已安装且未 --force 时自动跳过。
#
# 用法： sudo ./install/build-userspace.sh [--force] [--dry-run] [--with-regver]
#        [--skip-hexrpc] [--skip-libssc] [--skip-iio] [--skip-supplicant]
#        [--skip-libfprint] [--skip-fprintd]
#
# 源码缓存：/var/tmp/elish-build（可用 ELISH_BUILD_DIR 覆盖；不写进包目录）
# 需要网络（git clone）与 apt 依赖（sudo ./install.sh --deps 可自动装）。
set -euo pipefail
TAG=build
. "$(dirname "$0")/lib/common.sh"

FORCE=0; WITH_REGVER=0
SKIP_HEXRPC=0; SKIP_LIBssc=0; SKIP_IIO=0; SKIP_SUP=0; SKIP_LFP=0; SKIP_FPRINTD=0
while [ $# -gt 0 ]; do
    case "$1" in
        --force) FORCE=1; shift ;;
        --with-regver) WITH_REGVER=1; shift ;;
        --skip-hexrpc) SKIP_HEXRPC=1; shift ;;
        --skip-libssc) SKIP_LIBssc=1; shift ;;
        --skip-iio) SKIP_IIO=1; shift ;;
        --skip-supplicant) SKIP_SUP=1; shift ;;
        --skip-libfprint) SKIP_LFP=1; shift ;;
        --skip-fprintd) SKIP_FPRINTD=1; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) awk 'NR>1 { if ($0 ~ /^#/) { sub(/^# ?/, ""); print } else exit }' "$0"; exit 0 ;;
        *) die "未知参数: $1" ;;
    esac
done
need_root
mkdir_build

HEX_URL=https://github.com/linux-msm/hexagonrpc
LIBssc_URL=https://codeberg.org/mmcallister/libssc
LIBssc_REF=4e4c6c8cc702a754bd700506c01961aaf971a23b
IIO_URL=https://gitlab.freedesktop.org/hadess/iio-sensor-proxy
IIO_REF=ca0d5b0
LFP_URL=https://github.com/wrobelda/libfprint
LFP_REF=430e24e538ba137b6edcfe2e1b3b8cc29c66fa6a

meson_setup() { # $1=dir $2=extra-args...
    local dir=$1; shift
    if [ -d "$dir/build" ] && [ "$FORCE" = 0 ]; then
        run meson setup --reconfigure "$dir/build" "$@" >/dev/null || run meson setup "$dir/build" "$@"
    else
        run rm -rf "$dir/build"
        run meson setup "$dir/build" "$@"
    fi
}

# ---------------- hexagonrpcd（M2 可写 registry） ----------------
build_hexrpc() {
    local dir=$BUILD_DIR/hexagonrpc
    local P=$PKG_ROOT/sensors/userspace/hexrpc/patches
    step "hexagonrpcd 0.3.2 + warm-attach + M2 可写 registry"
    if [ "$FORCE" = 0 ] && [ -x /opt/hexrpc-032b/bin/hexagonrpcd.writable-v1 ] \
       && [ -x /opt/hexrpc-032b/bin/hexagonrpcd ]; then
        log "已安装，跳过（--force 可重编）"; return 0
    fi
    need_cmd git; need_cmd meson; need_cmd ninja
    git_checkout "$HEX_URL" "$dir" v0.3.2
    apply_patch "$dir" "$P/0001-hexrpc-0.3.2-warm-attach.patch"
    apply_patch "$dir" "$P/0003-hexagonrpcd-add-writable-hexagonfs.patch"
    apply_patch "$dir" "$P/0004-hexagonrpcd-support-writable-sensor-registry.patch"
    apply_patch "$dir" "$P/0005-hexagonrpcd-support-large-and-extended-callbacks.patch"
    meson_setup "$dir" --prefix=/opt/hexrpc-032b
    run ninja -C "$dir/build"
    run ninja -C "$dir/build" install
    run install -m 0755 "$dir/build/hexagonrpcd/hexagonrpcd" /opt/hexrpc-032b/bin/hexagonrpcd.writable-v1
    # 主二进制也用 M2 版本（监听器缺失 writable-v1 时直接用它，而不是旧的原厂构建）
    run install -m 0755 "$dir/build/hexagonrpcd/hexagonrpcd" /opt/hexrpc-032b/bin/hexagonrpcd
    log "hexagonrpcd.writable-v1 安装完成"

    if [ "$WITH_REGVER" = 1 ]; then
        local rdir=$BUILD_DIR/hexagonrpc-regver
        step "hexagonrpcd 0.3.2 + 0001+0002（regver 回退版，可选）"
        git_checkout "$HEX_URL" "$rdir" v0.3.2
        apply_patch "$rdir" "$P/0001-hexrpc-0.3.2-warm-attach.patch"
        apply_patch "$rdir" "$P/0002-hexrpc-0.3.2-registry-version.patch"
        meson_setup "$rdir" --prefix=/opt/hexrpc-032b
        run ninja -C "$rdir/build"
        run install -m 0755 "$rdir/build/hexagonrpcd/hexagonrpcd" /opt/hexrpc-032b/bin/hexagonrpcd.regver-v1
        log "hexagonrpcd.regver-v1 安装完成（回退用）"
    fi
}

# ---------------- libssc ----------------
build_libssc() {
    local dir=$BUILD_DIR/libssc
    step "libssc 0.4.4"
    if [ "$FORCE" = 0 ] && pkg-config --exists libssc 2>/dev/null; then
        log "已安装 libssc $(pkg-config --modversion libssc)，跳过（--force 可重编）"; return 0
    fi
    need_cmd git; need_cmd meson; need_cmd ninja
    git_checkout "$LIBssc_URL" "$dir" "$LIBssc_REF"
    meson_setup "$dir" --prefix=/usr/local -Dintrospection=false
    run ninja -C "$dir/build"
    run ninja -C "$dir/build" install
    run ldconfig
    log "libssc 安装完成"
}

# ---------------- iio-sensor-proxy ----------------
build_iio() {
    local dir=$BUILD_DIR/iio-sensor-proxy
    local P=$PKG_ROOT/sensors/userspace/iio-sensor-proxy/patches
    step "iio-sensor-proxy（含 hotplug-stayalive 补丁）"
    if [ "$FORCE" = 0 ] && [ -x /usr/libexec/iio-sensor-proxy ]; then
        log "已安装，跳过（--force 可重编）"; return 0
    fi
    need_cmd git; need_cmd meson; need_cmd ninja
    git_checkout "$IIO_URL" "$dir" "$IIO_REF"
    apply_patch "$dir" "$P/0001-iio-sensor-proxy-hotplug-stayalive.patch"
    meson_setup "$dir" --prefix=/usr -Dssc-support=enabled
    run ninja -C "$dir/build"
    run ninja -C "$dir/build" install
    log "iio-sensor-proxy 安装完成"
}

# ---------------- qsee-supplicant ----------------
build_supplicant() {
    local dir=$PKG_ROOT/fingerprint/src/qsee-supplicant
    step "qsee-supplicant（源码随包）"
    if [ "$FORCE" = 0 ] && [ -x /usr/sbin/qsee-supplicant ] \
       && [ -f /usr/lib/systemd/system/qsee-supplicant.service ]; then
        log "已安装，跳过（--force 可重编）"; return 0
    fi
    need_cmd make
    run make -C "$dir"
    run make -C "$dir" check
    run make -C "$dir" install-bin install-systemd install-doc
    run systemctl daemon-reload
    log "qsee-supplicant 安装完成（enable --now 由 turnkey 安装阶段执行）"
}

# ---------------- libfprint (goodixqsee) ----------------
build_libfprint() {
    local dir=$BUILD_DIR/libfprint
    local P=$PKG_ROOT/fingerprint/src/libfprint/patches/libfprint-goodixqsee-logging.patch
    step "libfprint（wrobelda goodix-qsee + 日志补丁）"
    if [ "$FORCE" = 0 ] && strings /usr/lib/aarch64-linux-gnu/libfprint-2.so.2.0.0 2>/dev/null | grep -q goodixqsee; then
        log "已安装 goodixqsee 版 libfprint，跳过（--force 可重编）"; return 0
    fi
    need_cmd git; need_cmd meson; need_cmd ninja
    git_checkout "$LFP_URL" "$dir" "$LFP_REF"
    apply_patch "$dir" "$P"
    meson_setup "$dir" --prefix=/usr -Ddrivers=goodixqsee -Ddoc=false \
        -Dinstalled-tests=false -Dgtk-examples=false -Dintrospection=true
    run ninja -C "$dir/build"
    run ninja -C "$dir/build" install
    run ldconfig
    log "libfprint 安装完成"
}

# ---------------- fprintd ----------------
install_fprintd() {
    step "fprintd / libpam-fprintd（apt）"
    if dpkg -s fprintd >/dev/null 2>&1; then
        log "fprintd 已安装，跳过"
    else
        run apt-get install -y fprintd libpam-fprintd
    fi
}

[ "$SKIP_HEXRPC" = 0 ] && build_hexrpc
[ "$SKIP_LIBssc" = 0 ] && build_libssc
[ "$SKIP_IIO" = 0 ] && build_iio
[ "$SKIP_SUP" = 0 ] && build_supplicant
[ "$SKIP_LFP" = 0 ] && build_libfprint
[ "$SKIP_FPRINTD" = 0 ] && install_fprintd

step "用户态构建完成"
if [ "$DRY_RUN" = 0 ]; then
    echo "  已安装:"
    for f in /opt/hexrpc-032b/bin/hexagonrpcd.writable-v1 /usr/libexec/iio-sensor-proxy /usr/sbin/qsee-supplicant; do
        [ -e "$f" ] && echo "    - $f"
    done
    pkg-config --exists libssc 2>/dev/null && echo "    - libssc $(pkg-config --modversion libssc)"
fi
