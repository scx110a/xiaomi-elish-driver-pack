# 内核升级风险评估（用本包修补新的 Armbian 内核）

> 结论基于 2026-11-13 的实测数据：GitHub 提交 churn + 把包内补丁对
> `armbian/linux@linux-6.18.y` HEAD 与 mainline `v6.19` 做 `patch --dry-run`。
> 工具：`kernel/compat-check.sh`（只读，见文末）。

## 0. 结论速览

| 场景 | 风险 | 预计工作量 | 说明 |
|---|---|---|---|
| **6.18.x 点更新**（同一个 Armbian `current` 分支） | **低～中** | 1–3 小时（含硬件冒烟） | 补丁基本能打；重点人工检查"整文件覆盖"、DTB/initramfs、Armbian 补丁集 rebase |
| **6.19 / 7.x（edge 或 major bump）** | **高** | 1–3 天 | fastrpc/ov8856 等需 rebase；qcom_scm/mdt_loader/qseecom 需按新 API 适配；DTB 重做 |
| 用户态（hexagonrpcd/libssc/iio） | 中低 | 视 fastrpc ABI 变化 | 与内核解耦较好，但 fastrpc ioctl/ABI 变化需重测传感器 |
| 指纹 qseecom / out-of-tree 模块 | 高 | 可能需改写 | 依赖 TEE/qcom_scm 内部 API，跨版本最脆 |

**一句话**：同 6.18.x 跟点更新风险不高（需人工审 + 验证）；跨 minor 属于"移植项目"，
但比从头做简单得多（见 §3）。

## 1. 被本包改动的内核文件更新频率（churn）

统计口径：GitHub API 触达该文件的 commit 数（代理指标，非行数）。
- mainline：2024-01 至今（上限 100）
- Armbian `linux-6.18.y`：2025-06 至今（更接近实际会收到的点更新）

| 内核文件 | 包内方式 | mainline (2024+) | Armbian 6.18.y (2025-06+) |
|---|---|---|---|
| `drivers/misc/fastrpc.c` | 补丁 0001（55 行） | 71 | 22 |
| `drivers/remoteproc/qcom_q6v5_pas.c` | 脚本 sed `auto_boot` | 59 | 7 |
| `drivers/firmware/qcom/qcom_scm.c` | **整文件复制** | 100+ | 16 |
| `drivers/soc/qcom/mdt_loader.c` | **整文件复制** | 17 | 9 |
| `drivers/pinctrl/qcom/pinctrl-lpass-lpi.c` | 补丁 0002（23 行） | 12 | 5 |
| `drivers/media/i2c/ov8856.c` | 补丁 0005（29 行） | 13 | 6 |
| `drivers/media/i2c/ov13b10.c` | 补丁 0006（97 行） | 9 | 2 |
| `drivers/media/i2c/hi556.c` | 补丁 0008（29 行） | 19 | 5 |
| `qcom_pm8150b_charger.c`（0004） | 补丁 | 不在 mainline | 来自 Armbian sm8250 补丁集 |
| `qseecom/core.c`（1934 行） | **整文件 + out-of-tree** | 不在 mainline | 依赖 TEE/qcom_scm API |
| `goodix_fp_spi` / `airoha_gps` | **out-of-tree 模块** | — | 依赖内核内部 API |
| elish CSOT DTS | mainline 最后改动 2023-05 | 稳定 | Armbian 补丁集中有大量 elish DTS 补丁 |

> Armbian sm8250-6.18 补丁集：70 个文件，2024 年以来目录级改动约 10 次
> （最近 2026-11-04；2026-08-20 刚 rebase 过 fastrpc revert 补丁）。

## 2. 实测（本轮 dry-run）

