#!/bin/bash
# kernel/build-and-install.sh — 自动完成内核补丁/配置/编译/模块/DTB/initramfs/boot 镜像
#
# 用法（需 root）：
#   sudo ./kernel/build-and-install.sh --kernel-src /var/tmp/kbuild-src --write-boot
#   sudo ./kernel/build-and-install.sh --kernel-src ... --dry-run          # 只演示
#   sudo ./kernel/build-and-install.sh --kernel-src ... --skip-build       # 复用已编好的产物
#
# 说明：
# - 源码树必须是与运行内核同版本的 Armbian sm8250 树（见 kernel/ARMBIAN-BASELINE.md / README 步骤 B）。
# - **只写 rootfs 与 boot_b，绝不写 boot_a / Android 分区**；--write-boot 才会重建并写 boot_b
#   （内部调用 kernel/update-boot-image.sh，含备份 + 回读校验）。
# - 会先备份：boot_b、现有 fastrpc/goodix/airoha 模块、联合 DTB、被覆盖的内核源文件。
set -euo pipefail
TAG=kernel
. "$(dirname "$0")/../install/lib/common.sh"

KSRC=""; WRITE_BOOT=0; SKIP_BUILD=0; JOBS=$(nproc)
while [ $# -gt 0 ]; do
    case "$1" in
        --kernel-src) KSRC=${2:-}; shift 2 ;;
        --write-boot) WRITE_BOOT=1; shift ;;
        --skip-build) SKIP_BUILD=1; shift ;;
        --jobs) JOBS=${2:-$(nproc)}; shift 2 ;;
        --yes) ASSUME_YES=1; shift ;;
        --dry-run) DRY_RUN=1; shift ;;
        -h|--help) awk 'NR>1 { if ($0 ~ /^#/) { sub(/^# ?/, ""); print } else exit }' "$0"; exit 0 ;;
        *) die "未知参数: $1" ;;
    esac
done
export DRY_RUN ASSUME_YES
need_root
[ -n "$KSRC" ] || die "缺少 --kernel-src <内核源码树>（本包不附带内核源码；见 kernel/ARMBIAN-BASELINE.md）"
[ -d "$KSRC" ] || die "内核源码目录不存在: $KSRC"

KREL_RUN=$(uname -r)
KREL_SRC=$(make -s -C "$KSRC" kernelrelease 2>/dev/null || cat "$KSRC/include/config/kernel.release" 2>/dev/null || true)
[ -n "$KREL_SRC" ] || die "无法读取源码树内核版本（源码树未配置？先 cp 本包 config 再运行）"
log "运行内核: $KREL_RUN"
log "源码版本: $KREL_SRC"
[ "$KREL_SRC" = "$KREL_RUN" ] || die "源码版本 ($KREL_SRC) != 运行内核 ($KREL_RUN)；ABI 不匹配，禁止安装"

TAGK=$PKG_ROOT/kernel
D=$(date +%Y%m%d-%H%M%S)
BK=/root/elish-kernel-backup-$D
step "0) 备份（$BK）"
run mkdir -p "$BK"
if [ "$WRITE_BOOT" = 1 ]; then
    run dd if=/dev/disk/by-partlabel/boot_b of="$BK/boot_b.img" bs=1M status=none
fi
for f in "/lib/modules/$KREL_RUN/kernel/drivers/misc/fastrpc.ko" \
         "/lib/modules/$KREL_RUN/extra/goodix_fp_spi.ko" \
         "/lib/modules/$KREL_RUN/extra/airoha_gps.ko" \
         "/usr/lib/linux-image-$KREL_RUN/qcom/sm8250-xiaomi-elish-csot.dtb"; do
    [ -e "$f" ] && run cp -a "$f" "$BK/$(echo "$f" | tr '/' '_')"
done
log "备份完成"

