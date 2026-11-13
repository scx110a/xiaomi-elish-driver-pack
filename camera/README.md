# 前摄 OV8856（L1）

> 本文是 2026-11-12 的开发过程记录（按时间追加），当前状态见文末「最终状态」与
> `camera/elish-camera/README.md`。

## 状态（2026-11-12）

- 前摄 OV8856 已 probe：CCI1 / CSIPHY4 / 2-lane，`ov8856 21-0010`
- CAMSS → CSID0 → VFE0 RDI0 通路打通，`/dev/video0` 1640x1232 `pgAA` raw
- 简单 QR + 复杂银行/微信码均已验证（ZXing 主解码，zbar 兜底）
- （历史：本段为前摄初始状态；后摄 OV13B10 已于同日打通，当前状态见文末「最终状态」）
- 无 3A/AF/EEPROM；固定曝光/增益；raw Bayer；视频通话还需软 ISP

## 安装脚本

```bash
sudo install -m 755 camera/elish-camera-init /usr/local/bin/
sudo install -m 755 camera/elish-camera-scan /usr/local/bin/
sudo apt install python3-zxing-cpp zbar-tools   # ZXing 主解码，zbar 兜底
```

> 删除 sudo 免密后也**无需额外授权**：`camera/install.sh` 会把 `/etc/elish-orientation.conf`
> 设为安装用户组可写；VCM focus sysfs 由 udev 规则保证 video 组可写。

## 使用

```bash
elish-camera-scan /tmp/qr.png
# 成功时输出 DECODED: ...
```

`elish-camera-init` 会设置：

- media links：`msm_csiphy4:1 -> msm_csid0:0`、`msm_csid0:1 -> msm_vfe0_rdi0:0`
- 格式：`SGRBG10_1X10/1640x1232`
- 控制：默认 `exposure=1508`、`analogue_gain=512`、`digital_gain=1024`

参数可覆盖：

```bash
EXPOSURE=1508 ANALOGUE_GAIN=2047 DIGITAL_GAIN=4095 elish-camera-scan
```

`elish-camera-scan` 默认自动尝试：
`screen 1508/512/1024`、`screen-dim 1508/1024/1024`、`paper 1508/2047/4095`、
`paper-soft 1508/1024/2048`、`bright 1508/128/1024`，以 zbar 解码成功为准；
都不成功时保留对比度最高的 preview。

## 依赖

- 内核：`6.18.44-current-sm8250`，启用 `VIDEO_QCOM_CAMSS`、`VIDEO_OV8856`、
  `MFD_QCOM_PM8008`、`REGULATOR_QCOM_PM8008`、`SM_CAMCC_8250`
- 联合 DTB：包内 `kernel/dtb/sm8250-xiaomi-elish-csot.dtb`（含 PM8008/OV8856/CAMSS 节点）
- userspace：`v4l-utils`、`zbar-tools`、`python3-pil`

## 限制

- 媒体链路不是持久配置，每次启动后由 `elish-camera-init` 设置；
- 曝光/增益是固定的，换环境需调整；
- 预览/扫码在软件里旋转 180°；raw 本身方向相反；
- 视频通话需要 PipeWire/libcamera 软 ISP 或自定义虚拟摄像头链路，尚未完成。

## 分辨率/全分辨率

- OV8856 原生：**3280×2464（8MP，4:3）**；当前默认抓帧用 **1640×1232（2MP）** 模式。
- 全分辨率 raw 抓帧已验证（3280×2464，单帧 10,131,968 B）。
- 当前软件 debayer 的 2×2 重建输出是 **1640×1232**；要得到真正的 3280 全彩需要完整插值/GPU，
  CPU 成本很高，暂不适合 30fps 录制。
- Venus H.264/HEVC 编码器支持 4K 输入，但瓶颈在软件 debayer，不在编码器。
- 实用建议：视频通话/录像用 640×480 / 1080p（从 1640 彩色源缩放）；8MP 全分辨率用于单帧/raw 静照。

