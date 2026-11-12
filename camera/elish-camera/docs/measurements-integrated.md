# elish-camera 端到端验收记录（2026-11-12 晚）

环境：Xiaomi Pad 5 Pro（SM8250 / elish），Armbian noble，内核 `6.18.44-current-sm8250 #10`，
Mesa 25.2.8 / Adreno 650（FD650），扇区 `_b`，桌面为 GNOME Wayland（测量时 Firefox/DSH 仍在运行，
负载 3–5，因此部分数字比空载偏低；已注明）。

管线：CAMSS packed RAW10 → GPU debayer（EGL/GLES，NV12/RGB888）→ Venus HEVC/H.264 → MP4；
或 → v4l2loopback（NV12）→ V4L2/PipeWire 消费者。

## 1. 拍照（全分辨率彩色）

| 相机 | 输出 | 旋转 | GPU RGB888 | 备注 |
|---|---|---|---|---|
| 前摄 OV8856 | 3280×2464 PNG | 180° | ~13 ms | AE/AWB 收敛后 wb≈(1.55,1.00,1.34)，肤色自然、方向正确 |
| 后摄 OV13B10 | 3120×4208 PNG | 270° | 21–26 ms | 白纸中性、文字可读 |

命令：

```bash
./build/elish-camera photo --camera front -o front.png
./build/elish-camera photo --camera rear  -o rear.png
```

## 2. 录像（全分辨率 + Venus 硬编）

| 相机 | 编码 | 分辨率 | 时长 | 帧数 | 平均 fps | 解码校验 |
|---|---|---|---|---|---|---|
| 前摄 | HEVC 20 Mbps | 3280×2464 | 8.02 s | 241 | **30.05** | `ffmpeg -f null -` 无错误 |
| 后摄 | HEVC 20 Mbps | 3120×4208 | 6.01 s | 180 | **29.96** | 无错误 |

- MP4 时间线按实测平均 fps 封装（`ELISH_MUX_FPS`），掉帧时不会“加速播放”。
- 编码器不限速吞吐（真实 sensor 内容，见 `measurements-encode.md`）：4208×3120 HEVC 90.6 fps、
  3280×2464 HEVC 135 fps，远高于 30 fps 实时需求。

命令：

```bash
./build/elish-camera record --camera front -o front.mp4 -d 8
./build/elish-camera record --camera rear  -o rear.mp4  -d 8 --codec hevc --bitrate 20000
```

## 3. 标准 V4L2 / PipeWire 接口（第三方 App）

- v4l2loopback 持久设备（模块参数已写入 `/etc/modprobe.d/elish-vcam.modprobe.conf`）：
  - `/dev/video2` = **Elish Front Camera**
  - `/dev/video20` = **Elish Rear Camera**
- `elish-camera serve --camera front|rear` 常驻推流，生产者输出 NV12：

| 项目 | 结果 |
|---|---|
| 前摄 loopback 3280×2464 NV12 | 30 fps 稳定（`elish-camera stream/serve`） |
| 后摄 loopback 3120×4208 NV12 | ~30 fps 稳定 |
| ffmpeg 消费者 | 1080p / 3280×2464 / 4208×3120 全部正常出帧 |
| GStreamer `v4l2src` | 全分辨率正常 |
| Guvcview | 可打开 NV12 并 STREAMON（GUI 出窗） |
| 3 消费者并发 | 30.04 fps（见 `docs/interface.md`） |
| PipeWire 相机节点 | WirePlumber 0.4.17 的 v4l2 monitor 自动发布 `/dev/video2` 为 `media.role=Camera` 节点（`Elish Front Camera (V4L2)`，id 57），`pipewiresrc` 可读 |
| xdg-desktop-portal Camera | frontend 存在：`org.freedesktop.portal.Camera`（`IsCameraPresent`、`AccessCamera`、`OpenPipeWireRemote`）；`IsCameraPresent=true` |

第三方 App 结论：