step "1) 集成指纹内核文件 + 头文件"
cp_src() { [ -e "$2" ] && run cp -a "$2" "$BK/$(echo "$2" | tr '/' '_')"; run cp -a "$1" "$2"; }
cp_src "$TAGK/include/linux/firmware/qcom/qcom_scm.h" "$KSRC/include/linux/firmware/qcom/qcom_scm.h"
cp_src "$TAGK/include/linux/soc/qcom/mdt_loader.h" "$KSRC/include/linux/soc/qcom/mdt_loader.h"
cp_src "$TAGK/include/uapi/linux/tee.h" "$KSRC/include/uapi/linux/tee.h"
run mkdir -p "$KSRC/drivers/tee/qseecom"
run cp -a "$TAGK/qseecom/." "$KSRC/drivers/tee/qseecom/"
cp_src "$TAGK/mdt_loader.c" "$KSRC/drivers/soc/qcom/mdt_loader.c"
cp_src "$TAGK/qcom_scm.c" "$KSRC/drivers/firmware/qcom/qcom_scm.c"
if ! grep -q 'drivers/tee/qseecom/Kconfig' "$KSRC/drivers/tee/Kconfig"; then
    run_sh "printf '\nsource \"drivers/tee/qseecom/Kconfig\"\n' >> '$KSRC/drivers/tee/Kconfig'"
fi
if ! grep -q 'CONFIG_TEE_QSEECOM' "$KSRC/drivers/tee/Makefile"; then
    run_sh "printf 'obj-\$(CONFIG_TEE_QSEECOM) += qseecom/\n' >> '$KSRC/drivers/tee/Makefile'"
fi

step "2) 应用内核补丁（幂等）"
apply_kpatch() {
    local p=$1
    if patch -p1 -d "$KSRC" --dry-run --forward < "$p" >/dev/null 2>&1; then
        run patch -p1 -d "$KSRC" --forward < "$p"
    elif patch -p1 -d "$KSRC" --dry-run --reverse < "$p" >/dev/null 2>&1; then
        log "已应用，跳过: $(basename "$p")"
    else
        warn "补丁不匹配，跳过: $(basename "$p")（源码版本可能不同）"
    fi
}
P="$TAGK/patches"
apply_kpatch "$P/0001-fastrpc-sdsp-vmid-and-sensors-pd.patch"
apply_kpatch "$P/0002-pinctrl-lpass-lpi-defer-on-clock-timeout.patch"
grep -q 'tristate "Qualcomm MDT firmware loader"' "$KSRC/drivers/soc/qcom/Kconfig" || \
    apply_kpatch "$P/0003-qcom-mdt-loader-user-selectable.patch"
apply_kpatch "$P/0004-smb5-typec-legacy-reset-resume.patch"
apply_kpatch "$P/0005-ov8856-bggr-vflip.patch"
apply_kpatch "$P/0006-ov13b10-of-supplies-reset.patch"
apply_kpatch "$P/0007-cn3927v.patch"
apply_kpatch "$P/0008-hi556-of.patch"

step "3) 确保 q6v5_pas auto_boot=true（SLPI 开机不抢先启动）"
Q6="$KSRC/drivers/remoteproc/qcom_q6v5_pas.c"
if grep -q 'sdm845_slpi_resource_init = {' "$Q6"; then
    if grep -A12 'sdm845_slpi_resource_init = {' "$Q6" | grep -q '\.auto_boot = false'; then
        run cp -a "$Q6" "$BK/qcom_q6v5_pas.c"
        run_sh "sed -i '/sdm845_slpi_resource_init = {/,/};/ s/\.auto_boot = false/.auto_boot = true/' '$Q6'"
        log "已恢复 auto_boot=true"
    else
        log "auto_boot=true 已正确"
    fi
else
    warn "未找到 sdm845_slpi_resource_init（内核版本不同？请人工确认 auto_boot）"
fi

