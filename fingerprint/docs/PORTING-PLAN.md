# 指纹移植实施计划（历史）

> 历史计划稿（2026-11-10）；当前状态见 `README.md`/`INSTALL.md`。最终 TA 结论：必须使用 `gfenu`（`fingerpr` 为错误/旧方案）。

目标：在 Armbian (SM8250/elish) 上启用 Goodix 指纹（登录/sudo/锁屏）。
硬约束：**不对 Android 分区做任何写操作**；**不破坏现有 Linux 系统**（每步可回退）。

---

## 参考实现组件

| 组件 | 仓库/分支 | 作用 |
|---|---|---|
| 内核驱动 · 传感器 | `wrobelda/linux` 分支 `goodix-fp-spi` | 电源/复位/中断控制（module） |
| 内核驱动 · TEE | `wrobelda/linux` 分支 `goodix-qsee`（或 `qcom-qseecom-tee`） | QSEECOM 后端接入 Linux TEE 子系统 |
| 用户态 supplicant | `wrobelda/qsee-supplicant` | TA 加载器 + listener 服务 |
| libfprint 驱动 | `wrobelda/libfprint` | fprintd 用的指纹后端 |
| 测试客户端 | 本仓库 `harness/gfharness.c` | 命令行验证 TA 通信 |

## 只读素材（已保存）

| 素材 | 位置 |
|---|---|
| TA 镜像 | `ta-image/fingerpr.mdt` + `b00~b07` |
| DT 参数 | `elish-board.dts`、`goodix-dt/` |
| 抓包日志 | `capture/command.log`、`listener.log` |
| 驱动源码 | `goodix-src/`（小米开源） |

---

## 阶段 0：安全基线与备份

- [ ] 记录当前内核/模块/启动镜像状态
- [ ] 备份 `/boot/armbian-kernel-csot.img`
- [ ] 备份 `/lib/modules/<ver>/` 关键模块清单
- [ ] 确认 Android 分区只读挂载（仅在需要时临时挂载，用完卸载）

## 阶段 1：TA 镜像部署（已完成 2026-11-09）

- [x] `cp ta-image/gfenu.* /lib/firmware/`（9 个文件，约 4.8 MB；最终必须用 `gfenu`，`fingerpr` 为错误/旧方案）
- [x] 验证文件权限和路径
- [x] 回退方法：`rm /lib/firmware/gfenu.*`

## 阶段 2 前置：内核重建方案（已评估）

参考实现的 TEE 驱动基于 Linux 7.1，且改动涉及**内置**部件（`tee_core.c`、`qcom_scm.c`），
无法作为模块加载，**必须重编内核**。

重建方案（忠实复现当前内核 + 追加 TEE 支持）：

1. 源码：mainline 6.18.44（`/var/tmp/kbuild-src` 已解压 1.7 GB）
2. Armbian 补丁：36 个（`kernel-build/armbian-patches/`，已验证**全部干净应用，0 reject**）
3. 内核配置：`kernel-build/config-6.18.44`（取自当前内核）
4. 追加补丁：
   - `drivers/tee/qseecom/core.c`（59.8 KB，新驱动）
   - `qcom_scm.c` +956 行（SCM qseecom 函数）
   - `tee_core.c` +166/−42（TEE 核心支持按名开会话）
   - `tee_drv.h` +12、`tee.h` +1
5. 版本与配置保持一致 → 现有模块仍可加载
6. DTB 保持我们已改好的版本，无需从源码重建

**风险**：编译约 30-60 分钟；补丁从 7.1 回移到 6.18 需要人工适配。

## 阶段 2：内核驱动

### 2.1 获取参考补丁
- [ ] 拉取 `goodix-fp-spi` 补丁系列
- [ ] 拉取 `goodix-qsee` 补丁系列
- [ ] 评估与 6.18.44 的 API 差异

### 2.2 传感器驱动（module，低风险）
- [ ] 编译 `goodix_fp_spi.ko`（gcc-14）
- [ ] 验证 vermagic / struct module 段大小
- [ ] `insmod` 试加载（不依赖 DT 也能加载，probe 会失败但不影响系统）

### 2.3 TEE 驱动（风险较高）
- [ ] 评估是否能作为 module（QSEECOM 后端）
- [ ] 编译、加载、验证 `/dev/teepriv0` 出现

### 2.4 DT 节点（**不走重建启动镜像**）
- [ ] 用 **runtime DT overlay** 方式（`/sys/kernel/config/device-tree/overlays/`）
- [ ] 节点内容（已确认）：
      fingerprint_goodix: reset=GPIO125(低有效), irq=GPIO23(高有效), 供电 3.008V
- [ ] 如果 overlay 不可行，再考虑重建 ABL 镜像（高风险，最后手段）

## 阶段 3：用户态验证

- [ ] 编译并运行 `gfharness`，验证：TA 加载 → open session → invoke (INIT)
- [ ] 部署 qsee-supplicant
- [ ] 编译 libfprint 驱动
- [ ] `fprintd` 枚举设备

## 阶段 4：系统集成

- [ ] `pam_fprintd` 配置（sudo / 登录 / 锁屏）
- [ ] GDM 指纹登录
- [ ] 实际测试录入与解锁

---

## 风险与回退

| 风险 | 缓解 |
|---|---|
| 内核模块导致死机 | 全部 module 化；`insmod` 前记录；重启即恢复 |
| DT 改动破坏启动 | **优先用 runtime overlay**，不改启动镜像 |
| TEE 驱动导致 panic | 单独加载，出错立即重启 |
| 误写 Android 分区 | 所有 Android 分区**只读挂载**，用完卸载；操作前 `mount | grep -c` 确认 |
| 影响现有系统 | 每步前备份、每步后验证；关键步骤写回退命令 |