- **原生 V4L2 App**（Guvcview/ffmpeg/gst/OBS/Chromium 非沙箱）：直接读 `/dev/video2` 或 `/dev/video20` 即可。
- **Flatpak/portal App**（Cheese/Snapshot/浏览器沙箱）：依赖 PipeWire 中 `media.role=Camera` 节点；
  本机节点已由 WirePlumber 自动发布，portal 链路具备条件；本机未安装 Flatpak 相机 App，
  因此“沙箱内首次授权弹窗→画面”未做 App 级端到端验证（见 `interface.md` §6）。
- 前后摄**不能同时出流**（实测：前摄启动会打断后摄）。第三方 App 一次使用一个相机；
  切换需重启 `serve`/重新打开对应 loopback 设备。

## 4. 性能预算（每帧）

| 阶段 | 前摄 3280×2464 | 后摄 4208×3120 |
|---|---|---|
| GPU debayer（含回读，NV12） | 11.0–12.1 ms（p95 ≤14.7） | 18.7–19.8 ms（p95 ≤22.1） |
| Venus 编码 push（含拷贝） | 2.6–3.9 ms（p95 ~5.8） | 3.2–5.4 ms（p95 ~7.4） |
| loopback 推送（可选） | 独立线程（~15 ms 拷贝，不阻塞录像路径） | 同左 |
| 硬件采集节拍 | 30 fps（33.3 ms） | 30 fps |

结论：前摄 30 fps 有余量；后摄在系统有重负载（Firefox 等占 GPU）时可能掉到 25–29 fps，
空载时可稳定 30 fps。详见 `measurements-gpu.md`、`measurements-encode.md`。

## 5. 复现与验收命令

```bash
cd elish-camera && make

# 拍照
./build/elish-camera photo --camera front -o /tmp/f.png
# 录像 + 同步作为虚拟相机
./build/elish-camera record --camera front -o /tmp/f.mp4 -d 8 --loopback /dev/video2
# 常驻虚拟相机（第三方 App 用）
./build/elish-camera serve --camera front
# 消费者验证
ffprobe /tmp/f.mp4
ffmpeg -v error -f v4l2 -input_format nv12 -video_size 3280x2464 -i /dev/video2 -frames:v 10 /tmp/out-%02d.png
gst-launch-1.0 -q v4l2src device=/dev/video2 num-buffers=30 ! fakesink
gst-launch-1.0 -q pipewiresrc target-object=v4l2_input._sys_devices_virtual_video4linux_video2 num-buffers=30 ! fakesink
```

## 6. 本轮的鲁棒性修复（死机重启后复测发现）

1. **Venus 设备号不跨启动稳定**：重启后 encoder 从 `/dev/video16` 变成 `/dev/video15`（与 decoder 互换）。
   已改为按 `/sys/class/video4linux/*/name == "qcom-venus-encoder"` 发现设备，不再硬编码。
2. **采集可能偶发无帧**：`VIDIOC_DQBUF` 增加 2 s poll 超时，`camera_ready()` 失败后自动
   重新配置 media graph 并重试（最多 3 次），避免整机卡死。
3. **MP4 时间线**：封装用实测平均 fps，避免掉帧时播放加速。

## 7. 相机控制/对焦/分辨率（2026-11-12 晚补充）

- 统一入口：`elish-camera-ctl`（panel/start/switch/gui/status/stop）；桌面菜单三项。
- Guvcview 限制（实测）：v4l2loopback 的 capture 格式由生产者决定（consumer `S_FMT` 被回显），
  Guvcview 的分辨率列表因此只有当前生产格式；前后摄又不能同时出流，所以切换必须在
  后端完成（脚本/面板做这件事），不能在 Guvcview 里直接切。
- 分辨率选择：`--size` 自动对齐真实 sensor 模式；`--fps` 低于模式帧率时丢帧。
  实测：后摄 2104x1560@60 模式以 `--fps 15` 输出，60 帧耗时 4.09s（≈14.7fps）；`--fps 30`
  60 帧耗时 2.15s（≈28fps）；前摄 1640x1232@30 60 帧 2.14s。
