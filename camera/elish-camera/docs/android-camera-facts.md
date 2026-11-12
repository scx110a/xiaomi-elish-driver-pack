# Android vendor 镜像相机取证（read-only，2026-11-12 晚）

> 来源：你自己的 Android vendor 分区镜像（1.5G ext4，volume "vendor"）只读挂载：
> `sudo mount -o ro,loop ~/vendor_a.img /mnt/android-vendor`。本文「已验证」结论均可用
> 文末命令复现，标「待验证」的不要当事实用。Chromatix 解析器 `docs/evidence/chromatix_parse.py`，
> EEPROM 解析器 `docs/evidence/eeprom-24c64-parse.py`（工具：strings/readelf/objdump/nm/python3）。

## 0. 文件清单（关键，均位于 `/mnt/android-vendor/lib64/camera/`）

| 文件 | 大小 | 作用/已确认 |
|---|---|---|
| `com.qti.sensor.elish_ov8856.so` | 11,352 | 前摄 sensor lib（stub）：导出 `GetSensorLibraryAPIs`；mini 符号仅 `CalculateExposure/FillExposureSettings`；**不含 mode 表/CFA** |
| `com.qti.sensor.elish_ov13b10.so` / `..._hi556.so` | 11,048 / 11,064 | 后摄/景深同上 |
| `com.qti.sensormodule.elish_aac_ov8856.bin` | 91,168 | 前摄 QTI Chromatix sensor module |
| `com.qti.sensormodule.elish_ofilm_ov13b10.bin` | 80,392 | 后摄同上 |
| `com.qti.sensormodule.elish_qtech_hi556.bin` | 124,904 | 景深同上 |
| `com.qti.eeprom.elish_aac_ov8856_p24c64f.so` | 11,032 | 导出 `GetEEPROMLibraryAPIs`（mini 符号 `GetChecksumStatus`） |
| `com.qti.eeprom.elish_ofilm_ov13b10_bl24sa64.so` / `..._qtech_hi556.so` | 11,040 / 11,032 | 同上 |
| `com.qti.actuator.elish_cn3927_ov13b10.so` | 11,016 | AF VCM：导出 `GetActuatorLibraryAPIs`（mini 符号 `GetParkLens`） |
| `com.qti.tuned.elish_*_front/wide/depth.bin` | 9.9M/20M/1M | chromatix 调优（AWB/LSC/demosaic），非逐机标定 |
| `lib64/hw/camera.qcom.so` | 9.3M | CamX HAL（含 IFE/CSID/Titan480；.gnu_debugdata 可解出完整符号表） |
| `etc/camera/camxoverridesettings.txt` | 1KB | CamX overrides（见 §7） |

文件 sha256：`docs/evidence/vendor-camera-files.txt`。

## 1. Bayer 顺序 / 方向

- **vendor_a.img 内没有 CFA/mountAngle 配置值（已验证=否定证据）**：
  - 三个 `sensormodule*.bin` 的节点名全量解析（853/748/1170 个，name 字段 24 字节）中，
    **没有** `colorFilter/bayer/mountAngle/cameraPosition/sensorOrientation` 任一字段；
  - CamX HAL 里确实有这些字段名（`SensorInfoColorFilterArrangement`、`mountAngle`、
    `cameraPosition`、`TranslateColorFilterArrangement`），但全镜像 `grep -rail 'mountAngle'`
    只命中 8 个 `.so`，**没有任何配置数据**承载它们 → 该配置来自 kernel DTB(dtbo)/odm，不在 vendor。
  - `vendor_a.img` 里无 DTB（全镜像扫 FDT magic `d00dfeed` 无命中；`/vendor/odm` 是指向 `/odm` 的符号链接）。
- **sensor 输出格式本身不含 CFA**：`streamConfiguration` 只有 `format/type=0x2B`（MIPI CSI-2 RAW10
  data type）+ `bitWidth=10`，与 Bayer 顺序无关。
- **结论（实测裁决，Lead 2026-11-12）**：前摄 OV8856 = **BGGR**（G-pair diff 0.0，另一配对 15.2）；
  后摄 OV13B10 = **GRBG**（G-pair diff 1.2）。已写入抓帧管线按相机分别配置。
  → 仓库内 `architecture.md` 里「前摄按 GRBG 解码」的说法**作废**，以前摄 BGGR / 后摄 GRBG 为准。
