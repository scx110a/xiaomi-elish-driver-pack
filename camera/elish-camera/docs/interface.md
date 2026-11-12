> **2026-11-12 晚间更新**：当前系统已把 v4l2loopback 配成两个持久设备
> （`/etc/modprobe.d/elish-vcam.modprobe.conf`）：`/dev/video2` = Elish Front Camera、
> `/dev/video20` = Elish Rear Camera（旧版只有 /dev/video2）。`elish-camera serve --camera front|rear`
> 常驻推流；两个设备都由 WirePlumber 自动发布为 `media.role=Camera` 节点。
> 前后摄不能同时出流（camss 限制），一次用一路。端到端验收见 `docs/measurements-integrated.md`。

# 通用相机接口：v4l2loopback 生产者 + PipeWire / portal（2026-11-12 实测）

> 本文所有结论均为本机实测（Xiaomi Pad 5 Pro / SM8250 / Armbian noble / kernel 6.18.44，
> PipeWire 1.0.5 + WirePlumber 0.4.17 + xdg-desktop-portal 1.18.4 + portal-gnome 46.2）。
> 未实测的条目会明确标注「未实测 / 待验证」，不要当事实用。
>
> 负责代码：`src/loopback.c`、`tools/loopback_test.c`、`tools/pw_camera_node.c`（可选原型）。
> 不要修改 `include/elish_camera.h`、`Makefile`、其他模块的文件。

## 0. 结论（TL;DR）

1. **给第三方 App 最可靠的通用接口是 v4l2loopback 虚拟相机 `/dev/video2`（NV12）**。
   原生 V4L2 应用（ffmpeg、GStreamer v4l2src、Guvcview）全部实测通过，
   1080p / 3280x2464 / 4208x3120 都能稳定 **30 fps** 推送与消费。
2. **Flatpak / portal 应用路线在本机已经打通，不需要升级 WirePlumber，也不需要 libcamera**：
   WirePlumber **0.4.17 自带 v4l2 monitor**，会把 `/dev/video2` 自动发布成
   `media.class=Video/Source`、`media.role=Camera` 的 PipeWire 节点（本机节点 id 57）；
   xdg-desktop-portal 的 Camera portal `IsCameraPresent=true`，
   `OpenPipeWireRemote` 能拿到 PipeWire fd，`gst pipewiresrc fd=<fd>` 实测取到
   NV12 1920x1080@30fps 帧（150 帧 / 5.12 s）。架构文档 §5.2 里
   「WirePlumber 0.4 没有 camera monitor」的说法**不准确**，真正缺的是
   `libspa-0.2-libcamera`（被禁止安装），它只影响「把 raw Bayer sensor 直接暴露成
   libcamera 相机」，不影响我们的 NV12 loopback 方案。
3. **自研 PipeWire 相机节点可行**：`tools/pw_camera_node.c` 原型成功发布
   `media.role=Camera` 的 NV12 1080p30 节点，`pipewiresrc` 可直接拉流，
   可作为「方案 B」（不依赖 v4l2loopback / WirePlumber monitor）。
4. **关键限制**：消费者必须在生产者**开始推流之后**再打开设备。
   v4l2loopback 在 `ready_for_capture=0`（还没有 writer STREAMON）时，
   CAPTURE 端 `VIDIOC_G_FMT/S_FMT` 返回 `EINVAL/EBUSY`；ffmpeg 会直接报
   `ioctl(VIDIOC_G_FMT): Invalid argument` 退出（实测 rc=234）。
   所以正式服务应**常驻推流**（例如空帧/黑帧保底），而不是等 App 打开再启动。

## 1. 本机环境事实

| 项 | 值 |
|---|---|
| 内核 | 6.18.44（v4l2loopback driver version 6.18.44） |
| v4l2loopback | 0.12.7，`/dev/video2`，card `"Elish Front Camera"` |
| 模块参数 | `video_nr=2 max_buffers=16 max_openers=10 max_width=8192 max_height=8192 exclusive_caps=N` |
| PipeWire | 1.0.5（`wireplumber` 0.4.17） |
| xdg-desktop-portal | 1.18.4-1ubuntu2.24.04.3，Camera portal version 1 |
| portal 后端 | xdg-desktop-portal-gnome 46.2-0ubuntu1（实现 `org.freedesktop.impl.portal.Access`） |
| SPA 插件 | 有 `v4l2`，**没有** `libcamera`（`libspa-0.2-libcamera` 未安装，且要求不要装） |
| Flatpak | 1.14.6；已装 App 里没有相机 App（无 cheese 可执行文件、无 gnome-snapshot） |
| 会话 | GNOME Wayland（`WAYLAND_DISPLAY=wayland-0`，`XDG_RUNTIME_DIR=/run/user/1000`） |