- 自动对焦：后摄 CN3927V VCM，`elish-camera focus --camera rear` 一次扫焦（粗 9 点 + 精 5 点，
  整帧 Laplacian 锐度指标），本场景粗扫 639 → 精调 679；AF 前后全帧锐度 13.77→14.56、
  文字区域 15.82→17.07。前摄无对焦硬件。
- VCM 需要 video 组可写 sysfs（`/etc/udev/rules.d/99-elish-vcm.rules`），否则普通用户写入静默失败。

## 8. 双设备自动路由（autoserve，2026-11-12 晚）

- 后端：`elish-camera autoserve`（`elish-camera-ctl` 常驻管理，systemd 用户无关）。
- 两个设备常驻并有格式：`/dev/video2` 前摄 3280×2464、`/dev/video20` 后摄 3120×4208。
- 自动路由实测：ffmpeg 打开 `/dev/video2` → 后端切前摄、3 帧全分辨率成功；关闭后
  后端自动停流；再打开 `/dev/video20` → 自动切后摄、3 帧 3120×4208 成功；空闲后停流。
- 切换延迟：检测 0.4s（去抖 2 次≈0.8s）+ 相机重配约 1s；消费者会先等待约 1–2s 再出画面。
- 仍然不能双路同时出流（硬件/camss 限制），两个设备同时被打开时只有最后打开的那个有画面。

## 9. PipeWire / WirePlumber / portal 端到端验证（2026-11-12 晚）

- 两个相机都由 WirePlumber 的 v4l2 monitor 发布为 PipeWire 节点：
  - id 55 `Elish Front Camera (V4L2)`（`object.path = v4l2:/dev/video2`, `media.role = Camera`）
  - id 57 `Elish Rear Camera (V4L2)`（`v4l2:/dev/video20`）
- portal：`org.freedesktop.portal.Camera.IsCameraPresent = true`；
  `OpenPipeWireRemote` 返回可用 PipeWire fd。
- **端到端实测**（用 portal fd + 自写 PipeWire 客户端，等价于 Flatpak/portal App 的取流方式）：
  - portal fd + target node 57（后摄）→ 捕获 8 帧，格式 3120×4208；
  - portal fd + target node 55（前摄）→ 捕获 8 帧，格式 3280×2464；
  - autoserve 日志同步显示 `users rear=1 → switching to rear`、`users front=1 → switching to front`。
- 即 **PipeWire 路径与 V4L2 路径是同一套路由逻辑**：App 使用哪个相机节点/设备，
  底层就打开对应的 `/dev/video20` / `/dev/video2`，autoserve 检测后切传感器。
- 直接（非 portal）PipeWire 访问受限：`gst-launch pipewiresrc target-object=...` 报
  `target not found`；直连节点时 stream 停在 paused。沙箱 App 应走 portal（已验证），
  原生 App 建议直接读 V4L2。
- 与 V4L2 相同的限制：一次只能一路相机出画面；切换约 1–2 s；分辨率/帧率仍由生产者决定。

## 10. Flatpak 相机 App 验收（Snapshot 51，2026-11-12 晚，已打通）

根因与解法：

- v4l2loopback 被 WirePlumber 发布成 PipeWire 节点后，**节点不暴露 `SPA_PARAM_EnumFormat`**
  （`pw-cli enum-params <id> 3` 为空），GStreamer 的 pipewire device provider 因此得到**空 caps**；
  GNOME Snapshot（aperture）用
  `is_camera()` = `device.has_classes("Video/Source") && caps 能匹配 video/x-raw(0..30fps)`
  过滤设备，空 caps 的 v4l2 节点全部被丢弃 → UI 显示“未找到摄像头”。
- 解法：`elish-camera` 现在**自己发布两个原生 PipeWire 相机节点**
  （`src/pwcam.c`）：
  - `elish-camera-front` / 描述 `Elish Front Camera`，NV12 3280×2464@30
  - `elish-camera-rear` / 描述 `Elish Rear Camera`，NV12 3120×4208@30
  - 属性 `media.class=Video/Source`、`media.role=Camera`，**EnumFormat 完整**；
  - portal fd 中可见（`OpenPipeWireRemote` 后的 registry 里能枚举到）。
  - autoserve 通过 `elish_pwcam_active()` 检测哪个节点被消费，自动把传感器路由到对应相机。
