# 指纹子包注意事项 / 已知问题 / 恢复

> 与传感器子包共用的坑（固件符号链接、boot_b、内核升级、suspend）见合并包根目录 `NOTES.md`。
> 这里只列指纹特有内容。

## 0. 铁律

1. 改内核/启动镜像前备份 Linux 启动槽（本机 `boot_b`，按 armbianEnv 的 `abl_boot_partition_label`）、`/boot/armbian-kernel-csot.img`、DTB；回退用 `dd` 写回同一槽位（只写 Linux 侧）。
2. **不要升级 edge 内核**（ABL 脚本通配符会生成坏镜像）；不要关 `CONFIG_DEBUG_INFO_BTF`；不要改
   `CONFIG_LOCALVERSION`。
3. PAM 改动前确保还有 root/密码登录通道（保持一个 root TTY 或用密码 sudo），不要只留指纹一条路。
4. 不要重新启用 NOPASSWD sudo（本包不含 `020-nopasswd*`，也不建议）。
5. 不要用 `fingerpr` TA；必须是 `gfenu`。

## 1. 内核 ABI 三件套（最危险）

每次重编内核后必须检查（详见 `../kernel/README.md`）：

| 检查 | 期望 |
|---|---|
| `strings vmlinux \| grep mod_unload` | `6.18.44-current-sm8250 SMP mod_unload aarch64` |
| `pahole -C module vmlinux \| grep 'size:'` | `size: 1280` |
| `CONFIG_DEBUG_INFO_BTF` / `CONFIG_MODULE_ALLOW_BTF_MISMATCH` | 都是 `=y` |

不一致 → 所有外部模块拒绝加载 → 黑屏只剩背光。BTF 编译需要 ≥4GB swap。

## 2. CMA 连续内存分配失败（TA 加载失败头号原因）

- 加载一个 TA 需要约 9MB **物理连续**内存（`qcom_tzmem_pool_new()` →
  `dma_alloc_coherent()`，本机要 2244 pages），来自 `cma=256M` 的 CMA 区。
- 这个 CMA 区平时也当普通可移动内存用（page cache/匿名页），所以 `CmaFree`
  经常只有几 MB～几十 MB；`cma_alloc` 必须先把占用该区的页**迁移**走才能拿到
  连续块，内存压力大时迁移失败 → `-EBUSY`。
- 症状：loader 日志 `event=loader_error ... errno=12 Cannot allocate memory`，
  dmesg：`cma: __cma_alloc: reserved: alloc failed, req-size: 2244 pages, ret: -16`
  （cma bitmap 仍显示大部分 free，属误导）；随后 dma-direct 回退 buddy
  order-12 分配，但 `CONFIG_ARCH_FORCE_MAX_ZONEORDER=10` → 只会 WARN 并失败。
- **这不是 TA 堆泄漏**，也不是永久性的：内存缓过来后同一分配会成功（实测用
  `/dev/dma_heap/reserved` 分配 9MB 正常）。每次重载 loader 都要重新做这次
  9MB 分配，所以重载越频繁越容易踩中。
- 现在的处理（2026-11-13）：
  - `qsee-app-loader@.service` 的 `ExecStart` 被 drop-in
    `60-retry-reclaim.conf` 换成 `/usr/local/bin/qsee-app-loader-retry`：失败时
    自动 `drop_caches` + `compact_memory` 后**退避重试**，不再要求重启系统；
    `NotifyAccess=all` + `TimeoutStartSec=0` 支持这种长时间重试。
  - `fp-ta-watchdog` 对 CMA 失败只记日志（重试由 wrapper 负责），并加了每 60s
    检查 loader 是否 inactive 的安全网；`-22`/TA 拒绝仍走“重启 loader”。
  - 正确用法仍是：**加载一次、loader 常驻**，不要频繁手动 `--load`/重载。
- 恢复：优先等 wrapper 重试成功（通常几秒内，drop_caches 后 CMA 立即有迁移目标）；
  必要时手动 `systemctl restart qsee-app-loader@gfenu`；重启系统仍是最后兜底，
  但已经不是 CMA 失败的必需操作。

## 3. TA "拒服务"（1062 / `scm call failed -22`）

- 触发：短时间内并发/连续多个 verify（脚本连测）会把 TA 留在 armed 状态。
- 恢复（不重启）：`sudo systemctl restart qsee-app-loader@gfenu`；
  已部署 `fp-ta-watchdog` 自动做（依赖 libfprint 日志补丁）。
