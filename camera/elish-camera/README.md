# elish-camera — 前后摄全分辨率 GPU 拍摄/录像管线

Xiaomi Pad 5 Pro（SM8250 / elish）在 mainline Armbian 上，把前后摄以**全分辨率、约 30 fps**
的彩色画面采集出来，用 **GPU（Adreno 650 / FD650，EGL-GLES）去马赛克**、
**Venus（V4L2 M2M）硬件编码**录像，并通过**标准 V4L2（v4l2loopback）+ PipeWire 相机节点**
暴露给第三方 App。

当前状态（2026-11-12 晚，已实机验收）：

| 能力 | 状态 |
|---|---|
| 前摄 3280×2464 彩色拍照 / HEVC 录像 | ✅ 30 fps |
| 后摄 4208×3120 彩色拍照 / HEVC 录像 | ✅ 30 fps（重负载下 25–29 fps） |
| GPU debayer（含回读） | ✅ 前 11–12 ms / 后 19–20 ms 每帧 |
| Venus 硬件编码 | ✅ 两分辨率 HEVC/H.264 均 >90 fps 吞吐 |
| V4L2 虚拟相机 `/dev/video2`（前）/ `/dev/video20`（后） | ✅ NV12 全分辨率 30 fps |
| PipeWire `media.role=Camera` 节点 / portal Camera | ✅ 节点自动出现、portal `IsCameraPresent=true` |
| 前后摄同时出流 | ❌ 硬件/camss 限制，一次一路 |

详见 `docs/architecture.md`、`docs/measurements/measurements-integrated.md`、`docs/interface.md`。

## 构建

```bash
make            # build/elish-camera + tools
```

依赖：`gcc`、`pkg-config`、`egl`/`glesv2`/`libdrm`/`gbm` 开发头、`zlib`、`ffmpeg`（MP4 封装）。
不需要 GStreamer（仅作为第三方消费者验证时使用）。

## 用法

```bash
# 相机信息
./build/elish-camera list

# 全分辨率拍照（默认前摄 180° / 后摄 270° 方向修正）
./build/elish-camera photo --camera front -o front.png
./build/elish-camera photo --camera rear  -o rear.png

# 录像（HEVC 20 Mbps，默认 30 fps；-d 0 表示一直录到 Ctrl-C）
./build/elish-camera record --camera front -o front.mp4 -d 10
./build/elish-camera record --camera rear  -o rear.mp4  -d 10 --codec h264 --bitrate 25000

# 录像同时作为虚拟相机输出（/dev/video2 前摄 / /dev/video20 后摄）
./build/elish-camera record --camera front -o front.mp4 --loopback /dev/video2

# 常驻虚拟相机，给第三方 App（Guvcview/ffmpeg/GStreamer/PipeWire/portal）
./build/elish-camera serve --camera front          # -> /dev/video2
./build/elish-camera serve --camera rear           # -> /dev/video20

# 指定分辨率/帧率/旋转/对焦
./build/elish-camera photo --camera rear --size 2104x1560 --rotate 90 --focus 640
./build/elish-camera record --camera front --size 1640x1232 --fps 30 --rotate 180
```

第三方 App 侧：

```bash
ffmpeg -f v4l2 -input_format nv12 -video_size 3280x2464 -i /dev/video2 -frames:v 10 out-%02d.png
gst-launch-1.0 -q v4l2src device=/dev/video2 num-buffers=30 ! fakesink
guvcview --device=/dev/video2
gst-launch-1.0 -q pipewiresrc target-object=v4l2_input._sys_devices_virtual_video4linux_video2 num-buffers=30 ! fakesink
```

## 相机控制面板 / 切换 / 对焦（2026-11-12 晚新增）

两个相机是**两个独立的标准 V4L2 设备**，并且后端会自动路由：

- `/dev/video2`  = 前摄（默认 3280×2464@30）
- `/dev/video20` = 后摄（默认 4208×3120@30）