设备能力（`v4l2-ctl -d /dev/video2 --all`）：`Video Capture` + `Video Output` +
`Read/Write` + `Streaming`（`announce_all_caps=true`，即 `exclusive_caps=N`）。

## 2. v4l2loopback 生产者语义（源码 + 实测）

来自 `/usr/src/v4l2loopback-0.12.7/v4l2loopback.c`，决定了 `src/loopback.c` 的实现：

1. **格式由生产者决定**：producer 对 `VIDIOC_S_FMT(OUTPUT)` 设 NV12 WxH 后，
   CAPTURE 消费者通过 `VIDIOC_G_FMT(CAPTURE)` 看到的就是这个格式
   （`vidioc_try_fmt_out` 直接写 `dev->pix_format`）。
2. **NV12 是 PLANAR 注册**：`bytesperline == width`，`sizeimage == width*height*3/2`，
   **没有 stride 补齐**。所以设备侧 buffer 是紧密的 Y 平面 + UV 平面；
   如果调用方 NV12 的 stride 大于 width，`elish_lb_push_nv12()` 会按行重排。
3. **生产者必须先启动**：`ready_for_capture` 只有在 writer `STREAMON(OUTPUT)`
   或第一次 `write()` 后才置 1；在此之前 CAPTURE 端 G_FMT 返回 `-EINVAL`、
   S_FMT 返回 `-EBUSY`（见 §4.4 实测）。
4. **OUTPUT 队列没有「空闲 buffer」概念**：`VIDIOC_QBUF(OUTPUT)` 只更新元数据
   （帧数据已经在 mmap 的 kernel image 里）并把该 buffer 标成最新帧；
   `VIDIOC_DQBUF(OUTPUT)` 返回的是「最后写入的那个 buffer」，不是标准意义上的
   出队。因此本实现的 push 循环是 **round-robin 填 4 个 buffer + QBUF**，
   不需要（也不应该依赖）OUTPUT DQBUF。
5. **多消费者**：每个 CAPTURE `open()` + STREAMON 的进程都能读到同一路帧，
   由内核分发；上限 `max_openers=10`。生产者不需要感知消费者数量。
6. **帧率**：`VIDIOC_S_PARM(OUTPUT)` 可设置 `timeperframe`，消费者 `G_PARM`
   能看到（ffmpeg 实测显示 30 fps）。但 v4l2loopback 的 `capture_param.capability=0`
   （没有 `V4L2_CAP_TIMEPERFRAME` 标志），部分 App（guvcview）会认为不能改 fps，
   实测它请求 25fps 失败后仍按我们推送的节奏取帧。
7. **设备名**由模块参数 `card_label` 在 modprobe 时决定，**运行中改不了**
   （`VIDIOC_S_INPUT` 只切 input，不设备名）。多相机需要每路一个 loopback 设备，
   要 `rmmod/modprobe v4l2loopback video_nr=2,3 card_label="Elish Front","Elish Rear" ...`，
   属于需 Lead 批准的操作；本次未做。替代方案见 §6.4。
8. 生产者关闭后格式**不会立刻复位**（实测残留 4208x3120 NV12 格式），
   下一个生产者仍应自己 S_FMT；消费者不要假设默认格式。

## 3. 生产者实现 `src/loopback.c`

公共 API 与 `include/elish_camera.h` 完全一致：

```c
elish_loopback *elish_lb_open(const char *dev, int width, int height, int fps);
int  elish_lb_push_nv12(elish_loopback *lb, const uint8_t *nv12, int stride);
void elish_lb_close(elish_loopback *lb);
```

`elish_lb_open()` 流程：
`open(O_RDWR)` → `QUERYCAP` 校验 OUTPUT+STREAMING → `S_FMT(OUTPUT, NV12 WxH)` →
`G_FMT(OUTPUT)` 取真实 `bytesperline/sizeimage` → `S_PARM(OUTPUT)` 设置 fps →
`REQBUFS(OUTPUT, MMAP, 4)` → `QUERYBUF` + `mmap` 4 个 buffer → `STREAMON(OUTPUT)` →
先用 0 帧（黑）填满 4 个 buffer（消费者一打开就能拿到第一帧）。