## 彩色虚拟摄像头

```bash
# 加载 v4l2loopback（首次或重启后）
sudo modprobe v4l2loopback video_nr=2 card_label="Elish Front Camera" exclusive_caps=0

# 启动彩色前摄 → /dev/video16
elish-camera-vcam
# 另一个终端/视频通话 App 中选 "Elish Front Camera"
```

- 输出：640×480 YUYV，软件 debayer + AWB
- 可调 `VCAM_FPS`（默认 15）、`WB_R`/`WB_B`（默认 1.8/1.5）
- 彩色单帧：`elish-camera-color /tmp/color.png`

## 硬件录制（Venus H.264）

```bash
elish-camera-vcam &   # /dev/video16
gst-launch-1.0 -e v4l2src device=/dev/video16 num-buffers=150 \
  ! video/x-raw,format=YUY2,width=640,height=480,framerate=30/1 \
  ! videoconvert ! v4l2h264enc ! h264parse ! mp4mux \
  ! filesink location=/tmp/front-record.mp4
```

Venus 编码器是 `/dev/video15`，硬件 H.264/HEVC；标准视频通话 App 使用 raw 虚拟摄像头，不直接使用 Venus。

## Flatpak / libcamera（COSMIC Camera）

相机 App 走 libcamera simple pipeline，软件 ISP 需要 DMA-BUF heap：

```bash
# 内核需 CONFIG_DMABUF_HEAPS + SYSTEM/CMA；本机内核 #10 已启用
sudo install -m 644 camera/99-elish-dma-heap.rules /etc/udev/rules.d/
sudo udevadm control --reload-rules
sudo udevadm trigger --subsystem-match=dma_heap
```

然后启动 `io.github.cosmic_utils.camera`。libcamera 会看到 `ov8856 21-0010`，软件 ISP 做 debayer；
`/dev/dma_heap` 必须是 `root:video 0660`，否则会报 `Could not open any dma-buf provider`。

## 高分辨率录制

- 前摄原生 **3280×2464（8MP）**；raw 全分辨率可抓。
- 高分辨率**彩色**录制瓶颈是软件 debayer/3A，不是 Venus：当前 2×2 快速 debayer 只输出
  1640×1232 彩色；真正的 8MP 彩色需要完整插值/GPU/NEON。
- Venus H.264/HEVC 编码器支持 4K，但没有硬件 ISP 可用。
- 后摄要参与高分辨率录制，还需要 OV13B10 驱动 + AF（CN3927）+ EEPROM；深度 Hi556 另算。
- 实用路线：1640 彩色源 → 1080p/720p → Venus；8MP 先做静照/raw。

## 已知：Flatpak COSMIC Camera / libcamera

- 已修：内核 `DMABUF_HEAPS_SYSTEM/CMA` + `/dev/dma_heap` udev（root:video 0660）；
  否则 libcamera 报 `Could not open any dma-buf provider`。
- 已修：boot cmdline 增加 `cma=256M`；否则软件 ISP 在 3200×2400 时报 CMA 分配失败。
- 仍卡：libcamera 将 CSI/CSID/VFE 配成 **BGGR**，但 OV8856 media bus 仍为 **SGRBG**，
  media pipeline 启动返回 `-32 EPIPE`（`Failed to start media pipeline: -32`），
  所以 COSMIC Camera 无预览/无窗口。
- 需要：给 libcamera 增加 OV8856 sensor properties（Bayer order GRBG），
  或让 OV8856 驱动以 BGGR 顺序报告/输出；属于 sensor/libcamera 适配，不是 CMA issue。

## OV8856 Bayer / libcamera 现状（只读确认）

- `ov8856.c` 同时支持 `SBGGR10_1X10` 和 `SGRBG10_1X10`，但 **2-lane 的 3280/1640 模式默认 SGRBG**；
  只有 4-lane 的 3264/1632 模式默认 `SBGGR10`。