- 实测结果：
  - Snapshot 启动 → 找到相机并显示**实时前摄画面**（3280×2464，经 portal fd + pipewiresrc）；
  - 切换 last-camera-id 为 `Elish Rear Camera` 后 → 显示后摄实时画面（3120×4208），
    autoserve 日志 `pipewire rear=1 → switching to rear`；
  - 原生 V4L2 路径（`/dev/video2`、`/dev/video20`）同时保持可用；
  - portal `IsCameraPresent=true`。
- 后摄自动对焦：切换到后摄时后端会在约 2 s 后自动扫焦（实测 focus 672 / score 286），
  之后每 20 s 复查一次；`elish-camera-ctl` 默认 `autofocus=1`。
- 备注：Snapshot 用 GSettings `last-camera-id` 记住上次相机；可设置默认：
  `flatpak run --command=gsettings org.gnome.Snapshot set org.gnome.Snapshot last-camera-id 'Elish Front Camera'`。

## 11. 方向自适应（2026-11-12 晚）

问题：竖屏（当前 Mutter transform=1）下前后摄画面都转了 90°。原因是我们原先把旋转硬编码为
横屏使用时的值（前 180°、后 270°）。

实现：`src/main.c` 新增 `display_transform()`（读 Mutter `DisplayConfig.GetCurrentState` 的
logical-monitor transform，回退 xrandr 几何）与 `default_rotation()`：

| 显示 transform | 前摄 | 后摄 |
|---|---|---|
| 0（横屏） | 180° | 180° |
| 1（竖屏，旋转 90°） | 90° | 90° |
| 2（横屏倒置） | 0° | 0° |
| 3（竖屏，旋转 270°） | 270° | 270° |

（实测两个模组相对设备的安装方向相同；旧文档里“后摄 270°”是按旧 helper 的口径写的，已修正。）

- `elish-camera photo/record/stream` 未显式给 `--rotate` 时也按此自动选择；
- `autoserve` 启动时按当前显示方向配置两个相机（pw 节点分辨率随之变为竖/横）；
- 手动覆盖：`elish-camera-ctl rotate front|rear 0|90|180|270|auto`（持久化在
  `~/.cache/elish-camera.conf`，重启后端生效）；
- 实测（transform=1，竖屏）：前后摄均 rot90（前 2464×3280、后 3120×4208），
  GNOME Snapshot 预览方向正确；后摄自动对焦正常（focus 352 / score 567.5）。
- 注意：运行中旋转屏幕后需要重新 `elish-camera-ctl start`（或 panel 重新应用）才会按新方向出流；
  运行中动态换方向需要重建 PipeWire 节点格式，暂未实现。

## 12. 前后摄镜像（2026-11-12 晚）

现象：Snapshot 里前后摄预览都是左右镜像，后摄镜像不可接受。

根因（读 Snapshot 自带的 aperture 源码 + 实测）：
- `aperture::Viewfinder` 的 `snapshot()` 只在 `is_front_camera == true` 时对预览做水平翻转；
- `is_front_camera = !matches!(camera.location(), CameraLocation::Back)`，
  `location()` 读的是 GstDevice 属性 **`api.libcamera.location`**（"front"/"back"/"external"），
  **缺省值是 Unknown，也按 front 处理**；
- PipeWire 设备提供器会把节点属性原样复制到 GstDevice 属性，但我们自己发布的
  `elish-camera-front/rear` 节点当时没有设置该属性 → Snapshot 把**两个相机都当成前置**，
  于是两个预览都被镜像。

解决：`elish_pwcam_create()` 增加 `location` 参数并发布
`api.libcamera.location = "front"`（前摄）/ `"back"`（后摄）。