`elish_lb_push_nv12()` 流程：按 CLOCK_MONOTONIC 绝对时间戳节拍 sleep → 选
round-robin buffer → 拷贝/按行重排 NV12 → 填 `bytesused = payload` → `QBUF`。
- 落后超过一帧时**重新对齐而不会爆发追帧**（计入 pacing drop），长跑稳定；
- `EINTR` 自动重试；`EAGAIN/EBUSY` 会 1ms 重试最多 ~1s（v4l2loopback 实际不会返回，
  为通用性保留）；`EINVAL/ENODEV` 视为致命错误返回 -1；
- 返回 0 表示该帧已入队。

`elish_lb_close()`：`STREAMOFF(OUTPUT)` → `munmap` → `close`，并打印累计帧数/丢帧数。

参数不够时（例如超过 `max_width/max_height` 或设备已被其他生产者占据），
`elish_lb_open()` 会打印实际拿到的格式，并提示需要更大的模块参数/去掉占用者，
**不会自行 rmmod 或改模块参数**。

## 4. 实测：生产者 + 消费者矩阵

所有 `/dev/video2` 访问都串行在 `flock -w 60 elish-camera/.locks/loopback.lock` 下。
生产者测试工具：

```bash
make                                              # 全绿（所有模块 + tools）
flock -w 60 .locks/loopback.lock -c \
  './build/loopback_test --dev /dev/video2 --size 1920x1080 --fps 30 --seconds 10'
```

### 4.1 生产者实际 fps（NV12，含动画测试图，0 pacing drop）

| 分辨率 | 时长 | 帧数 | 实测 fps | 备注 |
|---|---|---|---|---|
| 1920x1080 | 30.000 s | 901 | **30.03** | 长时间运行无 EINVAL/ENOSPC |
| 3280x2464 | 9.537 s | 287 | **30.09** | 前摄全分辨率 |
| 4208x3120 | 8.102 s | 244 | **30.11** | 后摄全分辨率 |
| 1920x1080（3 消费者同时） | 25.002 s | 751 | **30.04** | ffmpeg+gst+guvcview 同时读 |

payload 带宽：4208x3120 NV12 = 19,693,440 B/帧 ≈ **591 MB/s @30fps**，本机无压力。

### 4.2 ffmpeg（原生 V4L2）

```bash
# 1080p，5 秒
ffmpeg -y -f v4l2 -i /dev/video2 -t 5 -f rawvideo -pix_fmt nv12 /tmp/cap.nv12
# 输出 150 帧，文件 466,560,000 B = 150 × 3,110,400 B（尺寸完全正确）
# stderr: "Video: rawvideo (NV12 / 0x3231564E), nv12, 1920x1080, 30 fps"
```

| 分辨率 | 命令 | 结果 |
|---|---|---|
| 1920x1080 | `-t 5 -f rawvideo` | rc=0，150 帧，文件大小精确匹配 |
| 3280x2464 | `-t 5 -f null -` | rc=0，150 帧（5 s） |
| 3280x2464 | `-frames:v 20 -f rawvideo` | rc=0，242,457,600 B 精确匹配 |
| 4208x3120 | `-t 5 -f null -` | rc=0，151 帧（5 s） |
| 4208x3120 | `-frames:v 10 -f rawvideo` | rc=0，196,934,400 B 精确匹配 |

> 旧命令 `-c:v rawvideo -pix_fmt nv12 file.nv12` 会因缺少 `-f rawvideo` 报
> "Unable to choose an output format"；那是 ffmpeg 输出封装问题，不是相机问题。

### 4.3 GStreamer v4l2src

```bash
gst-launch-1.0 -q v4l2src device=/dev/video2 num-buffers=120 \
  ! video/x-raw,format=NV12,width=1920,height=1080 ! fakesink      # rc=0

gst-launch-1.0 -q v4l2src device=/dev/video2 num-buffers=60 \
  ! video/x-raw,format=NV12,width=3280,height=2464 ! fakesink      # rc=0

gst-launch-1.0 -q v4l2src device=/dev/video2 num-buffers=30 \
  ! video/x-raw,format=NV12,width=4208,height=3120 ! fakesink      # rc=0
```

### 4.4 Guvcview（原生 V4L2 GUI App，Wayland）

```bash
XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 \
DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus GDK_BACKEND=wayland \
timeout -s KILL 10 guvcview --device=/dev/video2 --verbosity=3
```

实测（日志摘录）：