- 前摄是 2-lane，所以 libcamera 把 CSI/VFE 配成 BGGR 时与 sensor 的 SGRBG 不一致，
  media pipeline 返回 `-32 EPIPE`。
- `vertical_flip` 控制存在，可改变 Bayer 顺序，但驱动 `get_format` 仍报 SGRBG；
  简单靠翻转不能自动解决 media pipeline 校验。
- 正确路线：给 libcamera 增加 OV8856 sensor properties（GRBG），或做等价的驱动适配；
  内核强行报 BGGR 属于 hack，颜色/方向风险高。

## GPU debayer 可行性（只读确认）

- GPU：Adreno 650（FD650），Mesa freedreno；OpenGL 4.6 / GLES 3.2 / Vulkan 1.3 Turnip；
- EGL 支持 `EGL_EXT_image_dma_buf_import`，可 zero-copy 导入相机 dma-buf；
- GStreamer 已有 `glshader`、`glcolorconvert`、`glupload`、`glimagesink`；
- GPU debayer 可行，预期比当前 CPU 全帧 0.84s 快一个数量级；但需要自写 shader/管线，
  libcamera 当前没有 GPU soft-ISP 路径。

## COSMIC Camera 状态（2026-11-12）

- 已用 `ov8856.ko` 小补丁（2-lane 默认 BGGR + VFLIP=1）让 libcamera media pipeline 能启动；
- CLI 实测：
  - 照片：`camera photo` → 3200×2400 JPEG；
  - 录像：`camera video -d 5` → 1920×1088 HEVC，5s/6.4MB；
- libcamera 实际使用 **EGL/FD650 GPU debayer**，日志 `Debayer debayer_egl.cpp`，benchmark 10.5 ms/帧；
- 但该 Flatpak 的 **GUI 仍不创建窗口**，即使用 `--preview-fake-camera --preview-source` 也一样，
  属于 App 的 iced/winit GUI 兼容性问题，不是相机/驱动；
- 当前可用方式：先在终端用 `flatpak run --command=camera APP photo|video`，或用我们的 vcam + V4L2 应用。
- 模块补丁备份：`/root/ov8856-bggr-20261112-170913/`（恢复原 `ov8856.ko` 后重启即可回退）。

## GUI 替代：Guvcview / Cheese

- **Guvcview**：直接走 V4L2，使用 `elish-camera-vcam` 的 `/dev/video2`，可用；需要 v4l2loopback
  `max_buffers=16`，否则会报 `QBUFS/QUERYBUF Invalid argument`。
- **Cheese**：走 PipeWire/portal，看不到 v4l2loopback 虚拟摄像头，不适用于当前 vcam 方案。
- vcam 默认曝光已调为 `ANALOGUE_GAIN=1536 DIGITAL_GAIN=2048`，避免过曝。

## 后摄上游基础（2026-11-12 查找）

postmarketOS/Nura `sm8250-xiaomi-elish` 的 MR 27 已给出 OV13B10 后摄 DTS：

- CSIPHY1；CCI0 i2c0，地址 `0x10`；4-lane，link-freq 560MHz；
- MCLK0（CAM_CC_MCLK0_CLK，19.2MHz，GPIO94）；reset GPIO93；
- 供电：`vreg_l7p`（2.8V）、`vreg_l1c_1p8`（1.8V）、`vreg_l2p`（1.2V）；
- 需要 `cam_mclk0_default` pinctrl 和 `camera_wide_active`（GPIO93）；
- 主线已有 `drivers/media/i2c/ov13b10.c`、`hi556.c`，只需 DTS + 使能模块。

## 后摄 OV13B10 进度（2026-11-12）

