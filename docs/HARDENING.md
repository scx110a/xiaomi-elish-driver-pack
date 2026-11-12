# 安全加固（可选，推荐）

本机（Xiaomi Pad 5 Pro / elish）在 2026-11-13 完成了以下加固，**保留指纹授权**：

## 已实施

1. **关闭 GDM 自动登录**
   - `/etc/gdm3/custom.conf` → `AutomaticLoginEnable=false`
   - 保留 `AutomaticLogin=caitming` 便于恢复；**重启后生效**

2. **删除 sudo 免密**（2 处）
   - `/etc/sudoers.d/010-caitming-nopasswd`（全量 `NOPASSWD: ALL`）→ 已移到备份；
   - `/etc/sudoers` 中 `caitming ALL=(ALL) NOPASSWD: /usr/bin/psd-overlay-helper` → 已删除
     （psd 当前为 symlink 模式，不使用 overlayfs）；
   - `visudo -c` 通过；`sudo -n true` 现在返回「需要密码」；
   - `020-nopasswd.disabled` 未动（文件名含 `.`，sudo 会忽略，属无效文件）。

3. **保留并验证指纹授权**
   - sudo：`/etc/pam.d/sudo` 的 `pam_fprintd.so max-tries=1 timeout=8`（指纹优先，8 秒超时→密码回退）；
   - GDM 登录/锁屏：`enable-fingerprint-authentication=true` + `/etc/pam.d/gdm-fingerprint`；
   - 已录入指纹：`left-index-finger`（Goodix QSEE）；
   - 实测：指纹按一下即可完成 sudo 授权。

4. **相机辅助脚本去 sudo 依赖**（删除免密后的配套修复）
   - `/usr/local/bin/elish-camera-ctl`、`/usr/local/bin/elish-camera-rear-autofocus`
     去掉 `sudo -n`；
   - `/etc/elish-orientation.conf` → `root:caitming 0664`（`elish-camera-ctl orientation set` 用户可写）；
   - VCM focus sysfs 由 udev 规则 `99-elish-vcm.rules` 保证 video 组可写；
   - 包内 `camera/install.sh` 已在新装时自动设置这些权限。

## 验证

```bash
sudo -k; sudo true        # 应要求指纹（8s 内）或密码
sudo -n true              # 应失败（无免密）
fprintd-list "$USER"      # 应有指纹模板
elish-camera-ctl orientation show
```

## 备份 / 恢复

- 用户可读：`~/security-hardening-backup-latest/`
  - `restore.sh`：恢复 sudo 免密 + GDM 自动登录（指纹 PAM 不动）
  - `fix-camera-sudo-deps.sh`：相机辅助脚本去 sudo 依赖（已执行过）
  - `README.md`：完整记录
- root 副本：`/root/security-hardening-backup-20261113-005009/`

恢复免密/自动登录（需要 root，用指纹或密码）：

```bash
sudo bash ~/security-hardening-backup-latest/restore.sh
```

## 注意

- 删除免密后，**DSH Agent（以 caitming 运行、无交互终端）不能再执行 sudo**；需要 root 的操作请在桌面
  终端手动执行，或临时恢复免密后操作、再重新加固。
- 指纹 sudo 属安全敏感配置；请确保密码登录可用（`/etc/pam.d/sudo` 已包含密码回退）。
- 升级 `libfprint-2-2` 等包可能影响指纹；升级后按 `fingerprint/INSTALL.md` 重装自编译库。