```
V4L2_CORE: video device: /dev/video2
{ pixelformat = 'NV12', description = 'Y/UV 4:2:0' }   # 枚举到的就是我们的格式
V4L2_CORE: checking format: NV12
V4L2_CORE: allocating frame buffers
V4L2_CORE: (VIDIOC_STREAMON) stream_status = STRM_OK
V4L2_CORE: process frame queue index 0
GUVCVIEW: (GUI) Screen resolution is (2560 x 1600)
```

结论：**Guvcview 能正常打开并显示我们的 NV12 相机**（GUI 窗口已创建、持续取帧；
本次 10 s 后由 timeout 结束）。
注意两点：
- guvcview 会尝试把 fps 改成 25（`fps configured to 1/25`），
  v4l2loopback 不报 `V4L2_CAP_TIMEPERFRAME` 能力，它最终仍按 30fps 取帧；
- Wayland 下 guvcview 对 `SIGTERM` 可能无响应（`timeout 10` 杀不掉），
  自动化时用 `timeout -s KILL 10`（实测 rc=137）。

### 4.5 PipeWire 消费者（本机自动出现相机节点）

无需任何系统改动：只要 `/dev/video2` 存在，WirePlumber 0.4.17 的 v4l2 monitor
就发布节点（实测在**没有生产者运行时**也一直在）：

```bash
$ wpctl status | sed -n '/^Video/,/^Settings/p'
Video
 ├─ Devices:
 │      55. Elish Front Camera                  [v4l2]
 ...
 ├─ Sources:
 │  *   57. Elish Front Camera (V4L2)

$ pw-dump | jq '...'   # 节点 57 关键属性：
   api.v4l2.path        = /dev/video2
   factory.name         = api.v4l2.source
   media.class          = Video/Source
   media.role           = Camera
   node.name            = v4l2_input._sys_devices_virtual_video4linux_video2
   node.description     = Elish Front Camera (V4L2)
   priority.session     = 980
```

生产者运行时，用 `pipewiresrc` 拉流（`target-object` 用节点 `object.serial`=58）：

```bash
gst-launch-1.0 -v pipewiresrc target-object=58 num-buffers=150 \
  ! video/x-raw,format=NV12,width=1920,height=1080 ! fakesink
```

实测 caps 协商为 `video/x-raw, format=NV12, width=1920, height=1080, framerate=30/1`，
150 帧约 5.0 s、rc=0（EOS）。全分辨率同样可用：

| 分辨率 | pipewiresrc 结果 |
|---|---|
| 1920x1080 | NV12 30fps，150 帧 / 5.0 s，rc=0 |
| 3280x2464 | NV12 30fps，60 帧 / 2.0 s，rc=0 |
| 4208x3120 | NV12 30fps，30 帧 / 0.99 s，rc=0 |

> `target-object` 用 **object.serial 数字**（58）可用；用 `node.name` 字符串
> （含 `.`）在 GStreamer 1.0.5 的 pipewiresrc 里连接失败（rc=255），这是
> GStreamer 侧限制，不是节点问题。用 `pw-cli ls Node` / `pw-dump` 找 serial 即可。

### 4.6 Camera portal（Flatpak 应用实际走的路径）

本机 Camera portal（version 1，xdg-desktop-portal 1.18.4）接口：

```
IsCameraPresent(property b)      -> true          # 本机实测（无生产者时也为 true）
AccessCamera(a{sv})             -> o handle       # 权限请求
OpenPipeWireRemote(a{sv})       -> h fd           # 受限 PipeWire 连接
```

用 `python3-gi` 走完整链路（脚本只做测试，未入库），在生产者运行时实测：

```
IsCameraPresent = True
AccessCamera returned handle: /org/freedesktop/portal/desktop/request/1_140/t
Response code: 0 results: {}          # 主机（非沙箱）应用被直接放行
OpenPipeWireRemote returned fd: 8
gst pipewiresrc fd=8 num-buffers=150 ! video/x-raw,format=NV12 ! fakesink
  -> caps = video/x-raw, format=NV12, width=1920, height=1080, framerate=30/1
  -> rc=0, 5.12 s, 150 帧
```

也就是说：**portal → 受限 PipeWire fd → NV12 帧** 这条机制链在本机是通的。
第二次请求时 `AccessCamera` 的 `Response` 信号在 12 s 内没有到达
（权限对话框/请求处于 pending），但 `OpenPipeWireRemote` 仍返回了可用 fd，
`pipewiresrc` 也取到了帧——说明主机应用（无 app id）路径对首次授权比较宽松；
对这个行为不要依赖，Flatpak 应用必须按 §7 的权限流程处理。

