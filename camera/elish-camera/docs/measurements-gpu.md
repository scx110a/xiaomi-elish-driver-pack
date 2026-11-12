# GPU debayer 实测（elish / Adreno 650 "FD650"）

## 1. 环境与 renderer

- Xiaomi Pad 5 Pro (SM8250), Armbian noble, kernel 6.18.44；Mesa 25.2.8-0ubuntu0.24.04.4。
- EGL surfaceless（**无显示器**）：`gbm_create_device(/dev/dri/renderD128)` +
  `eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR)` + `eglMakeCurrent(EGL_NO_SURFACE…)`，
  渲染全部进 FBO。实测 `GL_RENDERER=FD650`，`GL_VERSION=OpenGL ES 3.2 Mesa 25.2.8`，
  GLSL ES 3.20，`GL_MAX_TEXTURE_SIZE=16384`（两传感器 stride 4112/5264 均可上传）。
- 工具：`build/debayer_test`（见 §6）。测量时系统有其他负载（Firefox/整机联调，
  load average ≈5），下表是**同一窗口内 mean/p95**，括号内为多次运行中最好值。
- **Bayer**（按「两个 G 位置均值应相等」实测）：前摄 OV8856 = **BGGR**，后摄 OV13B10 =
  **GRBG**；`capture.c` 已分别设置。合成 RAW10 测试四种 pattern 均能还原参考色块。

## 2. NV12 全流程耗时（含 GPU 渲染 + 回读到 CPU NV12 缓冲 + stride 组装）

| 相机 / 原始尺寸 | rotate | 输出尺寸 | mean | p95 | 最好 mean |
|---|---|---|---|---|---|
| 前摄 3280x2464 BGGR | 0   | 3280x2464 | 11.0 ms | 14.1 ms | 10.7 ms |
| 前摄 3280x2464 BGGR | 180 | 3280x2464 | 11.9 ms | 14.7 ms | 10.7 ms |
| 前摄 3280x2464 BGGR | 270 | 2464x3280 | 12.1 ms | 13.5 ms | 11.9 ms |
| 后摄 4208x3120 GRBG | 0   | 4208x3120 | 19.0 ms | 22.1 ms | 18.3 ms |
| 后摄 4208x3120 GRBG | 180 | 4208x3120 | 19.2 ms (p50) | 22.0 ms (最好) | 18.7 ms |
| 后摄 4208x3120 GRBG | 270 | 3120x4208 | 19.1 ms | 19.9 ms | 18.5 ms |

- rot90/270 与 rot0/180 差异 <5%（旋转是 shader 内坐标映射，无额外 pass）。
- 阶段分解（前摄，`ELISH_GPU_PROFILE=1`，profile 会插 `glFinish` 略偏悲观）：
  upload 2.2 / unpack 1.3 / Y pass 3.1 / UV pass 2.2 / sync 0.5 / memcpy 1.3 / syncend 0.5 ms。
  后摄对应 ≈ **upload 3.6 / unpack 2.0 / Y 5.8 / UV 4.4 / sync+memcpy 4.5** ms。
- RGB888 静照：前摄 **12.5–13.2 ms**，后摄 **21–26 ms**（RGBA8 目标 32/52 MB + NEON RGBA→RGB）。

## 3. 场景统计 `elish_gpu_stats`（修正后）

- 修复了两个 bug：① y 方向隔行采样导致只覆盖单一行奇偶的 block 只统计到 R 或 B，
  R/B 被稀释成 ~1/2；② 单 shader 动态长循环在 fd650 上极慢（20–50 ms）。
  现在 = 三趟固定 4x4 kernel 的 GPU 降采样（1024x1024 block 均值 → 256 → 64 → 回读 16 KB）。
- 量纲（与 Lead/main.c 约定）：**先减黑电平 64/1024 并 clamp≥0，再乘 WB gain，得到
  pre-gamma 线性均值，10-bit RAW/4 → 0..255 标度**；`mean_y` = Rec.601 加权。
- 实测（wb=1，front BGGR raw，应≈24.8/40.2/28.5）：**R=24.88 G=40.14 B=28.66 Y=34.27**
  （独立 numpy 参考 24.86/40.14/28.67）；后摄 GRBG：R=51.16 G=104.50 B=64.13
  （参考 51.22/104.63/64.15）。apply AWB gain 后三通道均值相等（≈40.1 / ≈104.5）。