实测：
- `RUST_LOG=debug` 下 aperture 日志确认：
  `Camera found: Elish Rear Camera ... "api.libcamera.location": (gchararray) "back"`，
  前摄为 `"front"`；
- 后摄预览不再镜像；前摄保持镜像（自拍习惯，符合需求）。

注意：该属性会随节点重新发布；修改后需要重启 Snapshot/Aperture 系 App 才会重新读取。

## 13. 平板模式 / 自动旋转 / 旋转重建（2026-11-12 晚，配置化）

现象与根因：
- 移除磁吸保护套后自动旋转失效：mutter 46 的自动旋转需要
  `panel_orientation_managed = clutter_seat_get_touch_mode() && has_accelerometer &&
  get_laptop_panel()`；`touch_mode` 由 `SW_TABLET_MODE` 开关决定，而本机霍尔
  传感器就是该开关（磁铁在位 = 1）。
- 用“USB 键盘是否存在”做动态判断不可行：pogo 键盘控制器 `3206:3ffc Xiaomi Pad`
  在开机 ~5s 就枚举，拔掉键盘保护套后 USB 设备仍然存在（dmesg 无 disconnect），
  因此旧逻辑总认为键盘在位 → 平板模式被强制关 → 自动旋转永远失效。

解决：`elish-tablet-mode` helper 发布虚拟 `SW_TABLET_MODE`，行为由
`/etc/elish-orientation.conf` 控制：

```
tablet_mode = on | off | auto     # on=总是平板（自动旋转开，默认）；auto=跟随磁铁
rebuild_mode = idle | live | off  # idle=空闲重建（默认，C）；live=立即重建+自动重开 Snapshot（A）
```

- helper 每秒读取配置，变化立即生效；只发布 `SW_TABLET_MODE`，**不碰 `SW_LID`**，
  且对 gpio-keys 只读不 grab，合盖休眠（logind `HandleLidSwitch=suspend`）不受影响。
- 后端每 2s 读一次配置与 Mutter transform：
  - `idle`：正在被 App 使用时先记 pending，所有消费者退出后重建
    （PipeWire 节点按新方向重建、v4l2loopback 重新 S_FMT）；
  - `live`：立即重建，若 Snapshot 正在用相机则 `flatpak kill` + 重开；
  - `off`：只记录不重建。
- 实测（fake-transform 测试）：
  `live` 使用中旋转 → `live rebuild ... rebuild complete (front 2464x3280 rot 90, ...)`；
  `idle` 使用中旋转 → `rebuild deferred until idle`，消费者退出后
  `idle, rebuilding ... rebuild complete (front 3280x2464 rot 180, ...)`。
- 配置工具：`elish-camera-ctl orientation [tablet on|off|auto] [rebuild idle|live|off]`
  或 `elish-camera-ctl panel` → “屏幕方向（自动旋转 / 旋转重建）”。

## 14. 霍尔开关实测：键盘/保护套状态无法区分（2026-11-12 晚）

用 `elish-hall-monitor`（新增工具，读 `/dev/input/event*` 的 EV_SW 能力与 EVIOCGSW，
记录 `SW_LID`/`SW_TABLET_MODE` 变化）让用户依次做三步实测：

| 状态 | gpio-keys SW 读数 |
|---|---|
| ① 键盘保护套连接 | `SW=0x0`（LID=0, TABLET=0） |
| ② 断开键盘（保护套仍吸附） | `SW=0x0` |
| ③ 完全取下保护套 | `SW=0x0` |

结论：
- **三种状态读数完全相同**，键盘插拔/保护套取下都**不会改变** `SW_LID` /
  `SW_TABLET_MODE`；gpio-keys 的两个开关只反映保护套的**物理位置**：
  - 磁铁折到平板背面（支架/平板模式）→ `SW_TABLET_MODE=1`（用户此前实测“磁铁放到背后
    自动旋转生效”即为该开关被触发）；
  - 保护套合上盖住屏幕 → `SW_LID=1`（合盖休眠，logind `HandleLidSwitch=suspend`）。
