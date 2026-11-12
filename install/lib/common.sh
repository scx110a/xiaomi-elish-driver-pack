#!/bin/bash
# install/lib/common.sh — 合并包安装脚本的共享函数（由 turnkey/extract-all/build-userspace/kernel 脚本 source）
set -u

PKG_ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
BUILD_DIR=${ELISH_BUILD_DIR:-/var/tmp/elish-build}
EXTRACT_DIR=${ELISH_EXTRACT_DIR:-$BUILD_DIR/extracted}
DRY_RUN=${DRY_RUN:-0}
ASSUME_YES=${ASSUME_YES:-0}

log()  { printf '\033[1;34m[%s]\033[0m %s\n' "${TAG:-elish}" "$*"; }
warn() { printf '\033[1;33m[WARN]\033[0m %s\n' "$*" >&2; }
die()  { printf '\033[1;31m[FAIL]\033[0m %s\n' "$*" >&2; exit 1; }
step() { printf '\n\033[1;32m== %s ==\033[0m\n' "$*"; }
have() { command -v "$1" >/dev/null 2>&1; }

run()    { if [ "$DRY_RUN" = 1 ]; then printf '    DRY: %s\n' "$*"; else "$@"; fi; }
run_sh() { if [ "$DRY_RUN" = 1 ]; then printf '    DRY: %s\n' "$*"; else bash -c "$*"; fi; }
need_root() { [ "$(id -u)" -eq 0 ] || [ "$DRY_RUN" = 1 ] || die "需要 root：请用 sudo 运行本脚本"; }
need_cmd()  { have "$1" || die "缺少命令: $1（先运行依赖安装：sudo ./install.sh --deps）"; }

confirm() {
    [ "$ASSUME_YES" = 1 ] && return 0
    [ "$DRY_RUN" = 1 ] && return 0
    local a
    read -r -p "$* [y/N] " a || true
    case "${a:-}" in y|Y|yes|YES) return 0 ;; *) return 1 ;; esac
}

# detect_path <候选1> <候选2> ... → 打印第一个存在的路径
detect_path() {
    local f
    for f in "$@"; do
        [ -n "$f" ] && [ -e "$f" ] && { printf '%s\n' "$f"; return 0; }
    done
    return 1
}

mkdir_build() { run mkdir -p "$BUILD_DIR" "$EXTRACT_DIR"; }

# apply_patch <repo-dir> <patch-file> —— 幂等应用（已应用则跳过）
apply_patch() {
    local dir=$1 p=$2
    if [ ! -f "$p" ]; then die "补丁不存在: $p"; fi
    if [ "$DRY_RUN" = 1 ]; then log "DRY: 应用补丁 $(basename "$p")"; return 0; fi
    if ( cd "$dir" && git apply --reverse --check "$p" ) >/dev/null 2>&1; then
        log "补丁已应用，跳过: $(basename "$p")"
    elif ( cd "$dir" && git apply --check "$p" ) >/dev/null 2>&1; then
        log "应用补丁: $(basename "$p")"
        ( cd "$dir" && git apply "$p" ) || die "补丁应用失败: $p"
    else
        die "补丁与当前源码不匹配: $p（源码版本/分支不对？）"
    fi
}

# git_checkout <url> <dir> <ref> —— 不存在则 clone，存在则 fetch+checkout
git_checkout() {
    local url=$1 dir=$2 ref=$3
    if [ ! -d "$dir/.git" ]; then
        log "克隆 $url -> $dir"
        run git clone "$url" "$dir" || die "git clone 失败: $url（网络？）"
    else
        run git -C "$dir" fetch --tags --force origin || warn "git fetch 失败，尝试用本地缓存继续"
    fi
    run git -C "$dir" checkout -f "$ref" || die "git checkout $ref 失败（$dir）"
    run git -C "$dir" clean -fdx -e build >/dev/null 2>&1 || true
}