- 方向：vendor 无 mountAngle；Android 侧只有 OIS 角度范围 `maxRoll/Pitch/YawDegree = 360.0`（double，
  非 mountAngle）。方向以实测画面为准。

## 2. 前摄 OV8856（读自 `com.qti.sensormodule.elish_aac_ov8856.bin`）

- sensorName=`elish_ov8856`；moduleName=`elish_aac_ov8856_front`；EEPROMName=`elish_aac_ov8856_p24c64f`。
- **data-lanes**：`laneAssign = 0x10`。QTI 该字段为「每逻辑 lane 一个 nibble」，`0x10` = lane0→0、lane1→1
  ⇒ **2-lane**（与 mainline 一致）【高置信】。
- **MCLK**：`powerSetting`(0x90) 内出现 `19200000`（19.2 MHz）⇒ **MCLK=19.2MHz**（三个 sensor 相同）【高置信】。
- **模式/时序**：`streamConfiguration = {format/type=0x2B, bitWidth=10, width=3264, height=2448}`；
  `resolutionData` 内 `pixelClock=254400000`、double `fps≈30.03`；`downScaleFactor=1.0`。
  ⇒ **Android 原生模式 = 3264×2448@~30fps RAW10**（注意：与 mainline 现用 3280×2464 不同；OV8856
  两种分辨率都支持，但 Android 这套配的是 3264×2448）【已验证字段 / 30fps 高置信】。
- 曝光/增益寄存器地址表（写在 sensorDriverData blob，可对照 mainline）：`0x3500`(long exp)、
  `0x3508`(a-gain)、`0x3808/0x380A`(X/Y out size)、`0x380C/0x380E`(HTS/VTS)、`0x4322~0x4328`。
- 板级信息：**CSI PHY 编号、reset/standby GPIO、供电时序不在 vendor**（sensor module XML 里没有
  powerUpSequence 节点；`powerSetting` 只有数值型 configType）→【待验证，需 DTB】。

## 3. 后摄 OV13B10（`com.qti.sensormodule.elish_ofilm_ov13b10.bin`）

- sensorName=`elish_ov13b10`；moduleName=`elish_ofilm_ov13b10_wide`；
  **actuatorName=`elish_cn3927_ov13b10`（CN3927V，与 mainline 一致）**；
  EEPROMName=`elish_ofilm_ov13b10_bl24sa64`（BL24SA64，24c64 类 8KB，mainline 用 atmel,24c64@0x51）；
  flashName=`elish_pmic_flash`（PMIC flash）。
- **data-lanes**：`laneAssign = 0x3210` = nibble[lane0..3]=0,1,2,3 ⇒ **4-lane**（与 mainline 一致）【高置信】。
- **MCLK=19.2MHz**（同上）【高置信】。
- **模式/时序**：`streamConfiguration = {type=0x2B, bitWidth=10, width=4208, height=3120}`；
  `pixelClock=448000000`、double `fps≈30.04`、`downScaleFactor=1.0`
  ⇒ **Android 原生模式 = 4208×3120@~30fps RAW10**【已验证字段 / 30fps 高置信】。
- `PDConfigData` 存在（size 0x58，PDAF 配置）→ 后摄支持 PDAF。
- AF 驱动参数在 `actuatorDriver`(0x100)/`registerParam`/`deInitSettings` 节点；`GetParkLens` 说明
  Android 上电/切换时会 park lens（mainline 无此步，对应「AF 初值 0 导致失焦」的坑）。
- `powerUpSequence`/`powerDownSequence`/`i2cInfo` 节点存在但 **size=0** ⇒ 板级上电时序/地址不在 vendor
  【待验证】。mountAngle 同 §1【待验证】。

## 4. EEPROM 校准结构（新，重点）——后摄 8KB dump 已解析出 LSC 表

