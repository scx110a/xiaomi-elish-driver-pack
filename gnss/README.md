# GNSS 子包 — Airoha AG3335（elish / SM8250）

本子包让 Xiaomi Mi Pad 5 Pro（elish）上的 Airoha **AG3335** GNSS 在 Armbian Linux
下工作，并把 NMEA 交给 `gpsd`。**2026-11-12 已在实机打通并验证。**

- 硬件：AG3335（Airoha；Android 侧 `android.hardware.gnss@2.1-service-airoha`），
  QUP5 UART `0x994000` → `/dev/ttyHS1`
- 电源/时钟：LDO `TLMM GPIO46`、`GPS_EN TLMM GPIO158`、`PMIC PM8150 GPIO3 GPS clock`
  → 由内核模块 `airoha_gps.ko` 的 `/dev/airoha_gps` 控制
- 主机协议：Airoha **PAIR ASCII**（`/dev/ttyHS1`，**921600 8N1**）
- 架构（与 Android `anld` 同构，但数据交给 gpsd）：

```
gpsd  ← /dev/ttyGNSS0 (pty) ← airoha-gnssd ← /dev/ttyHS1 921600 ← AG3335
                                    ↕
                            /dev/airoha_gps（OPEN/CLOSE/DF/DI）
```

## 运行时初始化序列（2026-11-13 实测）

上电 → `$PAIR002` ACK 后，`airoha-gnssd` 还会发送（全部 runtime-only，不写 flash）：

| 命令 | 校验和 | 作用 |
|---|---|---|
| `$PAIR066,1,1,1,1,0,0` | `*3A` | 启用 GPS+GLONASS+Galileo+BDS |
| `$PAIR062,0,1` … `$PAIR062,6,1` | 见源码 | 使能 GGA/GLL/GSA/GSV/RMC/VTG/ZDA |
| `$PAIR086,0` | `*28` | 关闭二进制调试日志 |
| `$PAIR004` | `*3E` | hot start（PAIR066 后必须重启导航） |

> 只发 `$PAIR002` 的旧版本会出现"有 NMEA、有卫星 SNR、但永远没有 fix、GSV 仰角/方位为空"。
> 如遇此现象，确认 daemon 日志里有 `runtime init sent: PAIR021/066/062/086/004`。

## 关键事实（不要再走弯路）

1. **芯片自己从 flash 启动**：BROM（115200）→ `ag3335_bootloader.bin`@`0x08003000`
   → 切 **921600**、打印 `Jump to addr 0x8013000` → 运行 `gnss_demo.bin` 固件。
   旧文档的“停在 BROM”是**波特率看错**：bootloader/firmware 只在 921600 说话。
2. **上电后 ~1 秒内必须发 `$PAIR002*38`**（打开 DSP），否则固件保持静默，
   只能重新上下电（vendor 配置 `wake_duration=1000`）。
   **但只发 `$PAIR002` 不够**：还要发 `$PAIR066`（星座/NMEA 配置）+ `$PAIR062`
   （NMEA 语句使能）+ `$PAIR086,0`（关二进制调试日志）+ `$PAIR004`（hot start），
   导航才会真正开始。2026-11-13 实机验证：只发 PAIR002 时 16 颗星/SNR 20–40
   但室外 5 分钟无 fix；补发该序列后室外 **2 分钟内 mode=3、12 星、GGA quality=1**。
3. **不需要、也不要重刷 GNSS flash**。`partition_table` / bootloader / demo / config
   已在设备 flash 中且能正常启动。刷写流程只用于修复 flash（见 `extract/README.md`）。
4. 旧文档说的 `HDL_START_CMD1–4` 就是 BROM 下载握手四步
   `A0→5F / 0A→F5 / 50→AF / 05→FA`，**不是**运行时启动命令。
5. 串口上除 NMEA 外还有大量固件调试句（`$DACFOC_info/$TCFOC/$dopa…`），
   `airoha-gnssd` 会过滤，只把校验正确的 `$G*` 语句转给 gpsd。
6. 指南针（罗盘）与 GNSS 互不冲突：GNSS 用 QUP5 UART + PMIC GPIO3 + GPIO46/158；
   传感器走 SLPI/`fastrpc`，两者可同时工作（实机已验证 GPS NMEA 与
   `/net/hadess/SensorProxy/Compass` 航向同时输出）。

