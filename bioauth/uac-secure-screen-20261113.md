# UAC 安全桌面 / Linux pkexec / bioauth 全屏覆盖层：能力对比与可行性（2026-11-13）

> 结论先说：`bioauth-ui` 的全屏覆盖层只是**表现层（presentation）**，不是安全边界。
> 它能做到"看起来像 UAC"：独占全屏、遮住所有窗口和 GNOME 顶栏、居中卡片、单一窗口。
> 它**不能**提供 Windows UAC 安全桌面的核心保证：可信输入通道、输入隔离、防伪造、防同用户进程偷看按键。
> 主要原因不在我们的代码，而在 Linux 桌面（GNOME/mutter + Wayland）**没有给普通客户端提供 secure desktop / secure attention 原语**；
> 真正要接近 UAC，需要 compositor / greeter 级合作（GNOME Shell 内部 modal 或 GDM 会话切换），
> 这正是 GNOME 自己的 polkit agent 与我们的实现之间的本质差距（见 §2 源码证据）。

本文所有命令都在本机（Ubuntu 24.04 / GNOME 46.0 / mutter 46.2 / Wayland / aarch64 / 用户 caitming）实际执行过。
证据截图在 `bioauth/gui/tests/screenshots/`，测试驱动在 `bioauth/gui/tests/helper_driver.py`。

---

## 1. Windows UAC 安全桌面（对照基准）

Windows UAC 的同意/凭据提示运行在 **Secure Desktop** 上，机制要点：

- 切换到一个由 winlogon 拥有的独立桌面（`Winlogon` desktop），正常用户桌面（`Default`）被切走，不是"盖一层窗口"；
- 只有受信任的系统进程（consent.exe / winlogon）能在这个桌面上创建窗口或接收输入；
- 普通进程无法读取该桌面上的键盘输入（除非内核级恶意软件），也无法在上面画窗口/伪造提示；
- 背景是**冻结/压暗的桌面快照**（不是实时桌面），视觉上明确区别于普通会话；
- Ctrl+Alt+Del 是 **Secure Attention Sequence**，只有内核/winlogon 能响应，用户可用它确认"这是真的系统提示"。

结论：UAC 的强度来自"独立桌面 + 可信输入 + SAS"，**不是**来自"全屏 + 变暗"。我们只能模仿外观，无法用普通 Wayland 客户端复刻前三点。

---

## 2. Linux：pkexec + polkit agent（GNOME 46 实测源码）

### 2.1 GNOME 自己的提权 UI 在哪里

本机 `gnome-shell` 是 46.0，JS 编译进 `libshell-14.so` 的 GResource（不存在 `/usr/share/gnome-shell/org.gnome.Shell.gresource`，
所以任务里给的 `gresource list /usr/share/gnome-shell/org.gnome.Shell.gresource` 在本机为空/不可用）。实际可用的提取方式：

```bash
$ gresource list /usr/lib/gnome-shell/libshell-14.so | grep -i polkit
/org/gnome/shell/ui/components/polkitAgent.js

$ gresource extract /usr/lib/gnome-shell/libshell-14.so \
    /org/gnome/shell/ui/components/polkitAgent.js > /tmp/gs-extract/polkitAgent.js   # 476 行
$ gresource list /usr/lib/gnome-shell/libshell-14.so | grep -E 'modalDialog|lightbox|dialog\.js'
$ gresource extract /usr/lib/gnome-shell/libshell-14.so /org/gnome/shell/ui/modalDialog.js > /tmp/...
$ gresource extract /usr/share/gnome-shell/gnome-shell-theme.gresource \
    /org/gnome/shell/theme/gnome-shell-dark.css > /tmp/gs-extract/gnome-shell-dark.css
```

### 2.2 它做了什么（源码结论）

- `polkitAgent.js` 的 `AuthenticationDialog extends ModalDialog.ModalDialog`（`class AuthenticationDialog extends ModalDialog.ModalDialog`），
  默认 `actionMode: Shell.ActionMode.SYSTEM_MODAL`、`styleClass: 'prompt-dialog'`。
