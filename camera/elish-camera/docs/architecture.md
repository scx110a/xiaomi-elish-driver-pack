# elish-camera 架构与已验证事实（2026-11-12 晚，修订版）

> 本文替代旧交接文档中过时的相机结论；所有数字均来自本机实测。
> 早期交接文档中的错误点（Bayer/vflip/portal 等）已在本文与 `measurements/measurements-integrated.md` 中纠正。

## 1. 传感器与 media graph

### 前摄 OV8856
- entity `ov8856 23-0010`，subdev `/dev/v4l-subdev24`
- 链路（单相机模式）：CSIPHY4（subdev4）→ CSID0（subdev6）→ VFE0 RDI0 → `/dev/video0`
- 2-lane；模式：**3280×2464@30fps**、1640×1232
- raw：**pBAA**（MIPI RAW10 packed），3280×2464 stride **4112**，单帧 10,131,968 B
- 实际 CFA 顺序：**BGGR**（见 §2.1）
- 控制：`exposure` 6..2482、`analogue_gain` 128..2047、`digital_gain` 0..4095；
  **必须 `vertical_flip=0`**（见 §2.2）

### 后摄 OV13B10
- entity `ov13b10 20-0010`，subdev `/dev/v4l-subdev25`
- 链路：CSIPHY1（subdev1）→ CSID0 → VFE0 RDI1 → `/dev/video1`
- 4-lane；模式：**4208×3120@30fps**、4160×3120、4160×2340、2104×1560@60、2080×1170、1364×768@120
- raw：**pgAA**（packed，SGRBG），4208×3120 stride **5264**，单帧 16,423,680 B
- 实际 CFA 顺序：**GRBG**（见 §2.1）
- 控制：`exposure` 6..3188、`analogue_gain` 128..1984、`digital_gain` 1024..4095
- 对焦：CN3927V VCM，sysfs `/sys/bus/i2c/devices/20-000c/focus`（0..1023，非标准 V4L2）
- EEPROM：24c64 @ 0x51（8 KB）；Android 侧 LSC 表已解出（详细 Android 取证资料已移出包，归档于工作区）

### Hi556 景深（未通）
- Android 侧确认：CSIPHY0、CCI1 i2c0、MCLK2 GPIO95、reset GPIO26、2-lane / 437MHz；
  avdd=PM8008 L3P 2.8V，dovdd/dvdd=PM8150A L1/L2。
- 上电顺序：3 路供电 → MCLK → **最后**释放 reset（与 OV8856/OV13B10 相反）。
- mainline 阻塞点：PM8150A L2（`vreg_l2a_3p1` 固定 3.1V）被 USB HS PHY 占用，L1 无节点；
  不能简单改 L2 电压（会破坏 USB/键盘），需 L2 运行时互斥/迁移后再验证。
- 更详细的 Android 反编译/EEPROM/试错记录已移出包（历史归档）。

### 前后摄互斥（实测结论）
- 两个 sensor **不能同时出流**：后摄先开、前摄后开时，前摄一启动后摄立刻停流；
  反过来前摄启动会占住共享资源。原因在 camss/DTB 共享资源，不是功耗。
- 但两个相机是**两个独立的标准 V4L2 设备**：`/dev/video2`（前）/`/dev/video20`（后）。
  `elish-camera autoserve` 常驻并把「被打开的那个设备」映射到当前唯一可用的传感器：
  哪个设备有 consumer，就切到对应相机并给它喂帧；没有 consumer 时停流省电。
  因此第三方 App 只需打开想要的设备，不需要控制面板/脚本。
- 切换时仍必须 `media-ctl -d /dev/media0 -r` → 重新配置链路 → 设格式 → 开流。

## 2. 关键坑（全部实测）

### 2.1 Bayer 顺序（决定颜色是否正确）
- 方法：对同一帧 raw 统计 4 个 2×2 相位均值。Bayer 的两个绿色相位应几乎相等。
- 结果：前摄 `m(0,1)≈m(1,0)`（差 0.0）→ 绿在 (0,1)/(1,0) → **BGGR**；
  后摄 `m(0,0)≈m(1,1)`（差 1.2）→ 绿在 (0,0)/(1,1) → **GRBG**。
- 用错会得到紫/绿色偏；灰世界 AWB 无法纠正 R/B 互换造成的肤色错误。
- 驱动上报的 media bus code（前摄 SBGGR / 后摄 SGRBG）只用于 CSI 路由，不等于物理 CFA 顺序。

### 2.2 前摄全分辨率无帧的回归
- 17:09 的 `0005-ov8856-bggr-vflip.patch` 把 `V4L2_CID_VFLIP` 默认值改成 1；
  **vflip=1 时 3280×2464 模式 STREAMON 成功但永远没有 DQBUF**。
- 本管线每次打开都显式 `vertical_flip=0`（`capture.c`）。vflip=0 时 30 fps 正常。

### 2.3 其它
- `BG10`（16-bit plain Bayer）`VIDIOC_S_FMT` 接受但不出帧；GPU 输入必须 packed RAW10。
- 不要用 GStreamer `glshader` 做 debayer（只接受 RGBA）。
- `media-ctl -V` 设错 mbus code ⇒ STREAMON `-32 EPIPE`。
- 方向：前摄 180°，后摄 270°（以画面为准，CLI 可覆盖）。
- **Venus 设备号不跨启动稳定**：重启后 encoder/decoder 的 video 节点编号可能互换
  （本机曾从 video16/video15 变成 video15/video16）。必须按 sysfs 名字
  `qcom-venus-encoder` / `qcom-venus-decoder` 发现，不要硬编码。
- 采集偶发无帧：已加 2 s poll 超时 + 自动重配重试（最多 3 次）。

