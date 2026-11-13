# Venus 硬件编码模块实测（venus_enc.c / enc_test）

> 历史测量记录（2026-11-12，本机实测）。当前架构/接口见 `../architecture.md`、`../interface.md`。

> 2026-11-12 晚在 Xiaomi Pad 5 Pro（SM8250 / elish，Armbian noble，kernel 6.18.44-current-sm8250）
> 实测。所有数字来自本机 `/dev/video16`（qcom-venus，V4L2 stateful M2M multi-planar）。
> 相关硬件事实见 `docs/architecture.md` §4、§6。

## 0. 结论速览

- **两分辨率 × 两 codec 全部达标**：4208x3120 / 3280x2464 的 HEVC 与 H.264 连续 90 帧
  （3.000 s @30fps）**0 丢帧、0 固件跳过帧**，码流可被 ffmpeg 软件解码，MP4 可播。
- **实时性**：按 30fps 节拍推送 90 帧，端到端 28.9–29.2 fps（≈30，差值来自定时器粒度与
  flush），**没有任何一帧超过一个帧周期**（最坏迟到 11.4–17.6 ms < 33.3 ms）；
  单次 `elish_enc_push_nv12()` p95 = **4.7–8.1 ms**（含 4K NV12 拷入 V4L2 缓冲）。
- **吞吐（不限速）**：4208x3120 HEVC 90.6 fps / H.264 94.4 fps；3280x2464 HEVC 135 fps /
  H.264 136 fps。远高于 30fps 需求，与 architecture.md 中 GStreamer 黑场基线
  （94 / 92 fps @4208）一致，且这里是**真实传感器内容**。
- **码率控制**：HEVC 实测码率与目标偏差 −0.7 % / +0.7 %；H.264 偏低 −12…−16 %
  （CBR 在 H.264 + 该内容下欠冲，见 §5）。
- **GOP/IDR**：`V4L2_CID_MPEG_VIDEO_GOP_SIZE` 生效（每 GOP 插入 I 帧），但 mainline
  qcom-venus 把 HFI IDR period 硬编码为 0，**周期性 I 帧不是 IDR**；`force_keyframe`
  路径可产生真正的 IDR（实测 0/10/20/…/80 共 9 个 IDR）。
- **H.264 4208x3120 本身可以编码/解码**（不是硬件限制），但超出 H.264 level 5.1
  MaxFS（36864 MB，本图为 51285 MB），SPS 只能标 level 5.1（驱动 `h264_level` 控件
  最大值）。见 §6。
- 冷启动（固件首次加载）首帧约 **0.85 s**；之后每次 `elish_enc_start()` 后首帧
  56–93 ms。

## 1. 环境与固件

```
CPU/SoC : SM8250 (Xiaomi Pad 5 Pro, elish)
Kernel  : 6.18.44-current-sm8250 #10 SMP aarch64
Device  : /dev/video16  driver=qcom-venus  card="Qualcomm Venus video encoder"
           caps=0x84204000 (M2M multi-planar + streaming + ext pix fmt)
Firmware: /lib/firmware/qcom/sm8250/xiaomi/elish/venus.mbn（本次实际加载路径）
          （/lib/firmware/qcom/venus-6.0/venus.mbn 为通用回退）
OUTPUT  : NV12 only
CAPTURE : H264 / HEVC / VP8（本模块用 H264 / HEVC）
控件    : video_bitrate(32k..160M), video_bitrate_mode(VBR/CBR/CQ),
          video_gop_size(1..65535), force_key_frame(button),
          h264/hevc profile/level, h264_i_period, frame_skip_mode,
          hevc_*_frame_qp_value(1..63) 等
```

驱动把请求尺寸向上对齐：**宽 128、高 32**。所以：

| 请求（也是码流尺寸） | 实际 V4L2 帧尺寸 | stride |
|---|---|---|
| 4208x3120 | 4224x3136 | 4224 |
| 3280x2464 | 3328x2464 | 3328 |