## 目录

```
README.md / INSTALL.md / NOTES.md
docs/PROTOCOL.md        主机协议与 anld 逆向结论（精简版）
kernel/                 airoha_gps.ko 源码（电源/时钟/pinctrl 控制）
userspace/              airoha-gnssd 守护进程源码（纯 C，无第三方依赖）
systemd/                airoha-gnssd.service + gpsd drop-in
install/                安装 / 校验 / 卸载脚本
extract/                仅在 GNSS flash 损坏需要重刷时的厂商文件提取说明
```

## 快速使用

```bash
# 0) 前提：联合 DTB 已含 GNSS 节点（本包 kernel/dtb 已包含），内核模块已编译安装
sudo modprobe airoha_gps

# 1) 编译 + 安装守护进程和 systemd 服务，并配置 gpsd
sudo ./install/install.sh

# 2) 校验
sudo ./install/verify.sh
gpspipe -w -n 5        # 或 cgps / monitor-sensor（罗盘）
```

一次性验证（不开服务）：`sudo userspace/airoha-gnssd --test 8`（编译见 INSTALL.md）。

## 标准地图 API（GeoClue2）

gpsd 只服务 gpsd 客户端；GNOME Maps、gnome-shell 等**标准地图/位置应用**走 GeoClue2。
本子包已把这条链路接好：

```
gpsd(2947, ?WATCH nmea) → gps-nmea-share → Unix socket /run/gps-share.sock → GeoClue2 --network-nmea--> 应用
iio-sensor-proxy(Compass) → GeoClue2 compass source（可选）
```

- `gps-nmea-share`（本包新增）从 gpsd TCP 订阅 NMEA，按行发布到 `/run/gps-share.sock`；
  GeoClue 2.7 的 `[network-nmea]` 源直接读取该 socket（只解析 GGA/RMC）。
- `install.sh` 会自动：安装 bridge + systemd 服务、把 `/etc/geoclue/geoclue.conf` 的
  `nmea-socket` 指到 `/run/gps-share.sock`、并加 `geoclue` 的排序 drop-in。
- 实测：GeoClue 能连接并收到 NMEA；用合成 fix 验证 `Geoclue.Simple` 返回
  lat/lon（`description=GPS GGA`）。**室内无 fix 时 GeoClue 不会给位置，属正常**，
  到室外/窗边即可。
- 指南针：iio-sensor-proxy 的标准 D-Bus API 已可用（`CompassHeading`，实测约 176°）。
  GeoClue 2.7 的 compass source 需要 `ClaimCompass`，而 iio 的
  `net.hadess.SensorProxy.claim-sensor` polkit action 默认只允许活动会话
  （`allow_active=yes`），系统服务用户 `geoclue` 会被拒。本包新增
  `polkit/49-iio-sensor-proxy-geoclue.rules` 只放行 `geoclue` 用户对该 action 的
  claim。**修复后实测（合成定位）：GeoClue `Location` 同时返回位置与
  `heading≈177°`，iio NRestarts=0；100 次 claim/release + 并发压力测试无崩溃。**
  早前观察到的单次 iio SEGV 未复现，若再出现按 `gnss/NOTES.md` 收集 core 再分析。
- 验证：
  ```bash
  systemctl status gps-nmea-share geoclue
  ls -l /run/gps-share.sock
  gdbus call --system --dest org.freedesktop.GeoClue2 --object-path /org/freedesktop/GeoClue2/Manager --method org.freedesktop.DBus.Properties.Get org.freedesktop.GeoClue2.Manager InactiveTimeout
  # 应用侧：Geoclue.Simple.new_sync(...) / GNOME Maps
  ```

## 地图应用怎么选（2026-11-12 调研）

