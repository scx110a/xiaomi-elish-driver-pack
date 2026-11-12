# Android vs Linux 充电驱动对比（2026-11-12）

> 本文件是分析记录；实际修复补丁见 `kernel/patches/0004-smb5-typec-legacy-reset-resume.patch`
> 与 `kernel/CHARGER-RESUME.md`。

> 目的：解释为什么同一硬件 Android 能快充、Linux 在 suspend 后可能卡在
> `power_role=source` / `current_max=100mA`，以及第 2 步“基础快充”要改什么。
> 未修改任何系统/内核/驱动。

## 1. 两边的架构

| | Android | Linux 6.18.44 |
|---|---|---|
| 充电主控 | `qpnp-smb5.c` + `smb5-lib.c` 一套整合栈 | `qcom_pm8150b_charger.c`（精简 SMB5） |
| Type-C | SMB5 内部管理 CC/角色/APSD | 独立 `qcom_pmic_typec` + 通用 `tcpm.c` |
| 快充泵 | SMB5/`bq2597x`/`smb1390` 统一由充电栈控制 | `bq25970_charger.c` 独立驱动，依赖 Type-C 事件 |
| PD/QC | 完整 APSD/HVDCP/PD/PPS + votable | TCPM 有 PD 能力，但未与 SMB5 的 ICL/FCC 完整联动 |
| suspend/resume | 充电状态机和 Type-C 初始化更完整 | SMB5 驱动无 system PM；TCPM/charger 两套状态可能不同步 |

## 2. Android 关键代码

- `qpnp-smb5.c:3516 smb5_configure_typec()`：
  - probe 时检查 `TYPEC_LEGACY_CABLE_STATUS_BIT`；
  - 如果 legacy 状态被置位，先 `POWER_SUPPLY_TYPEC_PR_NONE`，延时 50ms，再
    `POWER_SUPPLY_TYPEC_PR_DUAL`；
  - 注释明确说明：**重启后 VBUS 先于 CC attach/detach，标准线会被误判成 legacy cable，
    必须复位 Type-C 检测逻辑**。
- `smb5-lib.c`：
  - `smblib_rerun_apsd()`（约 1491 行）、`smblib_rerun_apsd_if_required()`（约 1768 行）；
  - `smblib_set_usb_suspend()`（约 769 行）；
  - APSD/AICL/充电类型/温控/电流投票都在同一个 charger 状态机内。

来源：
- https://github.com/LineageOS/android_kernel_xiaomi_sm8250/blob/lineage-21/drivers/power/supply/qcom/qpnp-smb5.c
- https://github.com/LineageOS/android_kernel_xiaomi_sm8250/blob/lineage-21/drivers/power/supply/qcom/smb5-lib.c

## 3. Linux 差异

- `qcom_pm8150b_charger.c`：
  - `smb5_init_seq` 里有 Type-C 寄存器配置，但注释写着
    `FIXME: This will be handled by the type-c driver`；
  - **没有** Android 的 `smb5_configure_typec` legacy 复位；
  - `platform_driver` 只有 `.probe`，没有 `.suspend/.resume`；
  - 只在 probe 时 `dev_pm_set_wake_irq()`，`status_change_work` 负责 APSD/AICL。
- `qcom_pmic_typec.c`/`tcpm.c`：通用 TCPM，负责 CC/角色，但不做 SMB5 的 legacy 复位，
  也不负责把充电器类型/PDO 同步给 SMB5 的 FCC/ICL。
- `bq25970_charger.c`：`bq2597x_notifier_call()` 依赖 `tcpm-source-psy` 的 online 事件
  设置 `usb_present`；如果 Type-C 状态卡住，事件不再出现，快充泵就一直 `present=0`。

## 4. 对本次事故的解释

1. A 实验在**充电器插着**时 suspend；
2. resume 后 Linux 的 SMB5/TCPM 没有做 Android 那种 Type-C legacy 复位；
3. Type-C 卡在 source/legacy，`pm8150b` 只给 100mA，bq2597x 也不 present；
4. 热重启不做 legacy 复位，所以坏状态跨重启保留；
5. Android 启动时 `smb5_configure_typec` 会复位，所以 Android 充电正常；
6. 完整断电/Android 充一次后回 Linux，先开机再插充电器，恢复正常。

## 5. 第 2 步（基础快充）建议改动

1. 在 Linux charger probe/resume 增加 Type-C legacy 复位（等价 Android
   `smb5_configure_typec` 的 toggle）；
2. 给 `qcom_pm8150b_charger` 加 resume：重跑 APSD/AICL，恢复 current_max，
   `power_supply_changed()`；
3. `bq2597x` 在 probe/resume 时直接读 Type-C online，初始化 `usb_present`；
4. 把 PD/QC 固定档（5V/9V）的 ICL/FCC 设置接到 TCPM 协商结果。

**目标功率**：约 15–27W 基础快充。**不做**电荷泵 2:1、双电芯、PPS、33W+（第 3 步暂缓）。

## 6. 工作量

- 只修 suspend/resume + 充电恢复：2–5 天；
- 第 2 步基础快充：2–4 周；
- 第 3 步完整快充（电荷泵/双电芯/PPS）：再 4–8 周，暂不做；
- 完整移植 Android SMB5 栈：2–4 人月，不推荐。

## 7. 规避

- **充电器插着时不要 suspend**；
- 若又卡在 `power_role=source`/100mA：拔充电器 + 完整断电，或去 Android 充一次再回来；
- 不要反复 unbind/bind 充电器/TCPM。