- `modalDialog.js` 的 `_init` 里：
  - `this._lightbox = new Lightbox.Lightbox(this, {inhibitEvents: true, radialEffect: true});`
    `Lightbox` 用 `RadialShaderEffect` 做"中心亮/四周压暗"的 vignette，且 `inhibitEvents` 让点击落在对话框外也无效；
  - shell 主题 CSS：`.lightbox { background-color: black; }`，`.modal-dialog` 是 `#353535`、圆角 16px；
  - `this._eventBlocker = new Clutter.Actor({reactive: true})` 放在背景层，吞掉背景点击；
  - `Main.layoutManager.modalDialogGroup.add_child(this)` + `Layout.MonitorConstraint()`：覆盖**单个显示器**（不是所有输出）；
  - `open()` 里 `Main.pushModal(this, {actionMode: Shell.ActionMode.SYSTEM_MODAL})`，并检查
    `grab.get_seat_state() !== Clutter.GrabState.ALL` 就放弃并 `_emitDone(true)`——即**输入 grab 由 compositor 授予**。
  - 源码注释自己承认 grab 可能失败：*"This can fail if e.g. unable to get input grab … In an ideal world this wouldn't happen (because the Shell is in complete control of the session) but that's just not how things work right now."*
- 因此 GNOME 的提权对话框：**压暗 + 模态 + compositor 级输入 grab + shell chrome 层级（任何普通 app 无法画在它上面）**。
- 但它**仍然不是** Windows Secure Desktop：
  - 桌面仍在原会话里（没有切换到独立桌面），只是被遮罩；
  - 不阻止同用户进程直接读 `/dev/input/event*`（见 §4）；
  - 不阻止截屏/录屏；
  - 默认只覆盖一个显示器（多屏时其他屏幕不压暗）；
  - 提权成功后仍然只是 PAM/polkit 授权，没有 SAS 概念。

### 2.3 与我们实现的层级差距

| 维度 | GNOME polkit agent（shell 内） | bioauth-ui（普通客户端） |
|---|---|---|
| 绘制层级 | shell `modalDialogGroup`，高于所有 app | 普通 toplevel，compositor 排序，新窗口可能出现在其上方 |
| 输入 grab | `Main.pushModal(SYSTEM_MODAL)`，compositor 级 | 无；只有本窗口的键盘焦点 |
| 遮罩 | shell lightbox（径向压暗） | 自己画的接近纯黑全屏不透明层 |
| 多显示器 | 单显示器（MonitorConstraint） | 单窗口（本机单屏 1280x800@2x） |
| 被同用户进程伪造 | 难（要冒充 shell） | 容易（再开一个全屏窗口即可） |
| 安全边界 | 无（仍是用户会话内） | 无（更弱） |

> 换句话说：**GNOME 的 polkit UI 已经是我们能参考的"客户端上限"，而它本身仍不是安全桌面。**
> 我们不可能用普通 Wayland 客户端做到比 GNOME shell 内部 modal 更强。

---

## 3. 我们实际实现的覆盖层（bioauth-ui）

### 3.1 设计

- 单窗口、`set_decorated(False)`、`fullscreen()`、`Gtk.Overlay`：底层全屏不透明近黑画布 + 居中卡片
  （`460` 逻辑 px 宽，本机 scale=2 → 916 物理 px；实测居中偏移 −0.5 px）；
- 卡片：锁图标 + 标题（指纹验证/密码验证/验证通过）+ 服务名、状态行、常驻 red/green detail 行、spinner、
  密码输入+验证按钮、次要切换按钮、取消按钮（无标题栏，所以必须留取消/Esc 出口）；
- 协议不变（fd3 newline JSON），新增 `{"event":"ready","proto":2}`（窗口 map 后恰好一次）、
  `{"event":"password",...,"attempt":N}` 回显；`BIOAUTH_UI_LOG` 只记 op/事件，不记密码。

### 3.2 为什么是"不透明近黑"而不是"半透明压暗"（本机实测）

任务要求"dark dim overlay"。我们先按半透明实现（窗口背景透明 + `rgba(8,9,12,0.62)` 遮罩），
在真实会话里用 `gnome-screenshot` 抓 2560x1600 全屏后取样，结果：

| 实验（同一程序，仅窗口状态不同） | 预期半透明结果（0.62×黑 + 0.38×底图） | 实测 |
|---|---|---|
| **fullscreen** 透明窗口 + 半透明遮罩 | 底图被压暗，例如 RGB(54,56,59) | **RGB(5,6,7)，均匀纯黑** |
| fullscreen 透明窗口（无内容） | 底图原样 | 底图可见（空 surface 未触发） |
| **1280x800 非 fullscreen** 透明窗口 + 同遮罩 | RGB(54,56,59) | **RGB(54,56,59) ✅ 完全符合** |
| maximized（1280x800，非 fullscreen） | RGB(54,56,59) | **RGB(54,56,59) ✅** |
| fullscreen + `Gtk.Window.set_opacity(0.99)` | 仍应半透明 | 仍 **RGB(5,6,7)** |
| fullscreen 不透明 `#08090d` | RGB(8,9,12) | RGB(8,9,12) ✅（全屏每个角落都覆盖） |