码流仍精确为请求尺寸：通过 `VIDIOC_S_SELECTION`（`V4L2_SEL_TGT_CROP`，`V4L2_BUF_TYPE_VIDEO_OUTPUT`）
把 HFI 输入/输出分辨率设为 4208x3120 / 3280x2464，喂给固件的是 `stride × out_h × 3/2`
的 padded NV12（裁剪左上角）。没有 crop 时码流会是 4224x3136 / 3328x2464。

## 2. 构建与测试命令

```bash
cd elish-camera
make                     # 全绿；产物 build/elish-camera、build/enc_test 等
make tools

# 编码器是唯一使用 /dev/video16 的模块 —— 所有实机命令都要加锁：
flock -w 60 elish-camera/.locks/venus.lock -c '<命令>'
```

`tools/enc_test` 直接链接 `build/venus_enc.o`，支持三种帧源：

```bash
# 真实传感器内容：packed MIPI RAW10 GRBG（testdata/*.raw），内部做 2x2 去马赛克 +
# BT.601 limited range 转 NV12；--scroll 2 让每帧水平位移 2px（P 帧有真实残差）
./build/enc_test --codec hevc --size 4208x3120 --fps 30 --bitrate 20000000 \
    --frames 90 --keyint 30 --raw10 testdata/rear-4208x3120-sgrbg10.raw \
    --out /tmp/rear-4208x3120.h265

# 合成内容：gradient | noise | bars | black（自带运动）
./build/enc_test --codec hevc --size 4208x3120 --frames 90 --keyint 30 \
    --synthetic black --out /tmp/black.h265

# 实时节拍模拟（按 --fps 定时推送，统计迟到帧）
./build/enc_test ... --realtime

# 编码 + 直接调 elish_mux_mp4() 封装（验证公共 API 中的 mux 路径）
./build/enc_test ... --out /tmp/a.h265 --mux /tmp/a.mp4
```

输出的 `--- summary ---` 包含：实际 padded 尺寸/stride、push/encoded/dropped/固件跳过帧数、
keyframe 数（来自 V4L2 `V4L2_BUF_FLAG_KEYFRAME`，不是猜 NAL）、ES 字节数、视频时长、
wall time、启动首帧耗时、端到端/稳态吞吐、push 延迟 min/avg/p95/max、CPU%（user+sys）、
按“视频时长”折算的码率及其与目标偏差。

单元级 mux/校验脚本（不占用设备，可随时执行）：

```bash
tools/mux_mp4.sh -f 30 -c hevc /tmp/rear-4208x3120.h265 /tmp/rear-4208x3120.mp4
# 内部执行：ffmpeg stream copy（-c copy + setts 打时间戳）→ ffprobe →
#           ffmpeg -v error -i <es> -f null - 软件解码校验
```

## 3. 验收数据（90 帧 = 3.000 s @30fps，真实传感器内容）

### 3.1 不限速（最大吞吐）

| 配置 | 编码帧 | 丢帧/跳过 | throughput | steady-state | push p95 | 目标码率 | 实测码率 | CPU |
|---|---|---|---|---|---|---|---|---|
| 4208x3120 HEVC 20M | 90/90 | 0 / 0 | 90.58 fps | 96.27 fps | 11.17 ms | 20.00 Mbit/s | **20.14 Mbit/s (+0.7 %)** | 75 % |
| 4208x3120 H.264 20M | 90/90 | 0 / 0 | 94.38 fps | 100.84 fps | 12.35 ms | 20.00 Mbit/s | **16.87 Mbit/s (−15.6 %)** | 80 % |
| 3280x2464 HEVC 16M | 90/90 | 0 / 0 | 135.20 fps | 146.85 fps | 7.29 ms | 16.00 Mbit/s | **15.89 Mbit/s (−0.7 %)** | 77 % |
| 3280x2464 H.264 16M | 90/90 | 0 / 0 | 135.99 fps | 151.17 fps | 7.35 ms | 16.00 Mbit/s | **14.03 Mbit/s (−12.3 %)** | 81 % |

补充：4208x3120 HEVC 连跑两次为 84.12 / 88.57 fps（端到端），码率均为 20.14 Mbit/s，
结果稳定。

### 3.2 实时节拍（`--realtime`，按 30fps 推送；这是“连续 30fps 不阻塞”的验收项）