- joint DTB 已加：CSIPHY1/port@1、CCI0 i2c0 camera@10、MCLK0(94)/reset(93) pinctrl、
  PM8008 L7P/L2P phandle；boot_b 已更新，系统能看到
  `/soc@0/cci@ac50000/i2c-bus@0/camera@10`（i2c-20/20-0010）。
- `ov13b10` 驱动已补 OF match；probe 能绑定，并确认三路供电（l7p/l2p/l1c）和
  MCLK0 19.2MHz 都起来、reset 已释放。
- 但仍 `ov13b10 20-0010: failed to find sensor: -5`；保持供电后 i2cdetect CCI0 无任何地址，
  CCI1 前端正常。下一步查 MCLK0 pinmux/实际波形、reset 电平和 sensor 地址/CCI 接线。
- 补丁：`kernel/patches/0006-ov13b10-of-supplies-reset.patch`；
  DTB 备份 `/root/rear-camera-20261112-173149/`。

## 后摄 OV13B10 L1（2026-11-12 打通）

- 接线：**CSIPHY1 → CSID0 → VFE0 RDI1**；CCI0（ac4f000）i2c0、地址 0x10；
  MCLK0 GPIO94、reset GPIO93；供电 L7P/L2P/L1C。
- 驱动：`ov13b10` 补 OF match + avdd/dovdd/dvdd + reset 脉冲
  （`kernel/patches/0006-ov13b10-of-supplies-reset.patch`）。
- 已验证：`/dev/v4l-subdev25`、`/dev/video1`，2104×1560 SGRBG10/pgAA raw 可抓。
- 使用：
  ```bash
  elish-camera-rear-init
  elish-camera-rear-color /tmp/rear.png   # 默认 2104x1560、rotation=270
  ```
- 限制：**无 AF/EEPROM**，当前固定焦点/画质一般；曝光/增益 helper 里手动拉高
  （exposure=1590, analogue_gain=1984, digital_gain=4095），后续需 AF + 自动曝光。
- 后摄默认 2104×1560；驱动也支持 4208×3120 等 4-lane 模式，高分辨率后续测。

## 后摄 AF：CN3927V（2026-11-12）

- 后摄 VCM 是 **CN3927V**：CCI0 i2c0 地址 **0x0c**；`cam_vaf` = **PM8008 L4P 2.8V**；
  EEPROM 在 **0x51**（暂未驱动）。
- DTS 已加 `camera_vcm@c`；驱动 `kernel/patches/0007-cn3927v.patch` 提供
  `/sys/bus/i2c/devices/20-000c/focus`（0..1023，越大越近/远按实测方向）。
- `elish-camera-rear-autofocus` 会扫焦段并保留最清晰位置；实测室内目标最佳 focus=640。
- 注意：L1C 1.8V (`cam_vio`) 目前保持 always-on 以给 VCM I2C 上拉供电，后续可改成
  VCM 驱动管理。

## 后摄 AE/AWB（2026-11-12）

- `elish-camera-rear-color` 现在默认 **AE=1**：最多 4 次抓帧迭代曝光/模拟增益/数字增益，
  再按 gray-world 计算 AWB gains；可用 `AE=0` 关闭。
- 当前室内目标平均亮度仍偏低（mean≈152/1023），但成片亮度/清晰度正常；
  后续可把 target/曲线调到更接近 18% 灰。
- `elish-camera-rear-autofocus` 仍可先扫焦再拍。

## EEPROM / 高分辨率（2026-11-12）

- 后摄 EEPROM：**24c64, 8KB @0x51**，DTS `eeprom@51` 自动 at24 绑定；
  **本包不附带 EEPROM dump**（单机工厂校准数据，属本机私有）。需要做校准分析时从**你自己的设备**读取：
  ```bash
  # at24 绑定后（0x51），直接从 sysfs 读 8KB（<bus> 见 ls /sys/bus/i2c/devices/*-0051）
  sudo dd if=/sys/bus/i2c/devices/<bus>-0051/eeprom of=rear-eeprom-24c64.bin bs=8192 count=1
  # 可选解析：python3 camera/elish-camera/docs/evidence/eeprom-24c64-parse.py rear-eeprom-24c64.bin
  ```