样本：从**自己设备**读取的 8KB 后摄 EEPROM dump（8192 B，mainline at24 读取，未加密；
本包不附带该 dump 及其解析输出，读取方法见 `camera/README.md` §EEPROM；解析器 `docs/evidence/eeprom-24c64-parse.py`）。

**已验证布局**（按 0xFF 填充切段）：

| 偏移 | 长度 | 内容 | 置信 |
|---|---|---|---|
| 0x0000–0x006B | 108 B | header：模块信息/AF/AWB 等标定字段区（含 `0xFFFF`=未写入哨兵）；**逐字段含义未完全解码** | 待验证 |
| 0x006C–0x0753 | 1768 B | **LSC（镜头阴影）表**：4 通道 × 17×13=221 mesh，通道**交错**存放（stride 4），**大端 u16**；中心=1023，四角 232–264，4 通道相关系数 0.9988–0.9997 | **已验证** |
| 0x0754–0x0EC8 | 1908 B | 0xFF 填充 | 已验证 |
| 0x0EC9–0x12BD | 1013 B | 第二标定块（内部自 0xED8 起 ~998 B 数据；疑似 17 列 2D mesh/多条件标定） | 待验证 |
| 0x12BE–0x1FA0 | 3299 B | 0xFF 填充 | 已验证 |
| 0x1FA1–0x1FA3 | 3 B | 标记 `80 00 91`（疑似 CRC/结束标记） | 待验证 |
| 0x1FA4–0x1FFF | 92 B | 0xFF | 已验证 |

**LSC 实测网格**（channel0，17 列×13 行，行优先，单位 Q10 增益，1023=1.0）：
中心 1023、四角 0.23–0.26，径向平滑 → 典型 rolloff，判定为 LSC 无疑。
4 通道顺序**未验证**（QTI 惯例 R/Gr/Gb/B；需用平面场/色卡反推）。

**AWB/AF 校准**：header 区（108 B）与第二块（1013 B）应是 AF/AWB/模块信息，但当前样本中
`0xFFFF` 哨兵很多，且无明文 magic/结构（厂商私有打包）→ **字段布局待验证，禁止硬猜**。
可用的 vendor 侧辅助证据：`EEPROMDriverData` 节点（ov8856 size 0x8F0；含 `EEPROMName`/`calibrationInfo`/
`actuatorID`/`lightInfo`/`AFSyncInfo`/`DCC*` 子节点引用），其中出现过 `221`（=LSC mesh 节点数）与 `470`、`3072`
等数值，可作为 OTP 布局线索；但实际 OTP 字节偏移由 CamX `EEPROMData` 类在运行时按编译好的表解析，
vendor 镜像里没有可读的偏移表【已验证“没有”，布局待验证】。

**硬件验证方法**（建议）：① 用 `eeprom-24c64-parse.py` 提 4×17×13 LSC，与 tuned bin 的 `mod_lsc*`
参数/平面场暗角对比（corner/center 增益比一致即确认）；② header 0x00–0x6B 可做单字节扰动实验，
但需写 EEPROM，本任务禁止，请 Lead 决定；③ 若只需 LSC 提升全彩画质，直接把 0x6C 表接进
GPU debayer 后的 shading 校正即可，**不必先解出 AF/AWB**。

## 5. Hi556 景深（新，重点）——上电/时序证据

- 身份：sensorName=`elish_hi556`；moduleName=`elish_qtech_hi556`；EEPROMName=`elish_qtech_hi556`；
  调优 `com.qti.tuned.elish_qtech_hi556_depth.bin`。**确认是 Hi556 且带 EEPROM**（已验证）。
- 模式：`streamConfiguration = {type=0x2B(RAW10), bitWidth=10, width=2592, height=1944}`；
  `pixelClock=175872000`、fps double≈30.06；`laneAssign=0x10` ⇒ **2-lane**；MCLK **19.2MHz**。
  `moduleConfiguration` 内参：f≈1.94 mm、f/2.4、FOV≈71°×56°（depth 用途合理）。
