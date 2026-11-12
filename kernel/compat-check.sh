#!/bin/bash
# compat-check.sh — 只读检查本包内核补丁/整文件副本对新内核源码树的兼容性
#
# 用途：升级 Armbian/内核前，先看清"哪些补丁还能打、哪些冲突、哪些整文件覆盖会回退上游改动"。
#       不写任何系统文件；只在一个临时目录/fetch 缓存里做 patch --dry-run 与 diff。
#
# 用法：
#   ./kernel/compat-check.sh --src /path/to/pristine/kernel/source
#   ./kernel/compat-check.sh --fetch-ref linux-6.18.y --repo armbian/linux
#   ./kernel/compat-check.sh --fetch-ref v6.19 --repo torvalds/linux
#   ./kernel/compat-check.sh --src ... --report /tmp/compat.md --fail-on-copies
#   ./kernel/compat-check.sh -h
#
# 退出码：0=无补丁冲突（允许整文件差异/缺失文件警告）；1=有补丁冲突（或 --fail-on-copies 时整文件有差异）。
#
# 说明：
#   - --fetch-ref 只下载补丁目标文件 + 少数校验文件到临时目录（不是完整内核树），速度快；
#   - 0004（qcom_pm8150b_charger）的驱动来自 Armbian sm8250 补丁集，不在 arminan/linux 分支镜像里，
#     因此用 --fetch-ref 检查当前分支时该补丁会显示"目标缺失（SKIP）"，属预期；
#     完整补丁集树请用 --src 指向真实源码树。
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
PKG_ROOT=$(cd "$HERE/.." && pwd)
PATCH_DIR="$PKG_ROOT/kernel/patches"

REPO="armbian/linux"
REF=""
SRC=""
FETCH=0
REPORT=""
FAIL_ON_COPIES=0
FAIL_ON_MISSING=0
KEEP=0

usage() { awk 'NR>1 { if ($0 ~ /^#/) { sub(/^# ?/, ""); print } else exit }' "$0"; }

while [ $# -gt 0 ]; do
    case "$1" in
        --src) SRC=${2:-}; shift 2 ;;
        --fetch-ref) REF=${2:-}; FETCH=1; shift 2 ;;
        --repo) REPO=${2:-}; shift 2 ;;
        --report) REPORT=${2:-}; shift 2 ;;
        --fail-on-copies) FAIL_ON_COPIES=1; shift ;;
        --fail-on-missing) FAIL_ON_MISSING=1; shift ;;
        --keep) KEEP=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) echo "未知参数: $1（-h 查看用法）" >&2; exit 2 ;;
    esac
done

if [ -z "$SRC" ] && [ "$FETCH" = 0 ]; then
    usage; exit 2
fi
if [ -n "$REPORT" ]; then
    exec > >(tee "$REPORT") 2>&1
fi

C_CLEAN=0; C_FUZZ=0; C_CONFLICT=0; C_MISSING=0; C_ALREADY=0
COPY_WARN=0; MISS_FILES=0

say()  { echo "$*"; }
ok()   { echo "  OK    $*"; }
warn() { echo "  WARN  $*"; }
bad()  { echo "  FAIL  $*"; }

cleanup() { if [ "$FETCH" = 1 ] && [ "$KEEP" = 0 ] && [ -n "${TMPROOT:-}" ]; then rm -rf "$TMPROOT"; fi; }
trap cleanup EXIT