step "4) 配置"
if [ -f "$KSRC/.config" ]; then run cp -a "$KSRC/.config" "$BK/config.pre-elish"; fi
run cp -a "$TAGK/config-used-6.18.44" "$KSRC/.config"
run make -C "$KSRC" ARCH=arm64 olddefconfig
if [ "$DRY_RUN" = 0 ]; then
    for c in CONFIG_TEE=y CONFIG_TEE_QSEECOM=y CONFIG_QCOM_QSEECOM=y CONFIG_QCOM_MDT_LOADER=y \
             CONFIG_QCOM_FASTRPC=m CONFIG_DEBUG_INFO_BTF=y CONFIG_MODULE_ALLOW_BTF_MISMATCH=y; do
        grep -q "^$c" "$KSRC/.config" || warn "配置缺少: $c"
    done
    grep -q '^CONFIG_LOCALVERSION="-current-sm8250"' "$KSRC/.config" || warn "LOCALVERSION 不是 -current-sm8250"
fi

if [ "$SKIP_BUILD" = 0 ]; then
    step "5) 编译 Image + modules（-j$JOBS）"
    run make -C "$KSRC" ARCH=arm64 -j"$JOBS" Image
    run make -C "$KSRC" ARCH=arm64 -j"$JOBS" modules
else
    log "跳过编译（--skip-build）"
fi

step "6) 安装模块"
run cp -a "$KSRC/drivers/misc/fastrpc.ko" "/lib/modules/$KREL_RUN/kernel/drivers/misc/fastrpc.ko"
if [ -d "$TAGK/goodix_fp_spi" ]; then
    run make -C "$KSRC" M="$TAGK/goodix_fp_spi" ARCH=arm64 modules
    run install -D -m 0644 "$TAGK/goodix_fp_spi/goodix_fp_spi.ko" "/lib/modules/$KREL_RUN/extra/goodix_fp_spi.ko"
    run_sh "printf 'goodix_fp_spi\n' > /etc/modules-load.d/goodix-fp.conf"
fi
if [ -d "$PKG_ROOT/gnss/kernel" ]; then
    run make -C "$KSRC" M="$PKG_ROOT/gnss/kernel" ARCH=arm64 modules
    run install -D -m 0644 "$PKG_ROOT/gnss/kernel/airoha_gps.ko" "/lib/modules/$KREL_RUN/extra/airoha_gps.ko"
    run_sh "printf 'airoha_gps\n' > /etc/modules-load.d/airoha-gps.conf"
fi
run depmod -a

step "7) 联合 DTB（模型级，包内已验证；不从 DTS 重建）"
DTBDIR="/usr/lib/linux-image-$KREL_RUN/qcom"
run install -D -m 0644 "$TAGK/dtb/sm8250-xiaomi-elish-csot.dtb" "$DTBDIR/sm8250-xiaomi-elish-csot.dtb"
run install -d "/boot/dtb-$KREL_RUN/qcom"
run install -m 0644 "$TAGK/dtb/sm8250-xiaomi-elish-csot.dtb" "/boot/dtb-$KREL_RUN/qcom/sm8250-xiaomi-elish-csot.dtb"
run bash "$TAGK/dtb/verify-combined-dtb.sh" "$DTBDIR/sm8250-xiaomi-elish-csot.dtb"

step "8) initramfs 修复 hook + 重建 initramfs"
run bash "$TAGK/initramfs/install-hook.sh"

step "9) 重建 boot_b（不碰 boot_a）"
if [ "$WRITE_BOOT" = 1 ]; then
    confirm "即将重建并写入 boot_b（已备份到 $BK/boot_b.img）。继续？" || die "用户取消（未写 boot_b）"
    run bash "$TAGK/update-boot-image.sh"
else
    log "未加 --write-boot：跳过写盘。需要时手动执行："
    echo "    sudo $TAGK/update-boot-image.sh"
fi

step "完成"
cat <<EOF
已安装模块/DTB/initramfs${WRITE_BOOT:+，boot_b 已重建并校验}。
下一步：
  sudo reboot
  重启后验证：
    sudo $PKG_ROOT/install/check-all.sh
    dmesg | grep -E 'SDSP assign|qseecom|Direct firmware load.*slpi'
    ls -l /dev/fastrpc-sdsp /dev/tee0 /dev/goodix_fp /dev/airoha_gps
回滚：boot_b 备份在 $BK/boot_b.img；模块/DTB 备份也在该目录。
EOF