### 4.7 多消费者并发

在**同一个 flock 持锁命令**里同时启动：

```bash
flock -w 60 .locks/loopback.lock -c '
  ./build/loopback_test --size 1920x1080 --fps 30 --seconds 25 &
  sleep 1.5
  ffmpeg -y -f v4l2 -i /dev/video2 -t 5 -f null - &
  gst-launch-1.0 -q v4l2src device=/dev/video2 num-buffers=150 ! fakesink &
  XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 timeout -s KILL 6 \
    guvcview --device=/dev/video2 &
  wait
'
```

实测：ffmpeg 147 帧/5 s，GStreamer rc=0，Guvcview 打开 NV12 成功；
生产者 25.002 s 推 751 帧 = **30.04 fps**，无丢帧、无错误。
v4l2loopback 的 `max_openers=10` 对我们够用。

### 4.8 从 testdata RAW10 生成 NV12（`--raw`）

```bash
flock -w 60 .locks/loopback.lock -c \
 './build/loopback_test --dev /dev/video2 \
    --raw testdata/front-3280x2464-sgrbg10.raw --size 3280x2464 --fps 30 --seconds 8'
```

- 自动从文件大小推断 raw stride（10,131,968 B / 2464 行 = **4112 B**，符合架构文档）；
- 按 packed MIPI RAW10 解包、GRBG 简单最近邻去马赛克 → NV12；
- 实测 ffmpeg 抓到 1 帧 12,122,880 B，画面内容正确（真实室内场景/人脸），
  但**上下颠倒、偏紫**：这是预期现象——本工具只做「数据通路验证」，
  正确的 180° 旋转与白平衡/色彩由 GPU debayer 模块（`elish_gpu_debayer_nv12()`）负责。
- 该模式还支持 `--bayer rggb|grbg|gbrg|bggr`、`--raw-size WxH`、`--raw-stride N`。

### 4.9 消费者先于生产者打开（限制复现）

```bash
# 没有生产者推流时：
ffmpeg -f v4l2 -i /dev/video2 -frames:v 1 -f null -
# [video4linux2,v4l2] ioctl(VIDIOC_G_FMT): Invalid argument
# Error opening input file /dev/video2.   (rc=234)
```

原因见 §2.3。**解决方式：让 elish-camera 的服务常驻推流**（哪怕先推黑帧），
而不是按需启动。这应在主程序 `main.c` 的服务模式里落实。

## 5. 自研 PipeWire 相机节点原型（方案 B，已验证）

`tools/pw_camera_node.c`：直接向 PipeWire 发布 NV12 源节点，属性如下（实测
`pw-dump` 输出）：

```
media.type     = Video
media.category = Capture
media.class    = Video/Source
media.role     = Camera
node.name      = elish-camera-pw
node.description = Elish Camera (PipeWire NV12 prototype)
priority.session = 1000
```

它用 `pw_stream`（`PW_DIRECTION_OUTPUT`、`PW_STREAM_FLAG_DRIVER | MAP_BUFFERS`）
按 fps 定时 `trigger_process`，在 `process` 回调里填 NV12 彩条 + 移动色块，
并通过 `SPA_PARAM_Buffers` 把 `stride=width`、`size=w*h*3/2` 传给下游。

依赖安装（本机已装，仅头文件，无服务重启）：
`sudo apt-get install libpipewire-0.3-dev`（会同时装 `libspa-0.2-dev`，1.0.5-1ubuntu3.3）。
仓库 `Makefile` 不是我们维护的，所以**不把该可选工具加入 `make`**，手动构建：

```bash
gcc -O2 -Wall -Wextra -std=c11 -D_GNU_SOURCE tools/pw_camera_node.c \
    -o build/pw_camera_node $(pkg-config --cflags --libs libpipewire-0.3)
```

运行与验证（无需 flock，它不碰 `/dev/video2`）：

```bash
build/pw_camera_node --width 1920 --height 1080 --fps 30 --seconds 0 &
pw-cli ls Node | grep -A8 elish-camera-pw
gst-launch-1.0 -v pipewiresrc target-object=elish-camera-pw num-buffers=90 \
  ! video/x-raw,format=NV12,width=1920,height=1080 ! fakesink
```

实测：节点出现在 `wpctl status` 的 `Video → Sources`（id 65，
`Elish Camera (PipeWire NV12 prototype)`）；`pipewiresrc` 协商
NV12 1920x1080 30fps，90 帧 / 2.97 s，rc=0；进程退出后节点消失。
因为 `media.role=Camera`，portal 的 `IsCameraPresent` 也会同样为 true。