| 配置 | 编码帧 | 端到端 fps | push p95 | 迟到 >1 帧周期 | 最坏迟到 |
|---|---|---|---|---|---|
| 4208x3120 HEVC 20M | 90/90 | 28.96 | 7.37 ms | **0** | 17.57 ms |
| 4208x3120 H.264 20M | 90/90 | 28.92 | 8.12 ms | **0** | 16.45 ms |
| 3280x2464 HEVC 16M | 90/90 | 29.15 | 5.83 ms | **0** | 12.15 ms |
| 3280x2464 H.264 16M | 90/90 | 29.16 | 4.67 ms | **0** | 11.37 ms |

端到端 28.9 fps 而不是 30.0 fps 的原因：首帧（固件加载/预热）不计入节拍起点，
最后一次 `stop()` 需要 flush 尾部帧；节拍基于 `CLOCK_MONOTONIC` + `nanosleep`。
关键是**最坏迟到远小于一个帧周期（33.3 ms）**，即编码器不会阻塞 30fps 采集循环。

### 3.3 合成内容对照（4208x3120 HEVC，90 帧，keyint 30）

| 内容 | 实测码率 | throughput | 备注 |
|---|---|---|---|
| 真实 RAW10（后摄 testdata） | 20.14 Mbit/s | 90.6 fps | CBR 命中最准 |
| 黑场（合成 black） | 0.77 Mbit/s | 89.7 fps | CBR 不会为了凑码率硬塞垃圾；与 GStreamer 黑场基线同量级 |
| 渐变 + 移动块（gradient） | ≈目标 | ≈90 fps | 合成运动 |
| 彩条 + 移动（bars） | — | 21 fps | 瓶颈是 enc_test 的 Python 风格逐像素除法生成器，不是编码器 |

### 3.4 强制 IDR（`force_key_frame` 控件）

- 普通 GOP（`--keyint 30`）：90 帧中 3 个 I 帧（第 0/30/60 帧），但 ffprobe 的
  `key_frame` 只有第 0 帧 = **IDR**；第 30/60 帧是非 IDR I 帧（见 §6 驱动细节）。
- `--keyint 300 --force-idr 10`：实测 9 个真正 IDR，位置 **0,10,20,…,80**
  （ffprobe 逐帧 `key_frame=1` 验证），码率 +1.8 %。
- 等价的环境变量便于主管线使用：`ELISH_ENC_FORCE_IDR=N`（每 N 帧自动请求一次 IDR），
  实测 `N=10` 得到 IDR 位置 0/10/20。
- `build/elish-camera record --keyint N` 目前由 main.c 每 N 帧传
  `force_keyframe=true`，走的正是这条路径。

## 4. 码流/MP4 校验

### 4.1 软件解码（`ffmpeg -v error -i <es> -f null -`）

```
OK   rear-4208x3120.h265     OK   rear-4208x3120.h264
OK   front-3280x2464.h265    OK   front-3280x2464.h264
OK   black-4208.h265         OK   forceidr-4208.h265
OK   rt-hevc-4208.h265       OK   rt-h264-4208.h264
OK   rt-hevc-3280.h265       OK   rt-h264-3280.h264
（全部无任何 error 输出）
```

ffprobe（ES）：

| 文件 | codec | profile | 尺寸 | level | 帧数 |
|---|---|---|---|---|---|
| rear-4208x3120.h265 | hevc | Main | 4208x3120 | 186 (=6.2) | 90 |
| rear-4208x3120.h264 | h264 | High | 4208x3120 | 51 (=5.1) | 90 |
| front-3280x2464.h265 | hevc | Main | 3280x2464 | 153 (=5.1) | 90 |
| front-3280x2464.h264 | h264 | High | 3280x2464 | 51 (=5.1) | 90 |

level 是显式设置的（驱动 profile/level 控件默认 level 1.0，必须自己设）：
HEVC ≤ 8.9M 像素用 5.1、更大用 6.2；H.264 驱动最大值只到 5.1。

### 4.2 MP4（`tools/mux_mp4.sh`，ffmpeg stream copy）