`elish-camera autoserve`（由 `elish-camera-ctl` 管理）常驻运行，**哪个设备被 App 打开，
后端就把当前唯一可用的传感器切到那个设备**。所以在 Guvcview 的 **Device 下拉里直接选
`/dev/video20` 就会自动切到后摄**（约 1–2 秒出画面），不需要再手动切脚本/面板；用户
关掉设备后后端自动停止相机（省电）。

注意硬件事实：两个传感器**不能同时出流**（实测：一个开始出流，另一个立即停），
所以两个设备不能同时有画面；同一时刻只有一个 App 能取流。

分辨率/帧率：v4l2loopback 的格式由生产者决定（consumer 的 S_FMT 只会被回显），
Guvcview 里不能改；用面板或命令改后端配置（会自动重开 Guvcview）：

```bash
elish-camera-ctl panel                      # 图形面板：相机/分辨率/帧率/自动对焦
elish-camera-ctl start --front-size 1640x1232 --front-fps 30 \
                       --rear-size 2104x1560 --rear-fps 60 --autofocus
elish-camera-ctl gui rear                   # 在 /dev/video20 上打开 Guvcview
elish-camera-ctl status / stop
```

桌面菜单：**Elish Camera**（前摄）、**Elish Camera (Rear)**（后摄）、
**Elish Camera Settings**（分辨率/帧率/对焦面板）。

自动对焦（仅后摄，前摄 OV8856 是固定焦点镜头）：

```bash
elish-camera focus --camera rear            # 一次扫焦，输出最佳位置并写入 VCM
elish-camera photo --camera rear --autofocus
elish-camera-ctl start --camera rear --size 2104x1560 --autofocus   # 预览时自动对焦
```

注意：VCM 的 sysfs 需要 video 组可写，安装包里的
`/etc/udev/rules.d/99-elish-vcm.rules` 已处理；若手工部署请执行
`sudo chgrp video /sys/bus/i2c/devices/20-000c/focus && sudo chmod 664 .../focus`。

分辨率/帧率（会自动对齐到真实 sensor 模式）：

| 相机 | 模式 |
|---|---|
| 前摄 | 3280x2464@30、1640x1232@30 |
| 后摄 | 4208x3120@30、4160x3120@30、4160x2340@30、2104x1560@60、2080x1170@60、1364x768@120 |

`--fps` 低于模式帧率时会自动丢帧（例如后摄 2104x1560@60 模式录 30fps 视频）。

## 目录结构

```
include/elish_camera.h   公共 API（改前先看 docs/architecture.md）
src/capture.c            CAMSS raw 采集 + media graph + 传感器控制
src/gpu_debayer.c        GPU debayer / NV12 / RGB888 / 场景统计
src/venus_enc.c          Venus 硬件 H.264/HEVC 编码 + MP4
src/loopback.c           v4l2loopback 虚拟相机生产者
src/main.c               CLI + AE/AWB + PNG + 线程化推流
tools/                   各模块独立测试（debayer_test/enc_test/loopback_test/capture_test）
docs/                    架构事实、measurements/（历史测量）、接口矩阵、evidence/（反编译解析）
testdata/                少量 raw 测试帧（**不随包**；需要时自采，见 docs/DEVICE-DATA-EXTRACTION.md）
```

## Flatpak / portal App（已打通）

`elish-camera autoserve` 除了两个 V4L2 虚拟相机（`/dev/video2`、`/dev/video20`），还会发布
**两个原生 PipeWire 相机节点**（`Elish Front Camera` / `Elish Rear Camera`，NV12 全分辨率，
EnumFormat 完整、`media.role=Camera`）。Flatpak/portal App（已实测 GNOME Snapshot 51）能枚举、
预览、切换前后摄；切换/拍照时 autoserve 自动路由传感器。

> 为什么用自研 PipeWire 节点：v4l2loopback 经 WirePlumber 发布的节点不暴露 EnumFormat（空 caps），
> GStreamer/aperture 系的 App 会把它们过滤掉（“未找到摄像头”）。原生节点提供完整 caps 解决此问题。

