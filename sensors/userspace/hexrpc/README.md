# hexagonrpcd（hexagonrpc 0.3.2 + warm-attach 补丁）

## 为什么必须 0.3.x

- 0.3.x 使用 `-s`（INIT_ATTACH_SNS）与 `-R <hexagonfs root>` 的行为与本项目内核 fastrpc 匹配；
- 0.5.0 在 attach 时返回 `Unsupported handle: 0xffffffff`，无法使用。

## 补丁（必须）

`patches/0001-hexrpc-0.3.2-warm-attach.patch` 对应 `hexagonrpcd/rpcd.c`，内容：

1. `HEXAGONRPC_WARM_ATTACH=1` 时，open `/dev/fastrpc-sdsp` 采用**预热式重试**（20µs 级别），
   保证在设备出现的极短窗口内立即打开；
2. `FASTRPC_IOCTL_INIT_ATTACH_SNS` 遇到 `EPERM` 时毫秒级重试（50µs × 2000）。

原因：attach 必须发生在设备出现后的约 5–20ms 窗口内（矩阵实验：延迟 0ms 成功，≥50ms 全部失败），
太早 attach 返回 EPERM，太晚 PD 走错误初始化路径并崩溃。补丁把这个窗口稳定命中。

## 编译

依赖（Debian/Ubuntu）：`build-essential meson ninja-build pkg-config`（该版本无其它外部库依赖）。

```bash
git clone https://github.com/linux-msm/hexagonrpc
cd hexagonrpc
git checkout v0.3.2
git apply /path/to/patches/0001-hexrpc-0.3.2-warm-attach.patch

meson setup build --prefix=/opt/hexrpc-032b
ninja -C build
sudo ninja -C build install
```

安装后布局（本项目脚本依赖这个路径）：

```
/opt/hexrpc-032b/bin/hexagonrpcd
/opt/hexrpc-032b/lib/aarch64-linux-gnu/libhexagonrpc.so
/opt/hexrpc-032b/libexec/hexagonrpc/chrecd
```

`bin/hexagonrpcd` 依赖 `libhexagonrpc.so`，运行时通过
`LD_LIBRARY_PATH=/opt/hexrpc-032b/lib/aarch64-linux-gnu` 提供（启动脚本已设置）。

不要把它装到 `/usr/local` 覆盖系统里的其它 hexagonrpc 版本；本项目固定使用 `/opt/hexrpc-032b`。

## 运行方式（由 `slpi-sensors-start.sh` 调用）

```
LD_LIBRARY_PATH=/opt/hexrpc-032b/lib/aarch64-linux-gnu \
HEXAGONRPC_WARM_ATTACH=1 \
/opt/hexrpc-032b/bin/hexagonrpcd -f /dev/fastrpc-sdsp -d sdsp -s -R /run/sensors-rootsvc
```

验证 attach 成功：`/var/log/hexrpc-boot.log` 中 attach 段应出现
`stat(/vendor/etc/sensors/sns_reg_config)`、`sensors/registry` 等大量文件访问；
失败时通常只有一条 `/sys/devices/soc0/hw_platform` 就停止。


## 2026-11-12 加速补丁 0002（sns_reg_version 映射）— 可选增强

`patches/0002-hexrpc-0.3.2-registry-version.patch`（配合 0001 一起应用）：

hexrpc 0.3.2 的虚拟 persist 目录只暴露 `.../persist/sensors/registry/registry`
（映射到 hexagonfs root 的 `sensors/registry/`），PD 读取
`.../registry/registry/../sns_reg_version` 时永远 ENOENT；固件于是每次 attach 都
重跑 registry 初始化（每次约 1570 次 `temp.json` 写失败 + 退避重试，实测约 20-25s）。