结论：在本机 mutter 下，**非全屏**客户端能正确 per-pixel alpha 合成；
**全屏**客户端的透明像素被合成到黑色上（与 direct scanout / unredirect 全屏窗口的行为一致），
`set_opacity()` 也不能绕过。Lead 的独立 e2e 截图观察到同样的"整帧全黑、看不到背后浏览器"。

因此交付实现选择 **fullscreen + 不透明近黑画布**（`#08090d`）：

- 这是普通客户端在本机唯一能稳定做到"覆盖整个输出（含 GNOME 顶栏）+ 深色 UAC 观感"的方案；
- 它和 Windows UAC 的"冻结/压暗桌面快照"在观感上同类（UAC 也不是实时桌面）；
- 代价：看不到实时桌面内容（半透明压暗在 fullscreen 下不可用），且顶栏被完全盖住（实测 2560x1600 四角/边缘均匀 `#08090d`，基线中原本亮的像素 100% 被覆盖，见 §6）。

> 备注：如果将来换成不做全屏 unredirect 的 compositor/GPU 路径，可把 `.bioauth-dim` 改回
> `rgba(8,9,12,0.62)` 即得到半透明压暗；代码里保留了独立 CSS 类便于切换，但**默认不赌这个行为**。

---

## 4. 我们实现能/不能保证什么（诚实清单）

### 能保证（已实测，见 §6）

1. 视觉上的 UAC/pkexec 风格单一全屏窗口：覆盖 1280x800 的逻辑屏幕全部像素（包括 GNOME 顶栏区域），
   卡片居中，无双窗口/无第二 toplevel，密码/指纹/锁定/不可用/成功状态都有明确且常驻的视觉反馈；
2. 用户交互语义正确：切换指纹/密码、取消、成功 1s 绿色停留后**必然退出**（`destroy()+loop.quit()`，
   实测 1.15–1.19s，EOF 0.06–0.12s 退出，180s 超时路径（用 `BIOAUTH_UI_TIMEOUT=3` 复现同一代码路径）2.98s 退出）；
3. 防重复提交：提交后 entry/按钮立即失效，直到 PAM 下发下一个 `password`/`password_result`；
   AT-SPI 双击验证只产生 1 个 password 事件；`attempt` 回显与 prompt id 一致（11/12）；
4. 协议健壮：窗口 map 后才发 ready（恰好一次，含 `proto:2`）；GTK 初始化失败则非 0 退出且不发 ready；
   fd3 先到的消息会缓冲到 ready 之后处理；父进程 EOF 立即退出；BIOAUTH_UI_LOG 可诊断"起了但没出窗口"。

### 不能保证（这是安全边界问题，不是 bug）

1. **输入不可隔离**：该窗口只是普通 Wayland 客户端，没有 compositor 级 grab。
   GNOME 的 Alt+Tab / Super / Overview、通知、锁屏等 shell 快捷键仍然有效，用户可切走；
   同用户的其他进程仍可抢焦点/在上方开窗（新窗口通常会被 compositor 提升）；
2. **同用户进程可读按键**：本机 `caitming` 在 `input` 组（`getent group input` → `input:x:996:caitming`；
   `ls -l /dev/input/event0` → `crw-rw---- root input`），组内用户对 `/dev/input/event*` 可读（已用 `[ -r ]` 逐个验证）。
   也就是说，在密码输入框里敲的密码，同用户的任意进程都能从 evdev 直接读走——这层与 UI 无关，换任何 GTK 窗口都一样；
3. **无法防伪造**：同用户进程可以杀掉/遮住我们的窗口，自己画一个一模一样的"密码验证"窗口；
   普通客户端没有 SAS（Ctrl+Alt+Del / 可信指示），用户无法从视觉上区分真假；
4. **全屏请求由 compositor 裁决**：mutter 可以拒绝/改变全屏状态；多显示器时只覆盖当前输出；
   窗口是用户会话内的普通 surface，可被截图/录屏；
