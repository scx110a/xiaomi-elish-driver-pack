#!/bin/bash
# 合并包一键安装入口（等价于 install/turnkey.sh，支持 --help / --dry-run）
exec "$(cd "$(dirname "$0")" && pwd)/install/turnkey.sh" "$@"
