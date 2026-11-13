# 包内容清单（MANIFEST）

本归档只包含"安装传感器支持所需的代码、补丁、系统集成文件与文档"。
**不含**：固件、厂商数据（sensors/registry/acdb/socinfo）、编译产物（.ko/可执行文件/库）、日志、实验工具。

## 文件树

```
README.md                 总览、快速开始、红线
INSTALL.md                完整安装步骤（依赖/提取/编译/安装/验证/升级/回滚）
NOTES.md                  注意事项、已知问题、不变量、升级速查
PATCHES.md                所有补丁与配置修改清单（含 2 处内核 hunk）
VERSIONS.md               版本与校验值（BOM）
MANIFEST.md               本文件
SHA256SUMS                所有文件校验

userspace/
  README.md               用户态组件总览（编译顺序）
  hexrpc/README.md        hexagonrpcd 0.3.2 + warm-attach + registry-version + M2 可写 registry 说明
    patches/0001-hexrpc-0.3.2-warm-attach.patch
    patches/0002-hexrpc-0.3.2-registry-version.patch
    patches/0003-hexagonrpcd-add-writable-hexagonfs.patch（M2）
    patches/0004-hexagonrpcd-support-writable-sensor-registry.patch（M2）
    patches/0005-hexagonrpcd-support-large-and-extended-callbacks.patch（M2）
  iio-sensor-proxy/README.md  三处修复说明
    patches/0001-iio-sensor-proxy-hotplug-stayalive.patch
  libssc/README.md        libssc 0.4.4 编译说明

systemd/
  README.md               文件落位说明
  slpi-sensors.service / slpi-fw-guard.service
  sensor-boot-report.service / sensor-boot-report.timer
  iio-dropin/after-ssc.conf
  udev/{80-iio-sensor-proxy,81-libssc-xiaomi-elish,99-ssc-accel}.rules
  system-sleep/{40-cancel-scheduled-shutdown,60-sensors-suspend}.sh
  sensor-tuning.conf
  dbus/{net.hadess.SensorProxy.service, .conf, .policy}

scripts/
  README.md
  slpi-sensors-start.sh   启动主逻辑（符号链接不变量/监听器先行/健康检查/warm restart 兜底）
  slpi-sensors-stop.sh    ExecStop：只删 /run 副本，保持悬空链接
  hexrpc-sdsp-service.sh  warm 监听器循环（HEXAGONRPC_WARM_ATTACH=1，优先 writable-v1 → regver-v1 → 原厂）
  sensor-timing-diag.sh   一次性启动时序诊断（2026-11-12 加速验证，/var/log/sensor-timing.log）
  slpi-fw-guard.sh        开机早期固件路径守卫
  sensor-status.sh        只读诊断（AUTO_BOOT/PDM/attach 计数/iio/数值）
  sensor-boot-report.sh   开机报告生成

install/
  FILES-MAP.md            归档路径 → 系统路径 一一对应
  install-system-files.sh 安装脚本/unit/udev/hook/D-Bus/调参
  install-data.sh         组装数据母本 + 固件路径不变量
  check-upgrade.sh        软件/内核升级后的风险检查（只读）
  verify.sh               安装后/开机后自检
  uninstall.sh            卸载系统集成（可选删数据）

extract/
  README.md               数据/固件来源总表与提取流程
  extract-from-armbian-firmware.sh   registry/config/socinfo（armbian-firmware-full）
  extract-from-android-vendor-img.sh proto/hals 等 + 可选 acdb（Android vendor 镜像）
  extract-pmos-slpi-mbn.sh           单文件 slpi.mbn（pMOS APK）

docs/
  TROUBLESHOOTING.md      失败签名、恢复命令
  UPGRADE-IMPACT.md       ★ 升级影响评估（哪些升级会破坏功能、哪些必须保持）
  IIO-FIX.md              iio-sensor-proxy 修复原理与验证
  HANDOFF-SENSORS.md      传感器项目历史交接（第 17–45 节，已去除指纹项目部分）
```

## 明确不包含 / 需现场获取

| 类别 | 原因 | 获取方式 |
|---|---|---|
| SLPI 固件 `slpi.mbn` | 厂商固件二进制；不适合再分发 | `extract/extract-pmos-slpi-mbn.sh`（pMOS APK） |
| sensors 配置/registry/socinfo | 设备型号相关，来自发行包/vendor 镜像 | `extract/extract-from-armbian-firmware.sh` |
| proto/hals/calibratedCheck 等 | 来自设备 vendor 分区 | `extract/extract-from-android-vendor-img.sh` |
| acdb（可选） | 来自设备 vendor 分区 | 同上 |
| fastrpc.ko | 与内核 vermagic 强绑定 | 用 kernel 补丁自行编译 |
| hexagonrpcd / libhexagonrpc | 编译产物 | 按 userspace/hexrpc 编译 |
| iio-sensor-proxy / libssc | 编译产物 | 按 userspace 编译 |
| 日志/实验工具（sensor-test、matrix、调参编排） | 单机实验产物 | 不入库；历史记录见 docs/HANDOFF-SENSORS.md 与根 `docs/HISTORY.md` |

## 统计（打包时）

- 文件数：见 `find . -type f | wc -l`；体积约 200KB 量级（不含 SHA256SUMS）。
- 无任何序列号/密钥/MAC/IMEI 内容（打包前已全量扫描）；无二进制可执行文件。