注意事项：
- 方案 A（v4l2loopback + WirePlumber monitor）和方案 B **同时运行会出现两个相机节点**
  （实测 id 57 与 65 并存）。正式使用只启用一种。
- 该原型只发布 1920x1080（`--width/--height/--fps` 可调），不读真实视频源；
  接入真实管线时，在 `on_process` 里把 GPU debayer 输出的 NV12 拷进
  `buf->datas[0].data` 即可（注意 stride 与 `SPA_PARAM_Buffers` 一致）。
- 若某天需要每路相机独立名字/多路并发，方案 B 比 v4l2loopback 更灵活：
  每个相机一个 `node.name`/`node.description`，不需要 rmmod 模块。

## 6. PipeWire / portal 路线调研（只读，不改系统）

### 6.1 WirePlumber 现状（更正）

- `wireplumber 0.4.17-1ubuntu4.1`，**自带 v4l2 monitor**：
  `/usr/share/wireplumber/main.lua.d/30-v4l2-monitor.lua` +
  `/usr/share/wireplumber/scripts/monitors/v4l2.lua`（走 SPA `api.v4l2.enum.udev`），
  SPA 插件 `/usr/lib/aarch64-linux-gnu/spa-0.2/v4l2/` 存在。
- 所以 `/dev/video2` 被自动发布成 `Video/Source` + `media.role=Camera` 节点（id 57）。
- `libspa-0.2-libcamera` **没有**安装，SPA 插件目录里没有 `libcamera`。
  → 真实 sensor（`/dev/video0` 的 raw Bayer）不会出现在 PipeWire 里
  （`wpctl` 里那 14 个 `Qualcomm Camera Subsystem` device 没有对应 source），
  但这正是我们不需要的路径：我们自己的管线产出 NV12 → v4l2loopback。
- 架构文档 §5.2 的「没有 camera monitor」结论应更新为：
  **0.4.17 有 v4l2 camera monitor；缺的是 libcamera SPA 模块（仅影响 raw sensor 直出）**。

### 6.2 WirePlumber ≥0.5 升级评估（不建议）

| 途径 | 结论 |
|---|---|
| apt（noble / noble-updates） | **只有 0.4.17**，`apt-cache madison wireplumber` 无 0.5；`noble-backports` 源已配置但里面也没有 0.5 |
| 从 oracular+ 拷 deb | 不可取：依赖 `libwireplumber-0.5`、Lua 5.4 ABI、PipeWire 版本基线不同，跨发行版混装风险高 |
| 源码编译 0.5.x | 技术上可行（`meson`、`liblua5.4-dev 5.4.6` 可 apt 安装、`libpipewire-0.3-dev 1.0.5` 已装），但 0.5 的配置系统（`wireplumber.conf` + Lua 4/5 API）与 0.4 完全不同，替换 GNOME 46 正在用的 session manager 会影响音频/蓝牙/portal 权限，需要重写配置并回归；**收益为 0**（0.5 的 camera monitor 带来的 v4l2 能力 0.4.17 已有） |
| 结论 | **不要升级 WirePlumber**。当前 0.4.17 的 v4l2 monitor 已满足需求；0.5 的 libcamera camera monitor 需要 `libspa-0.2-libcamera`（本任务禁止安装），且我们不需要直出 raw sensor。 |

### 6.3 三个可选架构

- **方案 A（推荐，已全链路验证）**：GPU 管线 → NV12 → v4l2loopback `/dev/video2`
  → 原生 V4L2 App 直接读 / Flatpak App 经 Camera portal + WirePlumber v4l2 节点读。
  无需改系统。
- **方案 B（备选，已原型验证）**：GPU 管线 → NV12 → 自研 PipeWire 节点
  （`pw_camera_node.c`）`media.role=Camera`。不依赖 v4l2loopback、不依赖
  WirePlumber monitor；适合多路/自定义名字；需要自己处理 fd/权限/生命周期。
- **方案 C（不可用/需批准）**：libcamera 直出。需要 `libspa-0.2-libcamera`
  （被明确禁止安装）且 mainline CAMSS raw Bayer 需要 libcamera 的 IPA 支持；
  本机不适用。

### 6.4 多相机命名（当前限制与绕过）

当前 `/dev/video2` 的 card 名 "Elish Front Camera" 是模块参数 `card_label` 定的。
需求「前后摄各一个标准相机设备」时：

