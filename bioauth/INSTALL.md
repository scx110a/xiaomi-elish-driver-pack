# bioauth — 可选组件安装说明（驱动包内）

`bioauth` 是**通用**（不绑定 Goodix/TA）的生物认证 PAM 栈：PAM 模块 + root daemon +
单窗口 GTK4“安全屏幕”helper。它复用本包的 `fprintd`/libfprint 后端，因此**依赖先装好指纹栈**
（本包默认安装指纹；若用 `--no-fingerprint` 则 bioauth 只能走密码/不可用）。

> **默认不安装。** 合并包的一键安装器只有显式加 `--with-bioauth`（或
> `--bioauth-sudo` / `--bioauth-pkexec`）才会安装本组件；不带这些参数时完全不动
> `sudo`/`pkexec` 的 PAM 配置。

## 安装

```bash
# 只安装文件和测试服务（不改 sudo / pkexec）
sudo ./install.sh --no-deps --skip-extract --skip-build --with-bioauth --yes

# 安装并接入 sudo（PAM_IGNORE 时回落到密码；会先备份 /etc/pam.d/sudo）
sudo ./install.sh --no-deps --skip-extract --skip-build --bioauth-sudo --yes

# 安装并接入 pkexec/polkit（取消时回落到 GNOME 密码框；会先备份 /etc/pam.d/polkit-1）
sudo ./install.sh --no-deps --skip-extract --skip-build --bioauth-pkexec --yes

# 两者都接
sudo ./install.sh --no-deps --skip-extract --skip-build --bioauth-sudo --bioauth-pkexec --yes
```

也可以单独运行本组件（不经过 turnkey）：

```bash
cd bioauth
sudo scripts/install-bioauth.sh --dry-run        # 先看计划
sudo scripts/install-bioauth.sh                  # 安装文件（不动 PAM）
sudo scripts/install-bioauth.sh --enable-sudo    # 接入 sudo
sudo scripts/install-bioauth.sh --enable-pkexec  # 接入 pkexec/polkit
sudo scripts/install-bioauth.sh --rollback       # 恢复 sudo + polkit PAM 备份
sudo scripts/install-bioauth.sh --uninstall      # 卸载 bioauth（先恢复 PAM；指纹包文件不动）
```

依赖（Ubuntu 24.04）：`build-essential pkg-config libpam0g-dev python3 python3-gi gir1.2-gtk-4.0`
（turnkey 的 apt 依赖已包含；单独安装时自行准备）。

## 安全前提（红线）

1. **保留救援通道**：pkexec/polkit 不改时是纯密码救援；改 sudo 前先确认密码/root 可用。
2. **先用测试服务验证**，不要一上来改 sudo：
   ```bash
   pamtester bioauth-test "$USER" authenticate   # 无 TTY 时会弹全屏 bioauth 界面
   ```
3. 所有 PAM 改动都会备份到 `/root/bioauth-sudo-backup-<时间戳>/`；禁止只装一半组件
   （module/daemon/helper 必须同一构建，`bioauth-doctor` 会检查）。
4. 不要和本包旧的 `--pam`（`fingerprint/system/pam.d/sudo` 指纹栈）**同时**启用；
   turnkey 会拒绝同时传 `--pam` 与 `--bioauth-sudo`。
5. debug 日志只能通过环境变量开启（`BIOAUTH_PAM_DEBUG=1`），不要把 `debug` 写在真实
   `/etc/pam.d/sudo` 行上；日志不记录密码。

## 验证与诊断

```bash
bioauth-doctor                 # 安装一致性 + live helper ready + PAM 配置 + daemon 状态（31 项）
sudo systemctl status bioauthd
pamtester bioauth-test "$USER" authenticate
# 可选端到端（无需 root、不碰系统 PAM）：
make check-tty                 # pty 终端 UI 流程
make check-gui                 # 真实 Wayland 全屏 UI + AT-SPI 交互
```

## 回滚 / 卸载

- `sudo scripts/install-bioauth.sh --rollback`：从最近备份恢复 `/etc/pam.d/sudo` 与
  `/etc/pam.d/polkit-1`，停止 bioauthd（组件文件保留）。
- `sudo scripts/install-bioauth.sh --uninstall`：先做回滚，再删除 module/daemon/helper/
  doctor/config/systemd/PAM 测试服务；共享的 `fprintd-verify` 队列与指纹子包文件不动。
- 也可直接把合并包里的 `bioauth/` 目录删掉（它不参与默认安装）。

## 说明

- 本组件代码与 `/home/.../default-workspace/bioauth` 同源；包内快照包含源码、测试、
  诊断工具与文档（audit-pam / uac-secure-screen / verification）。
- 设计/安全边界、UAC 与 pkexec 对比见 `uac-secure-screen-20261113.md`；独立审计见
  `audit-pam-20261113.md`。
- GDM 未改（仍 stock `gdm-fingerprint` + `pam_fprintd`）；pkexec 是否接入由上面的
  `--bioauth-pkexec` 显式决定。