- 后摄全分辨率：4208×3120 raw 模式可用；AE/AF 后彩色 2×2 debayer 输出 **2104×1560**。
- `elish-camera-rear-color` 已支持 `W=4208 H=3120` 的 AE 循环；真正 4208 全彩仍需完整插值。

## Hi556 景深（进行中，2026-11-12）

- Android 接线：CSIPHY0、CCI1 i2c0、MCLK2 GPIO95、reset GPIO26、
  2-lane / link-freq 437MHz；avdd=PM8008 L3P 2.8V，dovdd/dvdd 按 Android 为 PM8150A L1/L2。
- 现状：DTS 节点/OF match 已加，`hi556 22-0020` 能 probe，但 I2C 读 ID 失败（-5）；
  保持 L3P + 临时 1.8/1.2 供电后 i2cdetect CCI1 仍无应答。
- 下一步需确认：PM8150A L1/L2 实际供电映射、sensor 真实型号/地址，或改用对应 PMIC rail；
  暂不影响前摄/后摄主线。

## Hi556 供电试错记录

- 已确认 Android vendor 里是 Hi556（`com.qti.sensor.elish_hi556.so`）。
- 试过两套供电映射：
  1. L3P + L12A(1.8) + L6A(1.2)
  2. L3P + L1C(1.8) + L2P(1.2)（与前后摄共用）
- 两套都能让 hi556 probe 到 `22-0020`，但 I2C 读 ID 始终 `-5`，CCI1 i2cdetect 无地址。
- 下一步必须从 vendor camera config / sensor library 拿到 **slaveAddr + powerSetting + reset/MCLK 时序**，
  或确认 depth 的 PM8150A L1/L2 在 mainline 的实际 regulator 节点。

## Hi556 进一步结论

- 已挂载 Android `vendor_a`，确认 `/vendor/lib64/camera/com.qti.sensor.elish_hi556.so`
  与 `com.qti.sensormodule.elish_qtech_hi556.bin`。
- 地址试过 0x20/0x24、供电试过 L3P+L12A+L6A 与 L3P+L1C+L2P，均 I2C 无应答。
- 下一步需解析 sensormodule 的 QTI Chromatix `powerSetting/slaveAddr`，或补 PM8150A L1/L2。
- vendor 镜像：`~/vendor_a.img`（你自己的 Android vendor 分区镜像）。

## Hi556 sensormodule 反编译结论

- `com.qti.sensormodule.elish_qtech_hi556.bin` 是 QTI Chromatix 二进制属性树。
- 顶层可解析字段：
  - `sensorDriverData` off=0 size=0x328
  - `cameraModuleData` size=0x28
  - `EEPROMDriverData` size=0x8f0
  - `sensorName` off=0xc40 size=0x10
  - `powerSetting` off=0xc50 size=0x90 / off=0xce0 size=0x78
- `slaveAddr` 在嵌套表里是 **对象节点（data offset 0x1bc0, size 0）**，不是可直接读的标量；
  因此不能再从该 bin 直接猜 I2C 地址/供电。
- 下一步只能反汇编 `com.qti.sensor.elish_hi556.so` 或找 vendor camera config 中明确的
  slaveAddr/powerSequence 证据，再改 DTS。

## Hi556 供电冲突结论（证据）

- Android base DTB：`pm8150a_l1` = 1.8V（RPMh ldoc1），`pm8150a_l2` = 1.2V（RPMh ldoc2）。
- Android depth `cam_vio=pm8150a_l1`、`cam_vdig=pm8150a_l2`。
- mainline joint：
  - 没有 `pm8150a_l1` 节点；
  - `vreg_l2a_3p1`（PM8150A L2）固定 **3.1V**，被 **PM8150B Type-C PD PHY、USB1/USB2 HS PHY 的 vdda33** 使用。