| 应用 | 来源 | 位置 | 朝向/罗盘 | 说明 |
|---|---|---|---|---|
| **GNOME Maps** | Ubuntu（已安装） | ✅ GeoClue（本包已接） | ❌（上游 #993 仍 open） | 最省事；只显示位置/地图 |
| **Pure Maps** | Flathub（`io.github.rinigus.PureMaps`，104MB） | ✅ GeoClue | ✅ QtSensors `QCompass` + 磁偏角校正，也读 GeoClue heading | 唯一原生支持罗盘的地图应用；GeoClue heading 已可用；若要用 QtSensors 路径需 Flatpak 加 `--system-talk-name=net.hadess.SensorProxy` |
| **Navit** | Ubuntu | ✅ gpsd | ⚠️ 用 GPS 行进方向（OSD compass/地图跟随），非磁力计 | 导航/跟随方向够用 |
| **FoxtrotGPS** | Ubuntu | ✅ gpsd | ⚠️ Bearing 来自 GPS | 轻量 |
| **Marble** | Ubuntu | ✅ gpsd | ❌（CompassFloatItem 只是地图罗盘） | 3D 地球 |
| **Viking** | Ubuntu | ✅ GeoClue + gpsd | ❌ | 老式地图 |
| **QMapShack / QGIS** | Ubuntu | ✅ gpsd | ❌ | 专业户外 |

结论：只要求位置 → **GNOME Maps**；导航时按行进方向转地图 → **Navit**；
想真正用磁力计朝向 → **Pure Maps**，但需要先修 iio claim（见上）并给 Flatpak 加传感器 D-Bus 权限。

### 实机安装验证（Pure Maps，2026-11-12）

已通过 Flathub 安装 `io.github.rinigus.PureMaps 3.5.1` 并验证：

- 地图瓦片正常渲染；
- 合成定位（31.2167N, 121.4667E）下点击应用内“定位”按钮，地图跳到该位置并显示蓝色位置标记
  → **GeoClue → Pure Maps 位置链路 OK**（真实定位需室外有 fix）；
- KDE 运行时自带 QtSensors 的 iio 插件
  （`/usr/lib/plugins/sensors/libqtsensors_iio-sensor-proxy.so` + `/usr/lib/qml/QtSensors`），
  Flatpak 已加 `--system-talk-name=net.hadess.SensorProxy`；
- Pure Maps 的罗盘逻辑只在 **follow/导航模式且速度 <10 km/h** 时激活（普通浏览模式按“定位”只是居中），
  因此建议点“跟随/导航”后再看地图朝向；`auto_rotate`/`auto_center` 可在设置中开启（本机已写入配置）。
- 安装/权限命令：
  ```bash
  sudo flatpak install flathub io.github.rinigus.PureMaps
  sudo flatpak override --system-talk-name=net.hadess.SensorProxy io.github.rinigus.PureMaps
  ```
- 截图（合成定位验证）：`../gnss-notes/puremaps-verified-20261112.png`（开发工作区）。

## 已验证结果（2026-11-12）

| 项 | 结果 |
|---|---|
| 芯片启动 | `Jump to addr 0x8013000`、`$PAIR021,AG3335M_V1.3.0...` |
| 初始化 | `$PAIR002*38` 发出后约 0.35–0.45 s 收到 `$PAIR001,002,0*39` |
| NMEA | `$GNGGA/$GNGLL/$GNGSA/$GPxGSV` 持续 1 Hz（室内无星时字段为空，属正常） |
| gpsd | 识别 `/dev/ttyGNSS0`，driver `NMEA0183`，TPV 正常解码 |
| 与传感器共存 | 同时输出 NMEA 与罗盘 heading（约 176°） |

## 参考（公开资料）

- [quectel-open-source/GNSSTool](https://github.com/quectel-open-source/GNSSTool)（GPL-3.0）：
  `mtk_brom_3335.cpp` 是 AG3335 BROM/DA/刷写的完整实现（仓库含 DA 二进制）。
- Quectel《LC29H&L89 R2.0&LC79H Firmware Upgrade Guide》：AG3335 平台逐字节刷写规范。
- [dimhoff/mt333x-fw-utils](https://github.com/dimhoff/mt333x-fw-utils)（BSD-3）：MT3333 祖先，
  相同的 4 步 BROM 握手。
- [satpulse](https://github.com/jclark/satpulse)（MIT）：AG33xx PAIR 命令表。

厂商固件（`ag3335_bootloader.bin`、`gnss_demo.bin`、`gnss_config.bin`、
`partition_table.bin`、`slave_da_UART.bin`）不随包提供；本机已在 flash 中，
如确需修复参见 `extract/README.md`。