- **上电时序（powerSetting 0x90，按 {configType, configValue, delayUs} 三连读）——高置信结构**：
  ```
  Hi556 up  : (9,0,1) (3,1,1) (1,1,1) (2,1,1) (0,19200000,2) (9,1,2)
  Hi556 down: (9,0,1) (0,0,1) (2,0,1) (1,0,1) (3,0,1)
  ```
  对照：OV8856 up=`(9,0,1)(1,1,0)(3,1,0)(2,1,0)(9,1,1)(0,19200000,5)`；
  OV13B10 up=`(9,0,1)(1,1,0)(2,1,0)(3,1,0)(9,1,5)(0,19200000,5)`。
  - configType 语义推断【高置信】：`0`=CLK（值=19200000Hz）；`9`=GPIO（出现 0→1 变化，为 reset/standby）；
    `1/2/3`=三路 regulator（值 1=开）。**注意 Hi556 与两颗 OV 的差别**：
    - OV8856/OV13B10：**先拉高 GPIO（reset 释放）→ 再开 MCLK**
    - **Hi556：先开 MCLK(0,19200000,2) → 最后才拉高 GPIO(9,1,2)**；且其 power-down 是 **先 GPIO 拉低、再关 MCLK**。
  - 三路 VREG 的**顺序**也有线序意义（Hi556: 3→1→2；OV8856: 1→3→2；OV13B10: 1→2→3），
    但 configType→具体 rail 名（VIO/VANA/VDIG）**不在 vendor 里**，需 Android DTB【待验证】。
  - 另一份相同的 0x90 块（位于 bin 末尾 EEPROM 段）delay 为 10/18，推测为 EEPROM 供电变体【待验证】。
- **I2C slave 地址不在 vendor**：`sensorSlaveAddress`/`sensorI2CFrequencyMode`/`i2cInfo` 节点
  **size 均为 0**（已验证），无其它承载 ⇒ 地址只在 Android DTB；mainline 试的 0x20/0x24 无法从 vendor 复核。
- 供电（转录自交接文档 §5.1 的 Android base DTB 证据，本镜像**无法**复核，标待验证）：
  `cam_vio→pm8150a_l1(1.8V)`、`cam_vana→pm8008_l3(2.8V)`、`cam_vdig→pm8150a_l2(1.2V)`。
- **对 mainline I2C 无应答给不出“确凿原因”（vendor 无 DTB）**；但 vendor 证据支持两个排查方向：
  1) **时序**：必须严格按 Hi556 的「三路 rail 先上 → MCLK 19.2MHz → 最后释放 reset」，且 reset 保持低电平
     至少 1µs 级；若 mainline 在 MCLK 之前就释放 reset，或 reset 极性反了，会无 ACK；
  2) **地址**：DT 里的 slave 地址与 mainline 试的 0x20/0x24 未必一致 → 需要 Android DTB 或 i2cdetect 全扫。

## 6. sensor 模式表 / fps 结论（回答 Lead 问题 3）

- **sensor .so 里没有 mode 表**：11KB stub，符号只有 `GetSensorLibraryAPIs` +
  `CalculateExposure/FillExposureSettings`（已验证：`readelf -s`/objdump）。
- **sensormodule bin 里每个 sensor 只有 1 个原生 `streamConfiguration`**（前/后/深各 1 个，见 §2/§3/§5），
  没有“每档分辨率 fps 列表”。Android 的其它输出分辨率是 IFE/CSID scaler 从原生模式派生
  （CamX 有 `availableStreamConfigurations`/`ScalerAvailableStreamConfigurations` 逻辑，属通用代码）。
- **全分辨率 30fps 证据**：`resolutionData` 中 double = 30.03/30.04/30.06 fps，配 PCLK
  254.4/448/175.872 MHz；例：OV13B10 4208×3120×30 ≈ 393.9 MP/s，/448 MHz ≈ 88% 占空比（合理）。
  `camxoverridesettings.txt` 也留有 IFE 带宽预算：`miIfeCamnocBandwidthMBytes=4317`、
  `miIfeExternalBandwidthMBytes=3746` ⇒ **全分辨率 30fps 是 Android 的既定目标**（已验证）。
- 另：`vsquality=5@30|6@30|8@30`（视频尺寸@fps）也印证 30fps 目标。

## 7. IFE 硬件 ISP / Venus 编码