- 因此不能简单把 L2 改成 1.2V：会破坏 USB/键盘；也不能用 L1C/L2P 代用 depth 的物理供电。
- 下一步需要：确认是否有另一个 3.3V rail 可迁移 USB HS PHY，或做 L2 的运行时切换（USB 与 depth 互斥），再继续 Hi556。

## 后摄真全彩 4208×3120（2026-11-12）

```bash
elish-camera-rear-autofocus          # 先对焦
W=4208 H=3120 FULL=1 elish-camera-rear-color /tmp/rear-full.png
```

- `FULL=1` 使用完整 bilinear debayer，输出 **4208×3120** 全彩（非 2×2 半分辨率）。
- 实测约 25MB PNG；AF 后清晰。
- 默认仍是 2104×1560 2×2 快速路径。

## PipeWire / portal 现状（2026-11-12）

- 安装了 `wireplumber` 后，PipeWire 能识别 `/dev/video2`：
  - `Elish Front Camera` [v4l2]
  - `pipewiresrc target-object=v4l2_input._sys_devices_virtual_video4linux_video2` 实测可出帧。
- 但 GNOME 46 的 `xdg-desktop-portal-gnome` **没有 `org.freedesktop.portal.Camera`**，
  所以 Cheese（走 portal）仍报“未发现设备”。
- 直接 V4L2 的 Guvcview 正常；PipeWire 原生客户端可读 vcam；
  portal App 需要升级 portal 到带 Camera 接口的版本，或在 portal 层实现 Camera。

## portal Camera 结论（证据）

- `xdg-desktop-portal` frontend **已编译 Camera 接口**（strings 里有
  `org.freedesktop.portal.Camera`、`AccessCamera`、`IsCameraPresent`）。
- 但 `gnome.portal` backend 的 Interfaces 列表**没有 Camera**；
  `xdg-desktop-portal-gnome` 46.2 未实现 Camera。
- 因此 Cheese（走 portal）报“未发现设备”，即使 PipeWire vcam 源可用。
- 需要升级/替换 GNOME portal backend，或实现 Camera portal backend；
  当前 Guvcview 直连 V4L2 仍可用。

## portal Camera backend 最终结论

- `xdg-desktop-portal` frontend 有 Camera 接口；
- GNOME 51.1 backend 源码仍**没有 Camera 实现**，KDE backend 也没有；
- Snapshot/Cheese 都会调用 `org.freedesktop.portal.Camera:OpenPipeWireRemote`，
  因 backend 缺失而显示“未找到摄像头”。
- 标准 portal App 要支持，必须实现 `org.freedesktop.impl.portal.Camera` backend
  （暴露 vcam 的 PipeWire 节点），这不是升级包能解决的。
- GUI 仍用 Guvcview 直连 `/dev/video2`；PipeWire 原生客户端可用 pipewiresrc。

## 全分辨率 GPU 管线（elish-camera/，2026-11-12 晚新增）

`elish-camera/` 是新的前后摄全分辨率拍摄/录像管线：CAMSS raw → GPU (FD650/EGL-GLES)
debayer → NV12 → Venus 硬件 HEVC/H.264 编码 和/或 v4l2loopback 虚拟相机。

- 前摄 3280×2464、后摄 4208×3120 均约 30 fps 彩色拍照/录像；GPU debayer 前 11–12 ms、
  后 19–20 ms 每帧；Venus 两分辨率 HEVC/H.264 吞吐 >90 fps。
- 虚拟相机：`/dev/video2`（前）、`/dev/video20`（后），NV12 全分辨率，标准 V4L2；
  PipeWire/WirePlumber 会自动发布为 `media.role=Camera` 节点，portal Camera 可见。