| 补丁 | Armbian `linux-6.18.y` HEAD | mainline `v6.19` |
|---|---|---|
| 0001 fastrpc | ✅ 干净（offset 1/5 行） | ❌ Hunk 2 FAILED（2 个 hunk） |
| 0002 pinctrl | ✅ | ✅ |
| 0003 mdt Kconfig | ✅ | ✅ |
| 0004 smb5 charger | 分支镜像无该文件（来自补丁集，需真实树复查） | 同左 |
| 0005 ov8856 | ✅ | ❌ Hunk 3 FAILED（3 个 hunk） |
| 0006 ov13b10 | ✅ | ✅（offset 1 行） |
| 0007 cn3927v | ✅ | ✅（offset 10 行） |
| 0008 hi556 | ✅ | ✅ |

另外：`sdm845_slpi_resource_init` 在 6.18.y 与 v6.19 中都存在，
所以 `auto_boot` 的 sed 逻辑跨版本风险低。

## 3. 为什么"移植"比从头做简单得多

本包已经把**最难、最耗时、最依赖硬件实验**的部分做完了：

- 硬件事实与调试结论（传感器 SLPI/PD、指纹 TA/看门狗、GNSS PAIR、相机 RAW10/供电/翻转/VCM；
  见 `docs/HARDWARE-STATUS.md`、`camera/elish-camera/docs/`）；
- 联合 DTB 的节点/属性参考与校验器（`kernel/dtb/README.md` + `verify-combined-dtb.sh`）；
- 内核集成点与补丁（fastrpc VMID、QSEECOM、mdt_loader、auto_boot、LPASS pinctrl、SMB5、相机 sensor）；
- 全部用户态组件与系统集成（hexagonrpcd/libssc/iio、qsee-supplicant/libfprint、GNSS、相机管线、
  systemd/udev/PAM/自启/唤醒恢复、安装器与自检）。

移植时通常只需要：**把补丁 rebase 到新源码树** + **整文件副本按新树重新适配** + **DTB 重新校验/生成**
+ **跑一遍硬件验证**。这些是"有明确参照物"的适配工作，而不是重新逆向/试错。
按当前数据估算：6.18.x 点更新约 1–3 小时；6.19 约 1–3 天；从零做则通常是数周到数月。

## 4. 升级操作清单

1. 继续 hold `linux-image/dtb/headers`、`armbian-firmware-full`，不要无脑 `apt upgrade`；
2. 升级前备份**目标 Linux boot 槽**（`update-boot-image.sh` 会自动检测并备份）；
3. 先跑兼容性检查（只读）：
   ```bash
   ./kernel/compat-check.sh --src /path/to/pristine/kernel/source
   # 或轻量拉取：
   ./kernel/compat-check.sh --fetch-ref linux-6.18.y --repo armbian/linux
   ```
4. 看 `apply_kpatch` 的 "补丁不匹配，跳过" 警告——不要忽略；必要时手工 rebase；
5. **整文件覆盖**（`qcom_scm.c` / `mdt_loader.c`）不要直接覆盖：
   先 `diff` 新树版本，把 QSEECOM 相关改动重新应用到新版本上；
6. DTB：`kernel/dtb/verify-combined-dtb.sh` 校验；必要时按 `kernel/dtb/README.md` 从新 DTS 重做；
7. 跑 `install/check-all.sh` + 传感器/指纹/GNSS/相机四项硬件冒烟；
8. 出问题用升级前备份回滚 boot 槽。

## 5. 工具：`kernel/compat-check.sh`

```bash
./kernel/compat-check.sh --src <新内核源码树>          # 全量树（推荐）
./kernel/compat-check.sh --fetch-ref linux-6.18.y --repo armbian/linux  # 轻量拉取
./kernel/compat-check.sh --fetch-ref v6.19 --repo torvalds/linux        # 预演 minor bump
```

输出：每个补丁 clean/fuzz/already-applied/conflict；整文件副本差异行数；关键符号/DTS 检查。
退出码：0=无补丁冲突（整文件差异/缺失为警告）；1=有冲突（或加 `--fail-on-copies`）。
