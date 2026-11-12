# 本地 libfprint-2-2 包（Goodix QSEE 自编译版）

本机把 Ubuntu 的 `libfprint-2-2`（1:1.94.7）替换成**同名本地包**：
`1:1.94.100+goodixqsee1-0`（wrobelda/libfprint `goodix-qsee` 1.94.100 + goodixqsee 日志补丁）。

## 为什么

- 指纹驱动 `goodixqsee` 不在 Ubuntu 版 libfprint 里；ninja 覆盖安装后 dpkg 状态是"被修改"，apt 升级有覆盖风险。
- 用同名本地 deb 后，`dpkg` 认为这就是当前安装版本，`fprintd` 的依赖
  `libfprint-2-2 (>= 1:1.94.1)` 仍然满足；再 `apt-mark hold` 防升级。

## 当前状态

```bash
dpkg -s libfprint-2-2 | grep -E '^Version|^Status'
# Version: 1:1.94.100+goodixqsee1-0
dpkg -V libfprint-2-2          # 应无输出
apt-mark showhold              # 应包含 libfprint-2-2
fprintd-list "$USER"           # 正常
```

## 本地 deb

- 路径（示例，按你自己的本地目录替换）：`/home/<user>/local-debs/libfprint-2-2_1.94.100+goodixqsee1-0_arm64.deb`
- 包含文件：`libfprint-2.so.2.0.0` + `.so.2`/`.so`、udev 规则、hwdb、GIR/typelib、pkgconfig、docs。
- 依赖复制自 Ubuntu 包（`libc6`、`libfprint-2-tod1`、`libglib2.0-0t64`、`libgudev-1.0-0`、`libgusb2`、`libnss3`）。

## 重新安装/升级后注意

`dpkg -i` 可能会清除 apt hold，安装后务必重新：

```bash
sudo dpkg -i /home/<user>/local-debs/libfprint-2-2_1.94.100+goodixqsee1-0_arm64.deb
sudo apt-mark hold libfprint-2-2
```

## 回退到 Ubuntu 原版

```bash
sudo apt-mark unhold libfprint-2-2
sudo apt-get install --reinstall libfprint-2-2    # 会装回 Ubuntu 1.94.7
sudo systemctl restart fprintd
```
> 回退后指纹（goodixqsee）会消失；需要重新按 `fingerprint/INSTALL.md` 编译/安装自编译版本。

## 重新构建本地 deb 的方法

1. 按 `fingerprint/INSTALL.md` 第 6 节编译安装自编译 libfprint；
2. 用 `dpkg-deb --build --root-owner-group` 打包（control 的 Version 用 `1:1.94.100+goodixqsee1-0`，Depends 从原 Ubuntu 包复制）；
3. 安装后 `apt-mark hold libfprint-2-2`。