1. 需要两个 loopback 设备：`modprobe -r v4l2loopback && modprobe v4l2loopback
   video_nr=2,3 card_label="Elish Front Camera","Elish Rear Camera" ...
   exclusive_caps=1`。**这是 rmmod/改模块参数，必须先经 Lead 批准**，本次未做。
   注意 `exclusive_caps=1` 时 /dev/video2 只有在生产者推流时才 announce CAPTURE，
   对「按需启动」的 App 更不友好，需要常驻推流。
2. 不改系统时：只有单一设备名；可以用 PipeWire 方案 B 给每路一个
   `node.name`/`node.description`（Flatpak/portal 显示的名字来自 PipeWire 节点）。
3. 也可以给 `/dev/video2` 设一个通用名字后，让 App 侧按内容/配置区分。

## 7. 第三方 App 兼容矩阵

| App 类型 | 使用接口 | 本机可行性 | 说明 |
|---|---|---|---|
| 原生 Linux App（ffmpeg / gst / Guvcview / 大多数老 App） | V4L2 `/dev/video2` | ✅ 已实测 | NV12 30fps，1080p/3280x2464/4208x3120 均可 |
| 原生 PipeWire App（GStreamer pipewiresrc / libpipewire） | PipeWire 节点 | ✅ 已实测 | `target-object=<serial>`，NV12；WirePlumber 自动建节点 |
| Flatpak App（Camera portal：GNOME Snapshot / Cheese 等） | portal → PipeWire fd | ✅ 机制已实测，**App 级未实测** | `IsCameraPresent=true`，`OpenPipeWireRemote` 返回 fd，`pipewiresrc fd=` 取帧成功；本机没装 Flatpak 相机 App |
| Flatpak App 直接读 `/dev/video*` | 无（sandbox 隔离） | ❌ | 需要 `--device=all`，不推荐；应走 camera portal |
| 只支持 YUYV/MJPEG 的 App | V4L2 | ⚠️ 需适配 | 当前设备只提供 NV12；Guvcview 支持 NV12，但部分老 App 会因格式不匹配失败。需要生产者侧转换（GPU/CPU）或第二路 YUYV 设备 |
| libcamera 原生 App（如 cam/部分 Flatpak） | libcamera | ❌ | 没有 `libspa-0.2-libcamera`，raw sensor 不可直接给 libcamera；走我们的 NV12 loopback 才行 |
| 需要独占/物理相机的 App | V4L2 | ✅ | `/dev/video2` 普通 V4L2 相机语义，多消费者共享 |

### Flatpak / portal App 需要满足的条件（本机）

1. **一个 `media.role=Camera` 的 PipeWire 节点要一直存在**：
   本机由 WirePlumber v4l2 monitor 对 `/dev/video2` 自动提供（即使生产者没推流，
   节点也在，所以 `IsCameraPresent` 恒 true）。若走方案 B 则要保持
   `pw_camera_node` 常驻。
2. **应用首次使用时通过 Camera portal 权限**：
   - 本机 `org.freedesktop.impl.portal.PermissionStore` 的 `camera` 表当前为空；
   - portal-gnome 46.2 实现了 `org.freedesktop.impl.portal.Access`（`AccessDialog`），
     沙箱应用首次 `AccessCamera` 会弹权限对话框，**需要有人点允许**；
   - 也可以预先授权（工具存在，本机未实测）：
     `flatpak permission-set camera <app-id> camera yes`（权限表的 table=id=app 参数按
     flatpak 1.14 的 `permission-set` 语法）。
3. **应用本身要使用 Camera portal**（不是直接 open `/dev/video2`）：
   拿到 `OpenPipeWireRemote` 的 fd 后，用 `pipewiresrc fd=<fd>` 或 libpipewire 消费。
4. **生产时只暴露一路相机节点**（方案 A 与 B 不要同时开），否则 App 会看到两个相机。

### Flatpak 端到端还没验证的部分（明确缺口）

- 本机没有可用的 Flatpak 相机 App（无 cheese 可执行文件、无 gnome-snapshot），
  因此「Flatpak 沙箱内 App 弹窗 → 允许 → 出画面」的完整 UI 流程未实测。
  主机侧的 portal API + fd + pipewiresrc 链路已完整验证，剩下的是 UI/权限交互。
- 建议后续装一个 Flatpak 相机 App 做最终验收（例如 `org.gnome.Snapshot`
  或 `org.gnome.Cheese`），或写一个最小 Flatpak 测试 App 走 Camera portal。

