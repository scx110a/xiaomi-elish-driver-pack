# 内核集成点（缺一不可，从零构建会直接编译失败）

指纹项目除了 4 个源文件，还必须把 **3 个头文件** 和 **2 个 Kconfig/Makefile 集成点** 放进内核树。
本包已提供这些文件（路径与内核树一致）：

```
kernel/include/linux/firmware/qcom/qcom_scm.h     # qcom_scm_qseecom_* listener API
kernel/include/linux/soc/qcom/mdt_loader.h        # qcom_mdt_read_image/get_image_size
kernel/include/uapi/linux/tee.h                   # TEE_IMPL_ID_QSEECOM=5（core.c 使用）
```

## 两种做法

- **推荐（完整可复现）**：直接应用权威系列补丁
  `kernel/patches/fingerprint/qseecom-tee-series.patch`（14 文件，含 drivers/tee/Kconfig/Makefile、
  drivers/soc/qcom/Kconfig、3 个头文件、Documentation、MAINTAINERS）；再应用 `0001/0002/0003`。
- **手工（使用本包已导出的文件）**：按下面步骤。

## 手工步骤

```bash
cd <内核源码树>
# 1) QSEECOM 驱动 + 头文件
cp -a <包>/kernel/qseecom drivers/tee/qseecom
cp <包>/kernel/include/linux/firmware/qcom/qcom_scm.h include/linux/firmware/qcom/qcom_scm.h
cp <包>/kernel/include/linux/soc/qcom/mdt_loader.h   include/linux/soc/qcom/mdt_loader.h
cp <包>/kernel/include/uapi/linux/tee.h              include/uapi/linux/tee.h

# 2) drivers/tee 集成（若尚无）
grep -q 'drivers/tee/qseecom/Kconfig' drivers/tee/Kconfig || \
  printf '\nsource "drivers/tee/qseecom/Kconfig"\n' >> drivers/tee/Kconfig
grep -q 'CONFIG_TEE_QSEECOM' drivers/tee/Makefile || \
  printf 'obj-$(CONFIG_TEE_QSEECOM) += qseecom/\n' >> drivers/tee/Makefile

# 3) 让 QCOM_MDT_LOADER 用户可选（pristine 6.18.44 是 `tristate` 无提示，
#    会被 QCOM_Q6V5_PAS=m select 成 m，导致 TEE_QSEECOM=y 依赖失败）
grep -q 'tristate "Qualcomm MDT firmware loader"' drivers/soc/qcom/Kconfig || \
  patch -p1 < <包>/kernel/patches/0003-qcom-mdt-loader-user-selectable.patch

# 4) 配置依赖检查（config-used-6.18.44 已满足）
grep -E '^CONFIG_(TEE|TEE_QSEECOM|QCOM_QSEECOM|QCOM_MDT_LOADER|QCOM_FASTRPC|DEBUG_INFO_BTF|MODULE_ALLOW_BTF_MISMATCH|LOCALVERSION)=' .config
```

可选（非构建必需）：`Documentation/tee/qseecom.rst`、`MAINTAINERS` 条目；本包未附带。

`drivers/tee/qseecom/Kconfig` 里 `TEE_QSEECOM` **depends on QCOM_MDT_LOADER**；
`QCOM_MDT_LOADER` 在 6.18.44 里已是 tristate（用户可选）。要求：

- `CONFIG_TEE_QSEECOM=y` 时 `CONFIG_QCOM_MDT_LOADER` 必须是 `y`（不能 `m`，否则 Kconfig 依赖不满足）；
- `CONFIG_QCOM_QSEECOM=y`、`CONFIG_TEE=y`、`CONFIG_LOCALVERSION="-current-sm8250"`。

## 为什么必须带这些头文件

`kernel/qseecom/core.c` 使用 `TEE_IMPL_ID_QSEECOM`（`tee.h`）、QSEECOM listener 注册 API
（`qcom_scm.h`）、以及 mdt_loader 的镜像读取 API（`mdt_loader.h`）。只覆盖 `.c` 文件会在编译时报
隐式声明/未定义符号。本包从实际可编译的 6.18.44 树中原样导出，无需修改。