Snapshot 默认相机由 GSettings 记住，可设为前摄：
`flatpak run --command=gsettings org.gnome.Snapshot set org.gnome.Snapshot last-camera-id 'Elish Front Camera'`。

## 屏幕方向 / 自动旋转（可配置）

GNOME 的自动旋转只在 mutter 认为设备处于**平板模式**时启用
（`PanelOrientationManaged = touch_mode && has_accelerometer && laptop_panel`）。
本机磁吸保护套的霍尔传感器被映射成 `SW_TABLET_MODE`，而 pogo 键盘的 USB HID
**开机就一直枚举、拔掉键盘也不会消失**，所以用户态无法可靠判断键盘是否在连接。

因此提供可配置的 `elish-tablet-mode` 虚拟开关 + 配置工具：

```bash
elish-camera-ctl orientation                 # 图形设置面板（zenity）
elish-camera-ctl orientation tablet on       # 总是平板模式 -> 自动旋转开（默认）
elish-camera-ctl orientation tablet off      # 关闭平板模式 -> 自动旋转关
elish-camera-ctl orientation tablet auto     # 跟随磁铁/保护套霍尔开关
elish-camera-ctl orientation rebuild idle    # 旋转时：空闲再重建相机流（默认，C 方案）
elish-camera-ctl orientation rebuild live    # 旋转时：立即重建并自动重开 Snapshot（A 方案）
elish-camera-ctl orientation rebuild off     # 旋转时：不重建（方向保持旧值）
```

- 配置写在 `/etc/elish-orientation.conf`，helper/后端会自动读取（1–2 秒内生效）；
- 虚拟开关**只发布 `SW_TABLET_MODE`，不碰 `SW_LID`**；合盖休眠仍由真实霍尔
  `SW_LID` → logind 处理（helper 只读监听 gpio-keys，不 grab 设备）；
- `live` 模式会 `flatpak kill` 再重开 Snapshot（约 2–3 秒黑屏）；
- `idle` 模式不打断正在预览的 App，相机空闲后自动按新方向重建，下次打开即正确。

## 屏幕方向 / 画面旋转（自动）

后端启动时读取 GNOME Mutter 的显示方向（`DisplayConfig` transform；无桌面会话时退回 xrandr
几何），自动给前后摄选旋转角，使**竖屏/横屏使用都是正的**：

- 横屏（transform 0）：前后摄都是 **180°**
- 竖屏（transform 1/3）：整体再加 270°/90°（当前竖屏实测：前后摄都是 **90°**）
- 横屏倒置（transform 2）：前后摄都是 0°
- 旋转屏幕后重新执行 `elish-camera-ctl start` 即可重新按当前方向配置

如果个别方向仍不对（相机模组校准差异），可手动指定并持久化：

```bash
elish-camera-ctl rotate front 90     # 立即重启后端并重开对应相机（front|rear, 0/90/180/270/auto）
elish-camera-ctl rotate rear 90
```

## 硬件访问与注意事项

- 实机访问加 flock（见 `docs/architecture.md` §8）。
- 不要改 DTB/内核模块、不要重启、不要装 `libspa-0.2-libcamera`（会破坏本管线）。
- 前后摄不能同时出流；切换相机需重新 `serve`。
- 前摄必须 `vertical_flip=0`（vflip=1 时全分辨率无帧，已在代码中固定）。
- 本机曾因磁盘满死机：测量/测试不要生成大 PPM/NV12 文件。

## 模块测试工具

```bash
./build/debayer_test testdata/front-3280x2464-sgrbg10.raw --input 3280x2464 --stride 4112 \
    --bayer bggr --rotate 180 --out-ppm /tmp/f.ppm --frames 20
./build/enc_test --size 4208x3120 --frames 90 --synthetic black --out /tmp/e.h265 --mux /tmp/e.mp4
./build/loopback_test --dev /dev/video2 --size 1280x720 --fps 30 --seconds 10 --pattern bars
./build/capture_test --camera front --frames 30 --out /tmp/cap
```