# ---------- 构建检查树 ----------
if [ "$FETCH" = 1 ]; then
    [ -n "$REF" ] || { echo "--fetch-ref 需要给出 ref（如 linux-6.18.y / v6.19）" >&2; exit 2; }
    TMPROOT=$(mktemp -d /tmp/elish-compat.XXXXXX)
    SRC="$TMPROOT/tree"
    mkdir -p "$SRC"
    say "== 拉取最小文件集：$REPO@$REF -> $SRC"
    fetch_one() {
        local path=$1
        local dir
        dir=$(dirname "$path")
        mkdir -p "$SRC/$dir"
        local i
        for i in 1 2 3; do
            if curl -fsSL --max-time 90 "https://raw.githubusercontent.com/$REPO/$REF/$path" -o "$SRC/$path" 2>/dev/null; then
                return 0
            fi
            sleep 1
        done
        rm -f "$SRC/$path"
        return 1
    }
    # 补丁目标的旧文件（新文件不需要拉取）
    while IFS= read -r f; do [ -n "$f" ] && fetch_one "$f" >/dev/null 2>&1; done < <(
        awk '/^--- /{old=$2} /^\+\+\+ /{new=$2; if (old != "/dev/null") print new; old=""}' "$PATCH_DIR"/*.patch \
        | sed 's#^b/##' | sort -u
    )
    # 整文件副本 / 关键符号 / DTS 校验用
    for f in \
        drivers/firmware/qcom/qcom_scm.c \
        drivers/soc/qcom/mdt_loader.c \
        drivers/remoteproc/qcom_q6v5_pas.c \
        drivers/tee/qseecom/core.c \
        arch/arm64/boot/dts/qcom/sm8250-xiaomi-elish-common.dtsi \
        arch/arm64/boot/dts/qcom/sm8250-xiaomi-elish-csot.dts; do
        fetch_one "$f" >/dev/null 2>&1 || true
    done
    say "  下载文件数: $(find "$SRC" -type f | wc -l)"
fi
[ -d "$SRC" ] || { echo "源码树不存在: $SRC" >&2; exit 2; }

say ""
say "== 1) 补丁 dry-run（$(ls "$PATCH_DIR"/*.patch 2>/dev/null | wc -l) 个补丁）"
for p in "$PATCH_DIR"/*.patch; do
    [ -e "$p" ] || continue
    name=$(basename "$p")
    req=$(awk '/^--- /{old=$2} /^\+\+\+ /{new=$2; if (old != "/dev/null") print new; old=""}' "$p" | sed 's#^b/##' | sort -u)
    missing=0
    for t in $req; do
        [ -e "$SRC/$t" ] || { missing=1; echo "  SKIP  $name: 目标缺失 $t"; }
    done
    if [ "$missing" = 1 ]; then
        C_MISSING=$((C_MISSING+1))
        continue
    fi
    out=$(patch -p1 --dry-run -N -d "$SRC" < "$p" 2>&1); rc=$?
    if [ "$rc" -eq 0 ]; then
        if echo "$out" | grep -q 'with fuzz'; then
            C_FUZZ=$((C_FUZZ+1)); warn "$name 可应用但有 fuzz"
            echo "$out" | grep -E 'Hunk|fuzz' | sed 's/^/          /'
        else
            C_CLEAN=$((C_CLEAN+1)); ok "$name"
        fi
    else
        if patch -p1 -R --dry-run -N -d "$SRC" < "$p" >/dev/null 2>&1; then
            C_ALREADY=$((C_ALREADY+1)); ok "$name（已应用，反向检查通过）"
        else
            C_CONFLICT=$((C_CONFLICT+1)); bad "$name 存在冲突"
            echo "$out" | grep -E 'Hunk|FAILED|can.t find' | sed 's/^/          /'
        fi
    fi
done

say ""
say "== 2) 整文件覆盖风险（升级时需人工 diff/rebase）"
copy_check() {
    local rel=$1
    local pkg_file=$2
    local src_file="$SRC/$rel"
    if [ ! -e "$src_file" ]; then
        warn "$rel 在新树中不存在（可能未合入/已改名）"
        MISS_FILES=$((MISS_FILES+1))
        return
    fi
    if cmp -s "$pkg_file" "$src_file"; then
        ok "$rel 与包内一致（无需处理）"
    else
        local changed
        changed=$(diff -u "$src_file" "$pkg_file" 2>/dev/null | grep -cE '^[+-][^+-]')
        warn "$rel 与包内不同（约 $changed 行差异；覆盖会丢失新树中的上游修复）"
        COPY_WARN=$((COPY_WARN+1))
    fi
}
copy_check "drivers/firmware/qcom/qcom_scm.c" "$PKG_ROOT/kernel/qcom_scm.c"
copy_check "drivers/soc/qcom/mdt_loader.c" "$PKG_ROOT/kernel/mdt_loader.c"
# 下列 out-of-tree 组件不覆盖源码树，只提示
say ""
say "== 3) out-of-tree / 关键符号检查"
grep -q 'sdm845_slpi_resource_init' "$SRC/drivers/remoteproc/qcom_q6v5_pas.c" 2>/dev/null \
    && ok "q6v5_pas: sdm845_slpi_resource_init 存在（auto_boot sed 可继续）" \
    || warn "q6v5_pas: 未找到 sdm845_slpi_resource_init（auto_boot 需人工确认）"
grep -q 'qcom_scm_qseecom' "$SRC/drivers/firmware/qcom/qcom_scm.c" 2>/dev/null \
    && ok "qcom_scm: 新树已含 qseecom 相关代码（可考虑不再整文件覆盖）" \
    || warn "qcom_scm: 新树不含 qseecom 代码（包内整文件覆盖仍是必需，风险最高项）"
[ -e "$SRC/drivers/tee/qseecom/core.c" ] \
    && ok "drivers/tee/qseecom 已存在（上游/补丁集已带）" \
    || warn "drivers/tee/qseecom 不在新树（由本包提供，需确认 TEE API 兼容）"
[ -e "$SRC/drivers/power/supply/qcom_pm8150b_charger.c" ] \
    && ok "qcom_pm8150b_charger.c 存在（补丁 0004 的目标文件）" \
    || warn "qcom_pm8150b_charger.c 不在该树（来自 Armbian sm8250 补丁集；用真实补丁集树复查 0004）"
for d in arch/arm64/boot/dts/qcom/sm8250-xiaomi-elish-common.dtsi \
         arch/arm64/boot/dts/qcom/sm8250-xiaomi-elish-csot.dts; do
    [ -e "$SRC/$d" ] && ok "DTS 存在: $d" || warn "DTS 缺失: $d（DTB 需另行生成/校验）"
done

say ""
say "== 汇总 =="
say "  补丁: clean=$C_CLEAN fuzz=$C_FUZZ already-applied=$C_ALREADY conflict=$C_CONFLICT target-missing=$C_MISSING"
say "  整文件覆盖差异: $COPY_WARN 项；关键文件缺失: $MISS_FILES 项"
if [ "$C_CONFLICT" -gt 0 ]; then
    say "  结论: FAIL —— 有补丁冲突，需要人工 rebase（见上）"
    exit 1
fi
if [ "$FAIL_ON_COPIES" = 1 ] && [ "$COPY_WARN" -gt 0 ]; then
    say "  结论: FAIL —— 整文件副本与新树不同（--fail-on-copies）"
    exit 1
fi
if [ "$FAIL_ON_MISSING" = 1 ] && { [ "$C_MISSING" -gt 0 ] || [ "$MISS_FILES" -gt 0 ]; }; then
    say "  结论: FAIL —— 有目标/关键文件缺失（--fail-on-missing）"
    exit 1
fi
say "  结论: PASS（补丁可应用；整文件/缺失项见上方警告，升级前需人工处理）"
if [ "$FETCH" = 1 ] && [ "$KEEP" = 1 ]; then
    say "  检查树保留在: $SRC"
fi
exit 0