- 安装：`sudo ./camera/install.sh`（编译 + 安装后端/脚本/udev/自启/休眠恢复）
- 用法：`elish-camera photo|record|stream|serve ...`，详见 `camera/elish-camera/README.md`
  与 `docs/architecture.md`、`docs/measurements-integrated.md`。
- 注意：前后摄**不能同时出流**（camss 限制），一次用一路；前摄必须 `vertical_flip=0`。

## 最终状态（2026-11-12 晚）

- 前后摄全分辨率彩色拍照/录像可用：前 3280×2464@~30fps、后 4208×3120@~30fps；
  GPU debayer（FD650）+ Venus HEVC/H.264；两个虚拟相机 `/dev/video2`（前）/`/dev/video20`（后），
  `elish-camera autoserve` 按 App 打开的设备自动路由传感器；
- PipeWire/WirePlumber 已发布两个 `media.role=Camera` 节点；portal `IsCameraPresent=true`，
  `OpenPipeWireRemote` 取流已实测（Flatpak/沙箱 App 路径）；
- 后摄软件自动对焦（CN3927V VCM）：`elish-camera focus --camera rear`、
  `--autofocus`；需要 `99-elish-vcm.rules` 让 video 组可写 focus sysfs；
- 分辨率/帧率用 `elish-camera-ctl panel` 或 `elish-camera-ctl start --front-size/--rear-size/--fps`；
- 限制：前后摄不能同时出流（一次一路，切换 1–2s）；分辨率由生产者决定（Guvcview 内改不了）；
  前摄固定焦点无 AF；Hi556 景深仍未通。
- 自检：`camera/elish-camera/verify.sh`（也已并入 `install/check-all.sh`）。

## 休眠唤醒后传感器/自动旋转恢复

`system/elish-sensors-resume-worker` + `system/elish-sensors-resume`（systemd sleep hook）
+ `system/elish-sensors-resume.service`（独立 cgroup 的 oneshot 服务）：

- suspend/resume 后等待加速计回来（0.5s 轮询，最多 60s），然后重启 `iio-sensor-proxy`，
  让 mutter 重新创建 D-Bus proxy 并 claim 加速计，恢复“折叠到背面 → 平板模式（自动旋转）”；
  若 60s 仍缺失则对 iio 做一次硬重启（`TimeoutStopSec=3` 限时）——**不再**拉起 SLPI 栈
  （2026-11-12 加速改造删除了无效的 slpi-sensors 兜底）；
- 实测（2026-11-13，M2 可写 registry 之后）：suspend→resume→加速计回来→iio 重新 claim 完成
  **总计 ~10.2s**（此前 ~35–40s），**自动旋转实测可用**；
- 日志：`journalctl -t elish-sensors-resume -b`

安装：

```bash
sudo install -m755 camera/system/elish-sensors-resume-worker /usr/local/sbin/
sudo mkdir -p /usr/lib/systemd/system-sleep
sudo install -m755 camera/system/elish-sensors-resume /usr/lib/systemd/system-sleep/
sudo install -m644 camera/system/elish-sensors-resume.service /etc/systemd/system/
sudo systemctl daemon-reload
```

查看日志：`journalctl -t elish-sensors-resume -b`

## 相机后端开机自启动

- `elish-camera-autostart`：等待 PipeWire socket 就绪后用 `elish-camera-ctl start` 拉起后端；
- `elish-camera-autoserve.desktop`（安装到 `/etc/xdg/autostart/`）：登录桌面后自动执行上面的脚本。

安装：

```bash
sudo install -m755 camera/elish-camera-autostart /usr/local/bin/
sudo install -m644 camera/elish-camera-autoserve.desktop /etc/xdg/autostart/
```

诊断工具 `camera/elish-check-usb`（安装到 `/usr/local/bin/`）可查看重启后键盘 USB 枚举次数和
平板模式状态：`elish-check-usb`。