## 8. 给第三方 App 的推荐接口方案（结论）

1. **主推**：v4l2loopback `/dev/video2`，NV12，生产者常驻推流。
   - 原生 App 直接 `-f v4l2 -i /dev/video2` / `v4l2src device=/dev/video2`；
   - Flatpak/GNOME App 走 Camera portal，本机现有系统组件即可（不用升级、不用装
     libcamera、不用改 WirePlumber）。
2. **服务必须常驻**：启动时先 `elish_lb_open()` 并推帧（没有画面时推黑帧/测试图），
   否则消费者先打开会拿到 `EINVAL`（§4.9）。
3. **多相机**：短期用方案 B 的 PipeWire 节点区分名字；若要两个 `/dev/videoN`
   标准设备，需要 Lead 批准后 rmmod/reload v4l2loopback（`video_nr=2,3` +
   `card_label=...`），并改用 `exclusive_caps=1` + 常驻推流。
4. **格式协商**：只提供 NV12。若发现目标 App 只支持 YUYV/MJPEG，再在
   生产者侧加格式转换（GPU 或 CPU）或增加第二路 loopback；这是后续可选工作，
   当前实测的 ffmpeg/gst/guvcview/pipewiresrc 都直接吃 NV12。
5. **验证/运维命令**见 §4；`build/loopback_test` 可作为长期压测/保底帧工具。

## 9. 复现步骤（最短路径）

```bash
cd elish-camera
make                                     # 全绿：build/elish-camera + tools

# 1) 生产者压测（1080p / 全分辨率）
flock -w 60 .locks/loopback.lock -c \
  './build/loopback_test --dev /dev/video2 --size 1920x1080 --fps 30 --seconds 10'
flock -w 60 .locks/loopback.lock -c \
  './build/loopback_test --dev /dev/video2 --size 3280x2464 --fps 30 --seconds 10'
flock -w 60 .locks/loopback.lock -c \
  './build/loopback_test --dev /dev/video2 --size 4208x3120 --fps 30 --seconds 10'

# 2) 原生消费者（生产者需同时运行，放在同一个 flock 命令里）
flock -w 60 .locks/loopback.lock -c '
  ./build/loopback_test --size 1920x1080 --fps 30 --seconds 20 &
  sleep 1.5
  ffmpeg -y -f v4l2 -i /dev/video2 -t 5 -f null -
  gst-launch-1.0 -q v4l2src device=/dev/video2 num-buffers=120 \
    ! video/x-raw,format=NV12,width=1920,height=1080 ! fakesink
  kill %1
'

# 3) PipeWire 节点消费者（生产者运行时）
flock -w 60 .locks/loopback.lock -c '
  ./build/loopback_test --size 1920x1080 --fps 30 --seconds 15 &
  sleep 1.5
  gst-launch-1.0 -v pipewiresrc target-object=58 num-buffers=90 \
    ! video/x-raw,format=NV12 ! fakesink
  kill %1
'

# 4) 可选：自研 PipeWire 节点原型
gcc -O2 -Wall -Wextra -std=c11 -D_GNU_SOURCE tools/pw_camera_node.c \
    -o build/pw_camera_node $(pkg-config --cflags --libs libpipewire-0.3)
build/pw_camera_node --seconds 0 &
gst-launch-1.0 -v pipewiresrc target-object=elish-camera-pw num-buffers=90 \
  ! video/x-raw,format=NV12 ! fakesink
kill %1
```

## 10. 本次对系统做的改动（透明记录）

- 只为可选原型安装了**开发头文件**：`libpipewire-0.3-dev`（连带 `libspa-0.2-dev`），
  版本 1.0.5-1ubuntu3.3，纯头文件，**未重启/未升级**任何 PipeWire/WirePlumber 服务。
- 未 rmmod / 未改 v4l2loopback 模块参数、未改 DTB/内核、未 reboot、
  未安装 `libspa-0.2-libcamera`、未改 WirePlumber 配置。
- 测试期间仅改动 `/dev/video2` 的当前格式（NV12），格式残留无害（下一个生产者会重设）。

## 11. 证据文件（本机 /tmp，非仓库）

- `/tmp/lb.log`、`/tmp/ff.log`、`/tmp/gst.log`、`/tmp/guv.out`、`/tmp/pw_src.log`、
  `/tmp/pw_custom.log`、`/tmp/pw4208.log`、`/tmp/portal_cam_test.py`
  （临时测试产物，未入库；需要可重新按 §9 复现）。
