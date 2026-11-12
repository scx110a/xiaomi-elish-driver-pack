# Armbian sm8250 内核基线获取（不随包提供完整补丁集）

原指纹归档里的 `armbian-patches/` 共 69 个文件，其中 **33 个是 0 字节**（不完整），
无法仅凭它重建基线树。本包因此**不再附带该补丁集**，改为按下面方式获取精确基线。

## 本机验证过的基线

- Linux mainline **6.18.44**（`/var/tmp/linux-6.18.44.tar.xz`，
  sha256 `0f72d938f06828e82c90405174fe572287db7bfe089e2fc46572a99a7f240d43`）；
- Armbian sm8250 补丁集（本机 `/var/tmp/kbuild-src` 已应用）；
- 内核配置：本包 `kernel/config-used-6.18.44`（含本项目所需 TEE/QSEECOM/BTF 等）；
- **0056-...remote-heap-CMA...patch 是 DTB/传感器最关键的补丁**（原归档里该文件为 0 字节！）；
  本机已安装 DTB 已包含对应 `/reserved-memory/fastrpc` + `memory-region`，不要用缺它的树重建 DTB；
- 指纹驱动改动：本包 `kernel/qseecom/`、`mdt_loader.c`、`qcom_scm.c`、`goodix_fp_spi/`；
- 传感器改动：本包 `kernel/patches/0001-fastrpc-...patch`。

## 重新获取 Armbian 基线（推荐）

1. 查看本机记录：`/etc/armbian-release` 中 `BUILD_REPOSITORY_COMMIT`（本机为 `db44df750`）、
   `LINUXFAMILY=sm8250`。
2. 克隆 Armbian build 仓库并检出对应 commit：
   ```bash
   git clone https://github.com/armbian/build
   cd build && git checkout db44df750
   ```
3. 取其 sm8250 `current` 内核补丁集与配置（不同 Armbian 版本路径可能不同，通常在
   `patch/kernel/archive/sm8250-6.18/`、`config/kernel/linux-sm8250-current.config`），
   按 Armbian 的方式应用到 Linux 6.18.44，再用本包 `config-used-6.18.44` 覆盖/对比配置。
4. 若你本机已有可用的 `/var/tmp/kbuild-src`，可直接在其上增量构建（最快，见 `kernel/README.md` 步骤 A）。

## 注意

- 本包 `kernel/patches/fingerprint/` 下的 `fp-spi-*.patch`、`scm-qseecom.patch` 是原项目对
  上游文件的**分片改动记录**（便于审阅），不是完整 Armbian 基线。
- 内核版本必须保持 `6.18.44-current-sm8250`（ABI/模块依赖）；升级内核需获取对应版本补丁集并重做。