0002 在同一个虚拟目录里补了 `sns_reg_version` 文件映射（母本
`/usr/share/qcom-ssc-elish/sensors/sns_reg_version`，内容 `version=4`），
让 PD 能读到版本书签。实测：version 读失败从 20 次/session 降为 0。
（注意：PD 仍会尝试写回 `temp.json`（hexrpc 0.3.2 不支持写），因此该补丁只降低部分
开销；彻底解决需要可写 persist——已于 2026-11-13 由下方 M2 补丁实现并验证。）

编译（与 0001 相同的 0.3.2 源码）：

```bash
git clone https://github.com/linux-msm/hexagonrpc && cd hexagonrpc
git checkout v0.3.2
git apply /path/to/patches/0001-hexrpc-0.3.2-warm-attach.patch
git apply /path/to/patches/0002-hexrpc-0.3.2-registry-version.patch
meson setup build --prefix=/opt/hexrpc-032b && ninja -C build && sudo ninja -C build install
```

**本包不附带预编译二进制**（遵循"只含代码/补丁/配置/脚本/文档"的打包约定）。
按上面命令自行编译；本项目部署时命名为 `/opt/hexrpc-032b/bin/hexagonrpcd.regver-v1`
（sha256 参考 `c52ecf31…`；hexrpc-sdsp-service.sh 的回退链会用它，缺失则用原厂 `hexagonrpcd`）。


## 2026-11-13 M2：可写 sensor registry（默认部署）

社区补丁（Armada OS / SteamOS-ARM 打包记录，原始作者 suckbluefrog）适配本机 v0.3.2：

- `patches/0003-hexagonrpcd-add-writable-hexagonfs.patch`：HexagonFS 增加 flag-aware open、write、truncate；
- `patches/0004-hexagonrpcd-support-writable-sensor-registry.patch`：apps_std 增加
  fwrite/fsync/fremove/ftruncate/frename，把 `/mnt/vendor/persist/sensors` 映射为可写树
  （只允许写 sensor persist 树），并修正 fflush 签名；`rpcd.c`/`rpcd_builder.c` 两处 hunk
  已适配本机 v0.3.2 base（本机 `fastrpc_apps_std_init` 在 `start_reverse_tunnel()` 内，补丁原始 base 稍旧）；
- `patches/0005-hexagonrpcd-support-large-and-extended-callbacks.patch`：listener input cache
  256B→1MB，并解码 QAIC method 31 转义（fwrite/rename 载荷与大 method 号需要）。

编译（**不要**再叠 0002；可写树已替代其只读版本映射）：

```bash
git clone https://github.com/linux-msm/hexagonrpc && cd hexagonrpc
git checkout v0.3.2
git apply .../patches/0001-hexrpc-0.3.2-warm-attach.patch
git apply .../patches/0003-hexagonrpcd-add-writable-hexagonfs.patch
git apply .../patches/0004-hexagonrpcd-support-writable-sensor-registry.patch
git apply .../patches/0005-hexagonrpcd-support-large-and-extended-callbacks.patch
meson setup build --prefix=/opt/hexrpc-032b && ninja -C build
sudo ninja -C build install
sudo install -m 0755 build/hexagonrpcd/hexagonrpcd /opt/hexrpc-032b/bin/hexagonrpcd.writable-v1
```

**本包不附带预编译二进制**。按上面命令编译后安装为
`/opt/hexrpc-032b/bin/hexagonrpcd.writable-v1`（sha256 参考 `68809903…`）；
`hexrpc-sdsp-service.sh` 优先使用它，缺失时回退 `hexagonrpcd.regver-v1` → `hexagonrpcd`。
回滚：删除该文件 + 重启。

实测（boot 2026-11-13 00:01）：PD registry 初始化 35s→8.2s，开机传感器 ~22s（改造前 79s），
`Tried to open ... for writing` 1570→0，517 次 fwrite 全成功，iio NRestarts=0。
可写落盘只到 `/run/sensors-rootsvc`（tmpfs，重启从母本重来），不污染 `/usr/share/qcom-ssc-elish`。