- 单次耗时（warm）：**≈5.6–7.8 ms**，其中统计本体（pass A 0.86 + B1 0.58 + B2 0.26 +
  readback 0.28 ms ≈ 2.0 ms）+ 共用的 RAW 上传 2.2 ms + unpack 1.3 ms。
  main.c 每 8 帧调一次 → 摊到每帧 ~0.7 ms。

## 4. dma-buf 回读与 CPU cache 一致性

- **关键优化**：`glReadPixels` 在本机只有 **~0.8–0.9 GB/s**（R8 全帧 13.1 MB 要 17 ms，
  RGBA8 49 MB 要 61.5 ms，PBO+map 也 ~1 GB/s）。改为 `/dev/dma_heap/system` 分配 dma-buf，
  `EGLImage` 导入当 render target，其 CPU mmap 是 **cached**：13 MB memcpy 仅 **1.2–1.3 ms
  (~10 GB/s)**。Y+UV 放在同一 dma-buf（两个 EGLImage，对齐 offset），每帧只一对
  `DMA_BUF_IOCTL_SYNC(START/END, READ)`，再按行拷进调用方 NV12 缓冲。
- 一致性验证（`--verify 60`）：phase 1 连续 60 帧同输入，输出 checksum 恒定
  **PASS 0/59**；phase 2 两种输入逐帧交替 120 帧，每帧 checksum 与对应输入一致
  **PASS 0/118**；stats 也随输入变化。前后摄均 PASS → 无旧帧/花屏。
- 若用 glReadPixels 回读，前摄全流程会变成 ~24–27 ms、后摄 ~35–40 ms（无法达 30fps 预算），
  这是本模块选择 dma-buf render target 的直接原因。

## 5. 结论：30 fps 预算

33.3 ms/帧预算下：前摄 GPU 模块 10.7–12 ms、后摄 18.5–19.5 ms，余量留给 Venus
（实测 3280x2464 131 fps / 4208x3120 92 fps）与 loopback push → **满足 30fps**。
整机联调实测前摄 live 13–16 ms、后摄 live 20–21 ms（有 Firefox 负载时 30–50 ms，属争用）；
`--no-ae` 端到端稳定 30.03 fps。后续可再优化：① 直接输出 dma-buf 给 Venus/loopback
（省 CPU 回读 ~2.5 ms，需扩展 API）；② 直接导入 V4L2 raw dma-buf 免上传（~2–3.6 ms）；
③ compute shader 融合 unpack+debayer（省 1.3 ms 与 16 MB 中间纹理）；④ UV pass 用
textureGather/2x2 box；⑤ stats 直接从 packed 解码以省掉共用 unpack。

## 6. 复现

```bash
make build/debayer_test
# 前摄（BGGR，180°）：NV12 计时 + RGB/NV12 PNG 素材
build/debayer_test testdata/front-3280x2464-sgrbg10.raw --input 3280x2464 --stride 4112 \
  --bayer bggr --rotate 180 --frames 30 --warmup 12 --wb 1.613,1.0,1.400 --stats \
  --out-ppm /tmp/f.ppm --out-nv12-ppm /tmp/f-nv12.ppm
# 后摄（GRBG，270°）
build/debayer_test testdata/rear-4208x3120-sgrbg10.raw --input 4208x3120 --stride 5264 \
  --bayer grbg --rotate 270 --frames 30 --warmup 12 --stats
# 一致性/花屏检查；阶段耗时
build/debayer_test testdata/front-3280x2464-sgrbg10.raw --input 3280x2464 --stride 4112 --verify 60 --no-timing
ELISH_GPU_PROFILE=1 build/debayer_test <...> --frames 1
```

## 7. 目视验证（AWB 用灰世界增益，仅用于看图；PNG 在 testdata/out/）

- `testdata/out/front-3280x2464-rot180-bggr-awb.png`（RGB888 路径，3280x2464）
  与 `...-nv12-half.png`（NV12 解码，半分辨率）：室内天花板/人物方向正确，灰墙中性、
  肤色自然、无绿偏；NV12 与 RGB 目视一致。
- `testdata/out/rear-4208x3120-rot270-grbg-awb.png`（RGB888，3120x4208）
  与 `...-nv12-half.png`：货架/墙/印刷品方向正确，颜色自然。
- 未做 AWB 时（wb=1,1,1）整体偏色属正常（前摄偏紫、后摄偏青），由整机 AWB 修正。
