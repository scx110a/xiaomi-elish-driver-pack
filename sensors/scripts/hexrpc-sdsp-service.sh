#!/bin/bash
# hexrpc-sdsp-service.sh — SDSP FastRPC 文件服务监听器（2026-11-12 加速版）
#
# 只读母本 /usr/share/qcom-ssc-elish -> /run/sensors-rootsvc（hexagonrpcd 只写副本）
#
# 2026-11-12 加速改造：优先使用带 sns_reg_version 映射的 hexagonrpcd 构建
# （/opt/hexrpc-032b/bin/hexagonrpcd.regver-v1）。原因：hexrpc 0.3.2 的虚拟 registry
# 目录原来只暴露 registry/ 子目录，PD 读 .../registry/registry/../sns_reg_version
# 永远 ENOENT，于是每次 attach 都重跑 registry 生成（1570 次 temp.json 写失败 +
# 固件退避重试，实测约 20-25s）。补丁让该路径可读（version=4），PD 应跳过生成。
# 若补丁二进制缺失，自动回退原二进制（功能与 2026-11-11 版一致）。
M=/usr/share/qcom-ssc-elish
ROOT=/run/sensors-rootsvc
export LD_LIBRARY_PATH=/opt/hexrpc-032b/lib/aarch64-linux-gnu
export HEXAGONRPC_WARM_ATTACH=1
RPCD=/opt/hexrpc-032b/bin/hexagonrpcd
if [ -x /opt/hexrpc-032b/bin/hexagonrpcd.regver-v1 ]; then
  RPCD=/opt/hexrpc-032b/bin/hexagonrpcd.regver-v1
fi
# 2026-11-12 M2：可写 sensor registry（社区 0005/0006/0007 补丁）；缺失时回退 regver-v1
if [ -x /opt/hexrpc-032b/bin/hexagonrpcd.writable-v1 ]; then
  RPCD=/opt/hexrpc-032b/bin/hexagonrpcd.writable-v1
fi
if [ -e /etc/disable-sensors ]; then echo disabled; sleep infinity; fi
while true; do
  rm -rf "$ROOT"; mkdir -p "$ROOT"
  cp -a "$M/sensors" "$M/socinfo" "$ROOT"/ 2>/dev/null
  [ -d "$M/acdb" ] && cp -a "$M/acdb" "$ROOT"/ 2>/dev/null
  chmod -R 777 "$ROOT" 2>/dev/null
  for i in $(seq 900); do [ -e /dev/fastrpc-sdsp ] && break; sleep 0.001; done
  [ -e /dev/fastrpc-sdsp ] || { sleep 0.002; continue; }
  echo "=== attach $(date +%H:%M:%S) ($(basename "$RPCD")) ==="
  stdbuf -oL -eL "$RPCD" -f /dev/fastrpc-sdsp -d sdsp -s -R "$ROOT"
  echo "=== exited rc=$? $(date +%H:%M:%S) ==="
  sleep 0.002
done
