# Android vs Linux 设备树对比（按本机实际硬件过滤 — 小米平板 5 Pro WiFi 版）

> 设备：Xiaomi Mi Pad 5 Pro **WiFi 版（无 5G）**，SM8250 / elish，CSOT 11" 面板
> 日期：2026-11-10（最终版）
> 方法：解出本机 Android 完整运行时设备树（`vendor_boot_a` 基础 DTB + `dtbo_a` 中匹配 elish 的
> overlay），再对照 Linux DTB、内核驱动源码、`.config` 和运行时 `/sys`，并用本机规格过滤。

---

## 0. 结论（最终）

本机**硬件存在但 Linux 未驱动**的只剩 **三类**：

1. **摄像头**（后 1300万 + 500万景深，前 800万；含 EEPROM/对焦/闪光灯/PM8008 相机 PMIC）
2. **传感器**（环境光、加速度、陀螺仪、电子罗盘；霍尔已由 `gpio-keys` 的 lid/tablet-mode 覆盖）
3. **GNSS**（Broadcom BCM4775；WiFi 版同样支持 GPS/GLONASS/北斗/伽利略/QZSS）

其余全部剔除：**5G 调制解调器、NFC、无线充电、红外、触觉马达、FPC 指纹、外接显示**
——这些在本机（WiFi 版）不存在；原因是 Android 的 `dtbo_a` 是 **elish 系列共用**的，
里面为 5G/其他配置留了节点（部分 enabled），本机并没有对应芯片。

---

## 1. 本机确认存在的硬件

| 类别 | 硬件 | 依据 |
|---|---|---|
| 显示/触摸 | CSOT 11" 2560×1600 120Hz、Novatek NT36523 | 规格 + Android DT ok |
| 摄像头 | **后 1300万 + 500万景深，前 800万**（3 摄 + EEPROM/对焦/闪光灯/PM8008） | WiFi 版规格 + Android DT ok |
| 音频 | 8 扬声器（8× CS35L41） | 规格 + DT ok（Linux 已驱动） |
| 传感器 | 环境光、加速度、陀螺仪、电子罗盘、霍尔 | 规格；Android 跑在 SLPI 子系统 |
| GNSS | Broadcom BCM4775（GPS/GLONASS/北斗/伽利略/QZSS） | WiFi 版规格明确支持 + Android DT `/soc/bcm4775` ok |
| 指纹 | Goodix GF3626ZS1（侧边/电源键） | 本项目 + DT ok |
| 键盘/笔 | Pogo 磁吸键盘、磁吸笔 | 规格 + Linux 已工作 |
| 电池/充电 | 8600mAh、67W 有线（bq27z561×2 + bq2597x×2 + pm8150b） | 规格 + DT + Linux 已驱动 |

---

## 2. 真实存在、但 Linux 未驱动（最终清单）

| # | 设备 | Android 侧 | Linux 侧 | 结论 |
|---|---|---|---|---|
| 1 | **摄像头全套**（后主摄/景深、前摄 + EEPROM + 对焦 + 闪光灯 + PM8008） | 3× `qcom,cam-sensor` + 3× eeprom + actuator + 3× camera-flash + pm8008 均 ok | `camss@ac6a000` **disabled**；无 CCI/sensor/闪灯节点；`CONFIG_MFD_QCOM_PM8008` 未开；无 `/dev/media*` | ❌ **未驱动** |
| 2 | **传感器**（环境光/加速度/陀螺仪/电子罗盘） | 规格有；Android 由 **SLPI**（传感器 DSP）驱动，AP DT 无节点 | Linux `slpi` remoteproc **offline**，无 AP 侧传感器驱动/DT | ❌ **未驱动** |
| 3 | **GNSS**（BCM4775） | 规格明确支持 GPS/GLONASS/北斗/伽利略/QZSS；`/soc/bcm4775` ok | 无 GNSS 驱动/DT（主线 GNSS 仅 MTK/SiRF/u-blox） | ❌ **未驱动** |

补充：
- **霍尔**已部分驱动：Linux `gpio-keys` 有 `switch-lid`、`switch-tablet-mode`（对应 Android 的 `hall_key`/`hall_key1`）。
- **闪光灯 LED** 已驱动（`/sys/class/leds/white:flash`）；缺的是相机本体。

---

## 3. 已剔除：本机不存在的硬件