- pogo 键盘的 USB HID 是**开机即枚举、拔键盘也不消失**（`3206:3ffc`，dmesg 无 disconnect），
  所以用户态**没有可靠的“键盘是否连接”信号**，无法自动区分键盘套/普通保护套。
- 因此自动旋转采用**配置模式**（`/etc/elish-orientation.conf` 的 `tablet_mode=on|off|auto`）：
  默认 `on`（自动旋转可用），用键盘时可切到 `off`；`auto` 则完全跟随磁铁位置。
- 虚拟开关仅发布 `SW_TABLET_MODE`，不碰 `SW_LID`；helper 对真实 gpio-keys 只读不 grab，
  合盖休眠不受影响。

## 15. 到底有几颗霍尔传感器（Android vendor DTBO 取证 + 实测，2026-11-12 晚）

反编译 `dtbo_a` 分区（32 个 overlay）得到的关键节点：

| 位置 | 定义 | 用途 |
|---|---|---|
| `gpio_keys/hall_key`（多个 overlay） | `gpios = <... 0x0e 0x01>`，`linux,input-type=EV_SW`, code=SW_LID | 合盖霍尔（vendor 里叫 hall_key） |
| `pinctrl/hall`（dtbo_8 等） | `hall_intr3_active`=gpio136、`hall_intr4_active`=gpio138（input-enable + pull-up） | 键盘/保护套 MCU 的两路霍尔中断 |
| `xiaomi_keyboard_mcu`（dtbo_8） | gpio155 input + pull-up/down | 键盘 MCU 就绪/中断脚 |
| `xm,gamekey`（dtbo_28） | `hall_left`=gpio121、`hall_right`=gpio64；`key_left`=gpio157、`key_right`=gpio158 | 磁吸游戏肩键的霍尔 + 按键 |

mainline 6.18 DTB 只声明了 2 个开关：
- `switch-lid` → gpio110（active low）→ SW_LID
- `switch-tablet-mode` → gpio121（active low）→ SW_TABLET_MODE
（`xm,gamekey` 驱动 mainline 没有，gamekey 的两颗霍尔在 Linux 下目前无功能。）

实测（`elish-hall-monitor` + gpio debugfs 快照，用户依次操作）：

| 状态 | SW_LID | SW_TABLET_MODE | 其他 GPIO 变化 | 系统行为 |
|---|---|---|---|---|
| 键盘套连接、键盘在前 | 0 | 0 | — | 笔记本模式（无 OSK/自动旋转） |
| 键盘折到平板背面 | 0（瞬时误触发 1） | **1**（gpio121 拉低） | gpio66 high→low | 平板模式：自动旋转 + 软键盘 |
| 合盖 | **1**（触发 suspend） | — | gpio8/38/51/75 随电源状态变化 | 合盖休眠 |
| 断开键盘 / 取下保护套 | 0 | 0 | gpio66 变低 | 仍按笔记本模式处理 |

结论：
- 硬件上**至少 4~5 颗霍尔**（合盖 hall_key、cover hall_intr3、cover hall_intr4、gamekey hall_left、gamekey hall_right）；
- mainline 只把其中 2 颗接到 input 开关（合盖 + 平板模式）；
- **键盘是否连接无法从霍尔/USB 判断**（pogo 键盘 USB 常驻枚举）；
- 但“保护套折到背面”有明确开关（SW_TABLET_MODE=1），这正是“折到背后触发软键盘/自动旋转”的原因；
- 因此 `tablet_mode=auto` 复刻系统原生行为；想要“没保护套也旋转”用 `on`；用键盘时用 `off`。
  配置：`elish-camera-ctl orientation tablet on|off|auto`。
- suspend/resume 后 iio-sensor-proxy 加速计可能晚就绪，mutter 的 PanelOrientationManaged 可能保持
  false，需要重新 claim（可加 resume hook 重启传感器栈）。

## 16. 休眠唤醒后传感器恢复（2026-11-12 晚，v2）