## 3. GPU（Adreno 650 / FD650）

- Mesa 25.2.8，EGL 1.5，GBM surfaceless 可无显示器建 context；renderer `FD650`；GLES 3.2。
- 自研管线：packed RAW10 在 GPU 内解码 → 双线性去马赛克 → WB/gamma → NV12 或 RGB888；旋转在 shader 内完成。
- 关键优化：`glReadPixels` 仅 ~0.8 GB/s → 改为 `/dev/dma_heap/system` 的 dma-buf + EGLImage
  render target（CPU cached mmap ~10 GB/s），Y/UV 同 buffer + `DMA_BUF_IOCTL_SYNC`。
- 实测（含回读）：前摄 3280×2464 NV12 **11.0–12.1 ms**；后摄 4208×3120 **18.7–19.1 ms**（p95 ≤22 ms）。
- 详见 `measurements/measurements-gpu.md`。

## 4. Venus 硬件编码

- 节点按 sysfs 名发现（`/dev/video15` 或 `/dev/video16`），V4L2 stateful M2M multi-planar。
- OUTPUT NV12，CAPTURE H.264 / HEVC / VP8；实测支持 4208×3120 与 3280×2464。
- 吞吐（真实内容，90 帧）：4208×3120 HEVC 90.6 fps / H.264 94.4 fps；
  3280×2464 HEVC 135 fps / H.264 136 fps。
- 实时 30 fps 节拍下 0 丢帧、`push` p95 4.7–8.1 ms；码流可被 ffmpeg 软解。
- 注意：mainline venus 把 HFI IDR period 硬编码为 0，周期 I 帧不是 IDR；`force_keyframe` 可产生真 IDR。
- 详见 `measurements/measurements-encode.md`。

## 5. 对外接口（第三方 App）

- v4l2loopback（模块参数持久化在 `/etc/modprobe.d/elish-vcam.modprobe.conf`）：
  `/dev/video2` = Elish Front Camera，`/dev/video20` = Elish Rear Camera。
- `elish-camera serve --camera front|rear` 常驻推 NV12；`stream`/`record --loopback` 也可推送。
- 消费者实测：ffmpeg / GStreamer v4l2src / Guvcview / 3 并发消费者均 30 fps。
- PipeWire：`elish-camera` **自己发布两个原生 PipeWire 相机节点**
  （`src/pwcam.c`，`elish-camera-front` / `elish-camera-rear`，NV12 全分辨率，`media.role=Camera`）。
  原因：v4l2loopback 经 WirePlumber 发布的节点**不暴露 EnumFormat（空 caps）**，
  GStreamer/aperture 系 App（GNOME Snapshot）会过滤掉；原生节点的 caps 完整，
  portal fd 中可见，Snapshot 可正常预览/拍照。
  WirePlumber 0.4.17 的 v4l2 monitor 仍会把 `/dev/video2/20` 发布成节点，供纯 PipeWire 客户端使用；
  autoserve 同时按 V4L2 打开和 PipeWire 节点消费两种事件路由传感器。
  **不需要升级 WirePlumber 0.5、不需要 libspa-0.2-libcamera**（旧交接文档 §6.3 结论有误）。
- xdg-desktop-portal Camera frontend 存在：`org.freedesktop.portal.Camera`
  （`IsCameraPresent` 属性 + `AccessCamera`/`OpenPipeWireRemote`）；本机 `IsCameraPresent=true`。
- Flatpak/portal 相机 App 的路由与授权验证见 `docs/interface.md`；不是后端缺失。
- 详见 `docs/interface.md`（含实测矩阵与命令）。

## 6. 代码结构

```
include/elish_camera.h   公共 API
src/capture.c            CAMSS 采集 + media graph + 传感器控制
src/gpu_debayer.c        EGL/GLES GPU debayer（NV12/RGB888/stats）
src/venus_enc.c          Venus V4L2 M2M 编码 + mp4 封装（ffmpeg stream copy）
src/loopback.c           v4l2loopback 生产者（NV12）
src/main.c               CLI：list/photo/record/stream/serve + AE/AWB + PNG + 线程化 loopback
tools/*                 各模块测试工具
docs/                   本文、measurements/、interface.md、evidence/（反编译解析脚本与证据）
testdata/               少量 raw 测试帧（磁盘有限，勿堆大文件）
```

## 7. 测试帧（testdata/）

| 文件 | 尺寸 | stride | 说明 |
|---|---|---|---|
| front-3280x2464-sgrbg10.raw | 3280×2464 | 4112 | 前摄全分辨率（实际 BGGR） |
| front-1640x1232-sgrbg10.raw | 1640×1232 | 2064 | 前摄 binning |
| rear-4208x3120-sgrbg10.raw | 4208×3120 | 5264 | 后摄全分辨率（GRBG） |

> `testdata/` 不随包（磁盘有限）；需要时按 `docs/DEVICE-DATA-EXTRACTION.md` 从自己设备采集 raw。

MIPI RAW10 解码：4 像素/5 字节，`p0=(b0<<2)|(b4&3)`、`p1=(b1<<2)|((b4>>2)&3)`、
`p2=(b2<<2)|((b4>>4)&3)`、`p3=(b3<<2)|((b4>>6)&3)`，10-bit 值 0..1023。

## 8. 开发/协作约定

- 实机访问加 flock：`.locks/camera.lock`（media0/video0/video1）、`.locks/venus.lock`（编码器）、
  `.locks/loopback.lock`（/dev/video2/video20）。
- 禁止：改 DTB/内核模块/重启/装 `libspa-0.2-libcamera`；需要先确认。
- 磁盘有限：不要留大 PPM/NV12/PNG；测量数据写 docs。