4 个配置全部 `mux_mp4: OK`，`ffprobe` 结果：`codec_name` / `width` / `height` /
`r_frame_rate=30/1` / `avg_frame_rate=30/1` / `nb_read_frames=90` 全部正确；
`ffmpeg -v error -i <mp4> -f null -` 解码无错误。

两个关键点：

1. **raw ES 没有时间戳**：直接 `ffmpeg -f hevc -i es -c copy out.mp4` 会让 mov muxer
   编出坏时间线（实测 90 帧被压成 ~1.67 s、r_frame_rate=60、尾部 40 帧时间戳塌缩）。
   `elish_mux_mp4()` / `mux_mp4.sh` 用 bitstream filter
   `setts=time_base=1/<fps>:pts=N:dts=N:duration=1` 给每个 packet 打 CFR 时间戳，
   得到精确的 30fps MP4。fps 来源：`ELISH_MUX_FPS`（默认 30），
   `enc_test --mux` 会按 `--fps` 设置。
2. 封装用 `-c copy`（stream copy），不重编码，CPU 占用可忽略。

## 5. 码率/质量观察

- **HEVC CBR 很准**（−0.7 % / +0.7 %）。真实内容在 20 Mbit/s 下，4208x3120 平均每帧
  约 84 KB（keyframe 约 1 MB 量级），画质由编码器按码率自动调 QP。
- **H.264 在本内容下欠冲 12–16 %**：目标 20 Mbit/s 实测 16.87，目标 16 Mbit/s 实测
  14.03。这是 H.264 + 该固件 CBR 的特性（不会为凑码率无限降 QP），不是 bug；
  需要严格命中码率时可以：改用 HEVC、设更低的 `--bitrate` 再观察，或调
  `V4L2_CID_MPEG_VIDEO_H264_*_QP`。黑场等极简内容在 CBR 下也只会用 0.77 Mbit/s。
- 实时节拍下 CPU 占用（含 4208x3120 NV12 每次 20 MB 的填充/拷入、UNIX 写文件）为
  单核 19–30 %；不限速时（编码器满负荷 + 拷贝）为 75–81 %。

## 6. 坑与限制（实测）

### 6.1 周期性 I 帧不是 IDR（mainline 驱动行为）

`drivers/media/platform/qcom/venus/venc.c` 的 `venc_set_properties()` 把
`HFI_PROPERTY_CONFIG_VENC_IDR_PERIOD` **硬编码为 `idr_period = 0`**，注释为
"only the first I-frame is IDR frame"。后果：

- `--keyint 30`：I 帧在第 0/30/60 帧，但只有第 0 帧是 IDR（ffprobe `key_frame` = 1 个）。
- 这意味着解码器可以正常解码/播放，但**随机访问/错误恢复点只有 IDR**：MP4 里每个
  GOP 只有第一个 IDR 是可 seek 的 sync sample。
- 解决办法（本模块已提供，二选一，不必改内核）：
  1. 调用方在需要的帧上把 `elish_enc_push_nv12(..., force_keyframe=true)`；
  2. 设 `ELISH_ENC_FORCE_IDR=N`，编码器内部每 N 帧发一次
     `V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME`。
  `main.c record --keyint N` 已按每 N 帧强制一次（见 §3.4）。

### 6.2 H.264 4208x3120 = level 5.1 标称超限（可编码、可解码）

- 4208x3120 = 51285 个宏块；H.264 level 5.1/5.2 的 MaxFS 都是 36864 MB。
  即该尺寸在 H.264 规范里需要 level 6.0+，而 mainline 驱动 `h264_level` 菜单
  最大值只到 5.1（`venc_ctrls.c`），固件 caps 也只声明到 level 5.2。
- 实测：**编码成功、ffmpeg 软件解码成功、90 帧完整**，SPS 标 level_idc=51。
  严格合规的播放器/硬件解码器可能按 level 拒绝，普通软件播放器不受影响。
- 需要“规范合规”的 H.264 全分辨率替代方案：
  - 用 **HEVC**（推荐，同样码率下画质更好，4208x3120 是 level 6.0/6.2 合法尺寸）；
  - H.264 缩到 ≤ 4096x2304（level 5.2 上限）或 3840x2160 录制；
  - 或对 H.264 码流做 SPS level_idc 后处理（ffmpeg
    `-bsf:v h264_metadata=level=6.2`；本模块默认不做，避免改动码流）。