- **Android 用硬件 ISP(IFE) 做 debayer**（已验证）：`lib64/hw/camera.qcom.so` 内含
  `camxifecsid.cpp`/`camxifedsx10.cpp`、`IFECSID`、`Titan480`、`demosaic`、`is_qcfa_sensor`、
  `TranslateColorFilterArrangement` 等字符串；sensor 只出 RAW10，去马赛克/线性化/降噪全在 IFE/Chromatix 里。
  ⇒ **mainline 没有 IFE 处理驱动（camss 只做 RDI raw dump），GPU debayer 是必要方案**。
- **Venus 硬件编码**（已验证）：`etc/media_codecs_kona_vendor.xml` 列有
  `OMX.qcom.video.encoder.{avc,hevc,vp8,heic}`（Venus/c2 硬件编码），与 `architecture.md` 里
  GStreamer `v4l2h265enc/v4l2h264enc` 实测吞吐一致。CamX 侧还开了 `enableGPURotation=TRUE`
  （Android 相机输出用 GPU 做旋转），说明 GPU 也参与相机后处理。

## 8. 置信度汇总 / 待验证清单
| 结论 | 置信度 |
|---|---|
| vendor 内无 CFA/mountAngle/DTB；Bayer 用实测（前 BGGR / 后 GRBG） | 已验证（含实测裁决） |
| OV8856 2-lane、OV13B10 4-lane、Hi556 2-lane（laneAssign 0x10/0x3210/0x10） | 高置信 |
| 三 sensor RAW10、原生模式 3264×2448 / 4208×3120 / 2592×1944、~30fps | 已验证 |
| MCLK=19.2MHz（powerSetting 内 19200000） | 高置信 |
| 后摄 EEPROM LSC = 0x6C 起 4ch×17×13 交错大端 u16 | 已验证 |
| EEPROM header(0x00–0x6B) AF/AWB 字段逐字段布局；block D(0x0EC9) 结构 | 待验证 |
| Hi556 上电顺序（3 rail→MCLK→reset 释放）/configType 语义 | 高置信 / rail 名待验证 |
| Hi556 I2C 地址、reset GPIO 编号与极性、CSI PHY 编号；IFE 硬件 debayer、Venus 硬编 | 待验证（DTB） / 已验证 |
## 附：可复现命令

```bash
# 只读挂载（用完 sudo umount /mnt/android-vendor）
sudo mount -o ro,loop ~/vendor_a.img /mnt/android-vendor
C=/mnt/android-vendor/lib64/camera

# 1) 解析 chromatix sensor module（打印 sensorName/laneAssign/streamConfiguration/resolutionData…）
python3 docs/evidence/chromatix_parse.py $C/com.qti.sensormodule.elish_ofilm_ov13b10.bin

# 2) 否定证据：vendor 里没有任何 CFA/mountAngle 配置（只命中 .so）
grep -rail 'mountAngle\|SensorInfoColorFilterArrangement' /mnt/android-vendor | sed 's#^#  #'
strings -a $C/com.qti.sensormodule.elish_aac_ov8856.bin | grep -iE 'bayer|colorfilter|mountAngle' || echo '无匹配'

# 3) sensor/EEPROM/actuator .so 只导出 API（无 mode 表/无 CFA）
readelf --dyn-syms -W $C/com.qti.sensor.elish_ov8856.so
readelf -s -W /tmp/arv/so/com.qti.sensor.elish_ov8856.so.symelf 2>/dev/null | head   # 见 evidence

# 4) 后摄 EEPROM：提取 LSC 4ch×17×13 网格
python3 docs/evidence/eeprom-24c64-parse.py \
  <你自己的 8KB 后摄 EEPROM dump>   # 读取方法见 camera/README.md §EEPROM

# 5) Hi556/OV 上电序列（完整输出见 evidence/power-setting-sequences.txt）
#    powerSetting 块 = n 组 {configType, configValue, delayUs}，用 chromatix_parse.py 同法定位

# 6) CamX IFE/CFA 证据
strings -a $C/../hw/camera.qcom.so | grep -oE 'camxifecsid\.cpp|camxifedsx10\.cpp|IFECSID|Titan480|demosaic' | sort | uniq -c
```