5. **不是可信输入通道**：没有 compositor/greeter 参与，密码经过的路径（GTK → helper 进程 → PAM → daemon）
   都在用户可控进程里（helper 以用户身份运行，可被同用户调试/替换）。
6. **PAM/daemon 侧的信任问题不在本层**：helper 只显示与收集，认证权威仍在 PAM/daemon；
   本层被攻破不等于认证被绕过，但意味着"看到的提示"和"输入的密码"不可信。

### 真正要接近 UAC 需要什么

- **compositor 级 secure attention / secure desktop**：由 GNOME Shell 自己（extension）用 `Main.pushModal(SYSTEM_MODAL)`
  + shell chrome 画提权 UI，禁止其它 client 抢占；这仍不是独立桌面，但比普通客户端强一个层级；
- **会话级切换**：像 Windows 的 `Winlogon` desktop 那样切到一个独立的 greeter/锁屏会话（GDM 的 greeter 进程、
  独立 VT/compositor）完成认证，再回到用户会话。GDM 的登录界面天然有这个形态，但普通 PAM 会话内无法调用；
- **输入路径最小化**：不做"在用户会话里收密码"的 UI，而是把认证提示放到 compositor/greeter；
  同时审计并移除不必要的 `input` 组权限（需要 root 改组，本次无 root，未做）；
- **Wayland 现实**：`gtk4-layer-shell`（本机未安装、无 root 安装）只能解决"层级"、不能解决"输入隔离/'可信'"，因此不是答案。

---

## 5. 与 pkexec/polkit 的实际行为对比（用户可见层面）

| 场景 | `pkexec` + GNOME polkit agent | 我们的全屏覆盖层 |
|---|---|---|
| 提权提示是否压暗屏幕 | 是（shell lightbox 径向压暗，非全黑） | 是（全屏近黑遮罩，实测覆盖顶栏） |
| 是否 modal/吞掉背景点击 | 是（grab + eventBlocker） | 否（背景被物理覆盖，点击落在本窗口；shell 快捷键仍可用） |
| 是否阻塞 shell 快捷键 | 基本是（SYSTEM_MODAL） | 否 |
| 多显示器 | 仅当前显示器被弹窗覆盖 | 仅当前输出全屏 |
| 失败/错误显示 | 对话框内错误行 | 常驻红色 detail 行（R8/R9 语义） |
| 成功后 | 对话框关闭，pkexec 执行 | 绿色"验证通过"停留 ~1s 后窗口必然退出，PAM 继续流程 |
| 可信性 | 无（同一用户会话内） | 无，且比 polkit agent 更弱 |

---

## 6. 验证记录（可复现）

环境：`XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus`，
本机 1280x800 逻辑（scale 2）→ 2560x1600 物理截图。测试全部使用**工作区副本** `bioauth/gui/bioauth-ui`
（mock parent：socketpair + fork + dup2(fd,3) + exec，未使用 root，未改 `/usr/local/libexec/bioauth-ui`）。

```bash
cd bioauth/gui/tests
python3 helper_driver.py gallery     # 截图 + 全屏覆盖/居中几何校验
python3 helper_driver.py lifecycle   # success/EOF/timeout 退出与单计时器
python3 helper_driver.py interact    # AT-SPI 点击/输入 + 防重复提交 + attempt 回显
python3 helper_driver.py all
```

### 6.1 覆盖层几何（gallery，最终代码）

```
screen: 2560x1600
bg_luma_min/max: 10.0 / 10.0        # 四角+四边+顶栏取样全部 RGB(10,10,11) 级暗色
bg_channel_spread: 0                # 均匀，说明是自绘遮罩而不是残留桌面
corners_covered: True
card_width_px: 916                  # 460 逻辑 px × scale2（含 1px 边框）
card_center_offset_px: -0.5         # 水平居中
card_centered: True
card_height_px: 446
bright_baseline_samples_outside_card: 51
bright_baseline_covered_pct: 100.0  # 覆盖前亮的桌面/面板像素 100% 被遮罩盖掉
```

截图（全屏 2560x1600，可直接看到四角/顶栏被覆盖、卡片居中）：
`00_desktop_baseline.png`（覆盖前桌面）、`01_fingerprint.png`、`02_fingerprint_mismatch.png`、
`03_fingerprint_retry_persistent.png`、`04_password_mode.png`、`05_password_mismatch.png`、
`06_locked_no_fingerprint.png`（`allow_fingerprint=false` → 无"使用指纹"）、
`07_fingerprint_unavailable.png`（不可用但保留"使用指纹"）、`08_success_green.png`。