- 作为记录：本轮 4208x3120 H.264 **没有**失败，因此没有失败 errno 可记；H.264 的
  唯一实测限制是码率欠冲（§5）和 level 标称。

### 6.3 病态内容会被固件静默跳过（重要限制）

用“均匀随机噪声”（`--synthetic noise`，Y+UV 全部随机）喂 4208x3120 时：

- 固件对随机噪声即使 QP 拉到最大也压不进目标码率，单帧可达 > 9.94 MB；
- 驱动给每个 capture buffer 的大小是 `ALIGN(w,128)×ALIGN(h,32)×3/2/2` =
  **9,936,896 B（4208x3120）**，装不下这种帧；
- 结果：firmware 返回 **empty（bytesused=0）的 capture buffer**，即静默跳帧；
  `dmesg` 无任何 venus 错误，也没有 `V4L2_BUF_FLAG_ERROR`。实测 90 帧里 6 帧
  没有对应 capture buffer（flush 超时），日志为
  `venus: warning: firmware returned an empty (not coded) frame ...`
  与 `venus: flush incomplete: 84 of 90`。
- 3280x2464 的随机噪声（capture buffer 6.15 MB）能装下，实测 30 帧正常。
- 真实传感器内容（testdata RAW10）与正常合成图案远达不到这个量级（4208 HEVC
  平均 84 KB/帧），**不影响验收与正常录制**。
- 实验性绕行：`ELISH_ENC_CAP_MB=<n>` 可在 `S_FMT` 时请求更大的 capture
  buffer（驱动取 max(请求值, 9.94MB)）。注意实测把 4208 的 buffer 加到 24 MB 后
  固件对这些超大帧的编码耗时/行为变得不可预测（本次 6 帧 0.5s 内没吐出一帧），
  因此该环境变量只作诊断用，不建议生产使用。

### 6.4 其他

- **冷启动首帧 ~0.85 s**（Venus 固件首次加载）。之后 `elish_enc_start()` 后首帧
  56–93 ms。主管线应在开流后丢弃/容忍这第一帧的延迟，`elish_enc_push_nv12`
  本身在预热后 p95 < 13 ms。
- 缓冲区数量固定 4 个 capture + 4 个 output（`min_number_of_output_buffers=4`）。
  4208x3120 时 4×19.9 MB input + 4×9.9 MB output ≈ 118 MB 显存/系统内存。
- 编码器固件在 4208x3120 下比 3280x2464 慢约 30–40 %（90 vs 135 fps），与
  architecture.md 的 GStreamer 基线趋势一致（94 vs 125 fps），不是本模块开销。
- 本模块只使用 `/dev/video16`，测试脚本全部要求
  `flock -w 60 elish-camera/.locks/venus.lock`；不触碰相机/loopback 设备。

## 7. 公共 API 实现情况（与 include/elish_camera.h 完全一致）

```c
elish_encoder *elish_enc_create(elish_codec codec, int width, int height, int fps,
                                int bitrate_bps, const char *es_path);
int  elish_enc_start(elish_encoder *enc);
int  elish_enc_push_nv12(elish_encoder *enc, const uint8_t *nv12, int stride,
                         uint64_t pts_ns, bool force_keyframe);
int  elish_enc_stop(elish_encoder *enc);      /* flush + STREAMOFF + 关文件 */
void elish_enc_destroy(elish_encoder *enc);
int  elish_mux_mp4(const char *es_path, const char *mp4_path, elish_codec codec);
```

内部实现要点：

- `create`：open(O_RDWR|O_NONBLOCK) → QUERYCAP 校验 M2M → S_FMT capture(codec) →
  S_FMT output(NV12) 读回真实 128/32 对齐尺寸与 stride → S_SELECTION crop 到请求尺寸 →
  S_PARM fps → 控件（CBR/VBR、bitrate、peak、GOP、profile/level）→ 打开 ES 文件。
