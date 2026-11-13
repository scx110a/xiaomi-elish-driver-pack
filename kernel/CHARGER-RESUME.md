# SMB5 充电：suspend/resume 后 Type-C legacy 卡死修复

**补丁**：`patches/0004-smb5-typec-legacy-reset-resume.patch`
（修改 `drivers/power/supply/qcom_pm8150b_charger.c`）

## 症状

- 充电器插着时 s2idle suspend/resume 后，充电卡死：
  - `typec` 显示 `power_role=source [sink]`
  - `pm8150b-charger current_max=100000`（100mA）
  - 电池实际 `Discharging`
  - `bq2597x present=0`
- 热重启不恢复；只有完整断电/Android 充电后才恢复。

## 根因

Android 的 SMB5 整合栈在 probe 时用 `smb5_configure_typec()` 清理
`LEGACY_CABLE_STATUS`（VBUS 先于 CC attach 会把标准线误判为 legacy cable）。
Linux 侧是精简 SMB5 + 独立 TCPM，原驱动没有这个 legacy 复位，也没有 system resume
处理；suspend 后 PMIC 停在 legacy/source 状态，SMB5 只给 100mA。

## 修复

在 `qcom_pm8150b_charger.c` 中：

1. `smb5_configure_typec()`：读 `LEGACY_CABLE_STATUS`（`chip->base + 0x50D`，
   bit1）；若置位，切换 `TYPE_C_MODE_CFG` 的 power-role 命令位：
   disable 50ms → dual 20ms → 恢复 try-sink。
2. 新增 `.resume`：重新执行 `smb5_init_hw()`，再 `smb5_configure_typec()`，
   调度 `status_change_work` 重跑 APSD/AICL。
3. probe 也调用一次 legacy 复位。

## 验证（2026-11-12，本机）

新内核 `6.18.44-current-sm8250 #7`，boot_b 内已含补丁。带充电器连续 3 轮
`suspend → 电源键唤醒`：

- 每轮都打印
  `qcom-pm8150b-charger ...: resetting stale Type-C legacy cable detection`
- `pm8150b-charger current_max=1950000` 保持
- `bq27z561-0 status=Charging`，电流约 +0.48A
- `suspend_stats.success=3, fail=0`
- `power_role` 仍可能显示 `source`（TCPM 类属性陈旧），但**充电不再卡 100mA**。

## 部署

按统一流程构建内核并写入 Linux 启动槽（`kernel/build-and-install.sh --write-boot` 或
`kernel/update-boot-image.sh`）。本机部署/回滚记录已移出包（工作区历史归档）。

## 限制

- `bq2597x` 快充泵仍可能 `present=0`；当前是 PMIC 基础充电（约 1.95A），
  不等于原装 33W 快充。快充（第 2/3 步）见工作区 TODO。
- 该修复针对 **suspend/resume 后充电卡死**，不改变 TCPM 的 `power_role` 显示。
- 不要把 `.config` 的 `CONFIG_LOCALVERSION` 去掉；模块 ABI 必须保持
  `6.18.44-current-sm8250`。