### 6.2 生命周期（lifecycle）

```
success_exit:  status=0, 1.15–1.19s   # 绿色停留约 1s 后退出，不等 EOF
success_single_timer: 1               # fingerprint_result ok + success 只调度 1 个 close timer
eof_exit:      status=0, 0.06–0.12s   # 父进程死 → 立即退出
timeout_exit:  status=0, 2.98–3.02s, cancel_event=True
               # 用 BIOAUTH_UI_TIMEOUT=3 复现 180s 同一代码路径；实际默认 180s
```

helper 日志（`BIOAUTH_UI_LOG=/tmp/bioauth-ui-test.*.log`，0600）可看到
`start → GTK4 import ok → Gtk.init ok → presenting → window mapped → window ready → tx ready →
rx op=… → tx event=… → success shown; arming 1000 ms close timer → success dwell elapsed →
finish reason='success' → main loop exited`，全程不出现密码明文（password 事件只记 `<value redacted>`）。

### 6.3 交互与防重复提交（interact，AT-SPI）

需要 `gsettings set org.gnome.desktop.interface toolkit-accessibility true` 让 GTK4 导出 a11y 树
（本机原为 false；驱动会设置，若不想保留可 `gsettings reset org.gnome.desktop.interface toolkit-accessibility`）。
`pyatspi` 模块未安装，用的是 `python3-gi` 的 `Atspi 2.0` typelib；GTK4 的 `Gtk.Entry` 在 AT-SPI 里角色名是 `text`
（不是 `PASSWORD_TEXT`），按钮/取消均可 `Action.do_action`。

```
ready_event: {'event': 'ready', 'proto': 2}
switch_via_atspi: True
entry_text_set: True                       # EditableText.set_text_contents
click1/click2: True                        # 双击"验证密码"
password_events_after_double_click: 1      # 第二次点击未产生事件（防重复提交生效）
attempt_echo_1: 11                         # 回显 password op 的 attempt id
second_submit_after_reenable: True         # password_result 失败后重新输入并提交成功
attempt_echo_2: 12
cancel_click: True -> cancel_event=True, exit 0.39s
```

截图：`09_atspi_password_mode.png`、`10_atspi_verifying.png`（提交后 entry/按钮失效、spinner 转动）、
`11_atspi_password_retry.png`（红色"密码错误（1/3）"后可重试）。

### 6.4 未验证 / 已知限制

- 未在**多显示器**环境验证（本机只有一块 1280x800 逻辑屏）；
- 未验证真实 sudo/pkexec PAM 端到端（由 Lead 在自己的 e2e 中覆盖；本驱动只看 helper 协议侧）；
- 未验证"其他进程在全屏窗口之上再开窗口"的实际 compositor 行为（GNOME 通常会提升新窗口）；
- 未做 180s 真实等待（用 `BIOAUTH_UI_TIMEOUT` 复现同一代码路径）；
- 半透明压暗在 fullscreen 下不可用是**本机 mutter 实测结论**，其他 compositor/GPU 上可能不同；
- 截图验证走的是 GNOME 截图路径（`gnome-screenshot`），与 direct scanout 的实际屏幕表现一致（Lead 亦从另一侧观察到同样全黑现象）。

---

## 7. 建议的后续（按收益排序）

1. **别把它当安全边界宣传**：文档/UI 文案上明确"这是提示，不是可信通道"；
2. 如果要把安全级别提一档：写一个 **GNOME Shell extension 版 polkit 风格 agent**（`Main.pushModal(SYSTEM_MODAL)` +
   shell 自绘），或者把 bioauth 接到 **polkit agent 框架**里（`polkitAgent` 的 `PolkitAgent.Session`），
   让提权提示由 shell/agent 呈现，而不是普通客户端；
3. 真要 UAC 级：需要 **GDM greeter / 独立会话**方案（登录态认证在 greeter 里做），这是发行版级改动；
4. 审计 `input` 组（需要 root）：若 caitming 不需要读 evdev，移除可消除"同用户进程直接读按键"的最大捷径；
5. 多显示器支持：按 `Gdk.Display.get_monitors()` 每屏一个覆盖窗口（多做几个 toplevel 仍是单逻辑 session，
   不会变成安全桌面，但视觉更完整）。