问题：suspend/resume 后 SLPI 传感器栈可能晚就绪；并且 mutter 46 只在创建 iio D-Bus
proxy 时 `ClaimAccelerometer`，若 iio 先无加速计、传感器随后才出现，mutter **不会重新 claim**，
`PanelOrientationManaged` 一直 false（折叠背面也不自动旋转）。

v1 失败原因：sleep hook 用 `setsid ... &` 起的后台 worker 仍在 `systemd-sleep.service`
的 cgroup 里，sleep 单元结束时被 systemd 杀掉（journal 只有 "starting iio"、没有后续）。

v2 实现：
- systemd sleep hook `/usr/lib/systemd/system-sleep/elish-sensors-resume`（post）
  `systemctl --no-block start elish-sensors-resume.service`（独立 cgroup，不会被杀）；
- `/etc/systemd/system/elish-sensors-resume.service`（oneshot）→
  `/usr/local/sbin/elish-sensors-resume-worker`：
  1) 等加速计出现（最多 60s；iio 不活跃则先 start）；
  2) 60s 仍无则拉起 `slpi-sensors.service` 再等 40s；
  3) 加速计就绪后**再重启一次 iio**，让 mutter 重建 proxy 并 claim；
  4) 再等 30s 确认 `HasAccelerometer`，全程 `logger -t elish-sensors-resume`。
- 实测：手动执行 worker → `accelerometer is back` → `restarting iio …` →
  `done: accelerometer ready and re-claimed`；强制平板模式下 iio 重启前后
  `PanelOrientationManaged` 均为 true（mutter 重新 claim 成功），切回 auto 后按实测霍尔为 false。

## 17. 键盘输入卡顿/吞键排查（同日）

现象：打字偶尔卡顿、吞输入；用户怀疑“键盘输入状态检测”。

事实：
- `elish-tablet-mode` 当前版本**只读 gpio-keys 的 SW_LID/SW_TABLET_MODE**并发布虚拟开关，
  从不打开键盘/鼠标 event 节点、不 EVIOCGRAB、CPU 占用≈0；
- 早期版本的“按 USB 设备判断键盘”逻辑已删除；
- dmesg 显示唤醒后 pogo 键盘 USB **每 ~7 秒断开重连一次**（`usb 1-1` device 27→30，
  每次 disconnect 后重新枚举 HID），期间丢键最可能来自 USB 重连，而非软件读取。
- 措施：新增 udev 规则 `99-elish-keyboard-usb.rules`（`power/control=on`，禁用该键盘 USB
  自动挂起）；同时把空闲时 `autoserve` 的 /proc 扫描从 400ms 放缓到 1s、方向轮询 2s→4s，
  降低后台 CPU 干扰。
- 若仍复现，用 `dmesg | grep 'usb 1-1'` 确认是否仍在重连（重连=硬件 pogo 接触/供电问题）。

## 18. 重启验收与相机后端自启动（2026-11-12 晚）

重启后实测（uptime 103s）：
- USB pogo 键盘：`journalctl -k -b | grep -c 'usb 1-1'` = **1**（只有开机枚举一次），
  不再每 7 秒断开重连；键盘 `event6/event8` 正常存在 → 之前的循环是 suspend 后
  USB/键盘 MCU 状态异常，重启已恢复。
- `elish-tablet-mode` active、`iio-sensor-proxy` active、`HasAccelerometer=true`；
  当前未折叠所以 `PanelOrientationManaged=false`（符合 auto 模式）。
- 相机后端原本不会自启动，现新增：
  - `/usr/local/bin/elish-camera-autostart`：等 PipeWire socket 出现后再
    `elish-camera-ctl start`；
  - `/etc/xdg/autostart/elish-camera-autoserve.desktop`：桌面登录自动执行；
  - 实测手动执行后后端启动、`/dev/video2/20` 就绪、两个 PipeWire 相机节点已发布。
- 诊断：`/usr/local/bin/elish-check-usb` 输出 USB 枚举次数、键盘设备、平板模式。
- 实验性的键盘 USB udev 规则（power/control=on）已从系统和安装包中移除（未解决问题）。