- `start`：两队列各 REQBUFS×4 + mmap，capture 全部入队，STREAMON capture→output；
  output 用内部 free-list 管理（**首个 push 前没有可 DQBUF 的 output buffer**，
  必须自己记录“尚未首次入队”的索引，否则第一次 push 会死等）。
- `push`：先 drain 已完成的 capture（写 ES、重新入队）→ 需要时发 force-IDR 控件 →
  取空闲 output buffer（free-list 或 DQBUF，必要时 poll ≤2 s）→ 按行拷 NV12 到
  padded buffer（宽 4208 拷 4208 B/行，UV 在 `stride*out_h`）→ QBUF（bytesused =
  `stride*out_h*3/2`）→ 再 drain。
- `stop`：flush（等待 encoded==pushed，每帧一个 capture buffer，B 帧为 0 不会死锁）→
  STREAMOFF output/capture → 释放 buffer → fflush/fsync/close ES。可重复调用。
- `mux_mp4`：fork/exec `ffmpeg -framerate $ELISH_MUX_FPS -f h264|hevc -i es
  -c copy -bsf:v setts=... -movflags +faststart mp4`，返回 ffmpeg 退出码。
- `src/venus_enc_internal.h`（非公共 API）：`elish_enc_get_stats()` 给
  `tools/enc_test` 提供 encoded/keyframes/skips/bytes/padded 尺寸等计数。

## 8. 给主管线（main.c）的备注

- `elish-camera record --keyint N` 已用 `force_keyframe` 每 N 帧强制 IDR，行为正确；
  若还想让驱动的 GOP I 帧与 `--keyint` 对齐，可在 create 前
  `setenv("ELISH_ENC_GOP", ...)`（否则默认 GOP = fps = 1 s）。
- `--keyint 0` 会让 main.c 里 `frames % o->keyint` 除零（SIGFPE），建议在参数解析里
  拒绝 0 或改成 `< 1 → 1`（main.c 属 Lead，未改）。
- `elish_mux_mp4()` 的 MP4 帧率取 `ELISH_MUX_FPS`（默认 30）；main.c 若支持
  `--fps 15/60`，建议在 mux 前按实际 fps 设置该环境变量。
- 录制循环里 `elish_enc_push_nv12()` 是同步拷贝（4208x3120 每帧约 20 MB），
  在 GPU debayer 之后调用时实测 p95 8 ms，可以和 `elish_lb_push_nv12()` 串行执行，
  不会把采集循环拖到 30fps 以下（见 §3.2）。

## 9. 复现实验的完整命令

```bash
cd elish-camera && make

# 4 组验收（30fps 节拍，90 帧）
for cfg in "hevc 4208x3120 testdata/rear-4208x3120-sgrbg10.raw 20000000" \
           "h264 4208x3120 testdata/rear-4208x3120-sgrbg10.raw 20000000" \
           "hevc 3280x2464 testdata/front-3280x2464-sgrbg10.raw 16000000" \
           "h264 3280x2464 testdata/front-3280x2464-sgrbg10.raw 16000000"; do
  set -- $cfg
  ext=$([ "$1" = hevc ] && echo h265 || echo h264)
  flock -w 120 .locks/venus.lock -c \
    "./build/enc_test --realtime --codec $1 --size $2 --fps 30 --bitrate $4 \
       --frames 90 --keyint 30 --raw10 $3 --out /tmp/out-$1-$2.$ext"
done

# 解码 + MP4 校验
ffmpeg -v error -i /tmp/out-hevc-4208x3120.h265 -f null -
tools/mux_mp4.sh -f 30 -c hevc /tmp/out-hevc-4208x3120.h265 /tmp/out.mp4

# 强制 IDR 验证（IDR 应出现在 0,10,20,...,80）
ELISH_ENC_FORCE_IDR=10 ./build/enc_test --codec hevc --size 3280x2464 \
  --frames 30 --keyint 300 --raw10 testdata/front-3280x2464-sgrbg10.raw \
  --out /tmp/idr.h265
ffprobe -v error -select_streams v:0 -show_entries frame=key_frame -of csv=p=0 \
  /tmp/idr.h265 | awk '$1==1 {print NR-1}'
```