- `fp-ta-watchdog` 现在区分：`-22`/QSEE refusal → 重载 loader；只有真正的
  `Cannot allocate memory`/CMA 碎片才停止 loader 并提示重启。
- `qsee-app-loader@.service` 增加了 restart limit drop-in（3 次/5 分钟、
  `RestartSec=10s`），避免 TA 失败时每 2 秒无限重载。
- 日常：**不要并发跑指纹操作**，上一个结束后再发下一个。

## 4. TA / mdt_loader

- `gfenu.mdt` 是 ELF32（原生支持）；`fingerpr.mdt` 是 ELF64（本包 mdt_loader 已加 ELF64 支持）。
- Android 里两套 TA：`gfenu` 正确、`fingerpr` 错误（FPC/旧方案）；用错时命令返回 `0xFFFFFFFF`。
- `/lib/firmware/fingerpr.*` 是历史工作副本，本项目最终不需要。

## 5. libfprint 会被 apt 覆盖

- 自编译 libfprint 1.94.100（`wrobelda/libfprint` goodix-qsee + 日志补丁）会覆盖发行版 `libfprint-2-2`。
- `apt upgrade` 升级 `libfprint-2-2` 后指纹设备消失 → 重新 `ninja -C build install && sudo ldconfig`，
  然后 `systemctl restart fprintd`。
- fprintd 的 systemd 沙箱默认 DeviceAllow 不含指纹/TEE 设备 → 必须保留
  `/etc/systemd/system/fprintd.service.d/60-goodix-qsee.conf`（`/dev/goodix_fp` + `/dev/tee0..3`）。

## 6. sudo 指纹 / PAM 坑

- 本版 PAM 不认 `success=end`，会丢弃整行；必须 `success=done`。
- `pam_exec` 默认以**登录用户**身份运行，触发文件必须放 `/run/user/<uid>/`。
- PAM 上下文里不能直接弹窗；必须"hook 写文件 + 用户会话 watcher 弹窗"。
- zenity 默认解析 Pango markup，命令含 `<>&` 会坏 → 必须 `--no-markup`。
- 认证成功关闭弹窗靠 PAM **session** hook；`sudo -v` 不建 session → 靠 12s 超时关闭。
- 指纹与密码不能真正并行：指纹优先、8s 超时回退密码；等待期间输入的密码会缓冲。
- 从无终端环境（后台/服务）触发 sudo 不会弹密码提示，8s 不按指纹就失败，属预期。
- 回退：恢复 `/root/fp-pam-backup-*/sudo`；不要依赖本包附带备份（本包**不含** PAM 备份和 NOPASSWD 文件）。

## 7. DTB 与 `firmware-name`

- 联合 DTB 需要：指纹 `/fingerprint` 节点（`compatible="goodix,gf3626"`、`firmware-name="gfenu"`、
  GPIO 23/125、L10A 供电）+ 传感器 `qcom,vmids=<3 15 5 6>`。
- **不要用 dtc 反编译重编译改属性**：会把 `regulators-1/ldo10` 的 4 字节电压属性改坏。
  用内核 DTS 源码重编，或按 `../kernel/dtb/README.md` 的就地脚本（保持长度）。
- 升级内核会覆盖 DTB 并重建 boot_b（`zz-update-abl-kernel` 用包内 DTB）→ 指纹和传感器同时失效，
  必须重做联合 DTB（见 `../kernel/README.md`）。

## 8. 识别率

- `fprintd-verify` 的 `verify-no-match` 是**真识别失败**（160×36 侧边小面积，对位置/干湿/按压力度敏感）；
  `verify-retry-scan` 等是图像质量问题，不是驱动故障。
- 建议：录入/验证时手指干燥、位置一致、力度稳定；多录几根手指。

## 9. 其他

- `gjs` 不要卸（GNOME 通知依赖）；`apt autoremove` 会带走。
- `/etc/dpkg/dpkg.cfg.d/10-firmware-trim` 会让 `dpkg -V` 报大量 missing，属正常噪音。
- 指纹模板在 `/var/lib/fprint/...`（用户私有），**不要**拷贝到别的机器/进安装包。
- 升级影响（内核/固件/libfprint/PAM/fprintd）见合并包根目录 `docs/UPGRADE-IMPACT-ALL.md`
  与 `../sensors/docs/UPGRADE-IMPACT.md`。