| 设备 | Android 设备树判决 | 本机规格（WiFi 版） | 处理 |
|---|---|---|---|
| **5G 调制解调器** | elish 共用 DT 含 modem 相关配置 | **无 5G**（用户确认 WiFi 版） | ✅ 剔除 |
| **NFC**（`qcom,nq-nci`） | `/soc/i2c@984000/nq@28` **disabled** | 无 | ✅ 剔除 |
| **触觉马达**（`aw8697`） | 节点 **disabled** | 无振动 | ✅ 剔除 |
| **红外**（`ir-spi`） | 节点 ok，但父 `/soc/spi@988000` **disabled** | 无 | ✅ 剔除 |
| **无线充电**（`idt,p9418`） | 节点 ok（共用 DT） | 仅 67W 有线，无无线充 | ✅ 剔除 |
| **FPC 指纹**（`fpc,fpc1020`） | 变体节点 | 本机 Goodix | ✅ 剔除 |
| **外接显示/写回** | Android 显示栈节点 | 无 DP 输出 | ✅ 剔除 |
| **Android 专有充电栈**（smb5/smb1390/ln8000…） | Android 使用 | — | ✅ 不算缺口：Linux 用 `pm8150b-charger + bq2597x + bq27z561` 替代，充电正常 |
| **PMIC 附属功能**（adc-tm5/pwm-lpg/tri-led/qnovo5/bcl…） | Android 使用 | — | ✅ 非用户可见，温度由 `qcom_tsens` 覆盖 |

---

## 4. 已正常驱动的设备（对照）

触摸 NT36523、显示面板、背光 KTZ8866、双电池 BQ27Z561、充电 PM8150B+BQ2597X+Type-C、8×CS35L41 音频、WiFi/BT QCA6390（ath11k + btqca）、指纹（本项目）、磁吸键盘/鼠标（USB HID）、音量键 + 霍尔盖/平板模式开关、Venus 硬解、GPU、UFS、PCIe、PMIC（regulator/RTC/ADC/thermal/闪光灯）。

---

## 5. 继续补驱动的难度（只剩三类）

| 设备 | 工作量 | 说明 |
|---|---|---|
| 相机 | 大 | 启用 CAMSS + 补齐 CCI/CSIPHY/PM8008 连线 + 为具体 sensor 写驱动（型号需从 Android 固件/DT 再确认）|
| 传感器（ALS/加速度/陀螺/罗盘） | 大 | 高通方案传感器在 SLPI 固件里，需先让 Linux 加载 SLPI（`remoteproc@5c00000` 目前 offline）并实现 SLPI↔AP sensor 通道；或确认传感器是否挂在 AP 的 i2c 上另写驱动 |
| GNSS（BCM4775） | 大 | 需写 BCM4775 GNSS 驱动 + 固件加载（无主线参考）|

> 结论：本机除相机、传感器、GNSS 外，Linux 已驱动全部外设。这三类都属于大工程，与指纹项目无关。

---

## 6. 复现方法（从本机分区解 Android 设备树）

```bash
# 1) vendor_boot_a -> 基础 DTB
python3 - <<'PY'
import struct
d=open('/dev/disk/by-partlabel/vendor_boot_a','rb').read(4096)
ver,page=struct.unpack('<II',d[8:16]); rsize=struct.unpack('<I',d[24:28])[0]
hsize,dsize=struct.unpack('<II',d[2096:2104])
off=page+((rsize+page-1)//page)*page
base=open('/dev/disk/by-partlabel/vendor_boot_a','rb').read()[off:off+dsize]
open('/tmp/android-base.dtb','wb').write(base)
PY

# 2) dtbo_a -> 29 个 overlay（DTBO 表是大端），ov-12 = xiaomi elish
python3 - <<'PY'
import struct
db=open('/dev/disk/by-partlabel/dtbo_a','rb').read()
magic,total,hs,es,ec,eo,pg,ver=struct.unpack('>8I',db[:32])
for i in range(ec):
    e=db[eo+i*es:eo+(i+1)*es]
    sz,off,_,_=struct.unpack('>IIII',e[:16])
    open(f'/tmp/ov-{i:02d}.dtb','wb').write(db[off:off+sz])
PY

# 3) 合并出 Android 运行时 DT
fdtoverlay -i /tmp/android-base.dtb -o /tmp/android-elish.dtb /tmp/ov-12.dtb
fdtget -t s /tmp/android-elish.dtb /soc/i2c@984000/nq@28 status        # disabled
fdtget -t s /tmp/android-elish.dtb /soc/i2c@a8c000/aw8697_haptic@5A status  # disabled
fdtget -t s /tmp/android-elish.dtb /soc/spi@988000 status              # disabled
```

> 只读分区安全（用户属 `disk` 组即可读）；不要写 `boot_*`/`vendor_boot_*`/`dtbo_*`。
