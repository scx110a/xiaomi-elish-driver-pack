# GNSS 子包注意事项

## 红线

1. **不要写 `boot_a` / Android 分区**；只操作 rootfs / boot_b。
2. **不要为了定位重刷 GNSS flash**。当前 flash 内容正确且能启动；刷写流程
   （`../gnss/extract` 与历史 `brom_download.py`）只用于修复损坏的 flash，风险高。
3. 改 DTB 时不要用 `dtc` 往返（会破坏 regulator 4 字节属性）；用本包已验证 DTB
   或完整源码重建，见 `../kernel/dtb/README.md`。
4. 串口必须 **921600 8N1**；BROM 阶段的 115200 只用于下载，不用于固件通信。
5. `$PAIR002` 必须在上电后 **约 1 秒内** 发出；错过窗口只能断电重来（守护进程
   已自动重试 4 次，每次含上下电）。

## 已知限制

- `airoha-gnssd` 目前只做“上电 + PAIR002 + NMEA 转发 + 15 s 无数据自愈”；
  AGPS/EPO、原始测量、NMEA 语句定制尚未接入。需要时用标准
  `$PAIRnnn,…*cs` 命令扩展（XOR 校验，见 `docs/PROTOCOL.md`）。
- 室内/无天线时没有定位结果（GGA 字段为空、GSV 0 星）是正常现象。
- 未定位前 gpsd 时间可能显示 2019 年（芯片无有效 RTC 日期，靠星历/网络对时）。
- 守护进程常驻会保持 GNSS LDO 开启（约几十 mW 级）；如需要省电可停服务。
- 与传感器栈共用没有硬件资源冲突；传感器栈自身约 1/14 概率的 PD 静默卡死与本
  子包无关（重启恢复）。
- **GeoClue 标准 API**：位置已接通（`gps-nmea-share` → `/run/gps-share.sock`）；
  室内无 fix 时 GeoClue 不给位置，属正常。指南针的 iio-sensor-proxy API 可用；
  GeoClue 的 `ClaimCompass` 需要 polkit 放行（本包 `polkit/` 规则只放行 geoclue 用户），
  修复后 `Location.heading` 可用（实测 177°）。需要朝向的地图应用首选
  **Pure Maps**（Flathub，QtSensors QCompass + GeoClue heading）；Navit/FoxtrotGPS
  用 GPS 行进方向，不需要罗盘。若 iio 偶发 SEGV：确认 `/etc/polkit-1/rules.d/`
  规则存在，重启 iio 后再测；仍复现需开 core dump 分析（与 GNSS 无关）。

## 排障

| 现象 | 排查 |
|---|---|
| `cannot open /dev/airoha_gps` | `modprobe airoha_gps`；确认 DTB 有 `/bcm4775` 与 `gps-ldo` |
| `cannot configure /dev/ttyHS1` | QUP5 UART 节点未启用（DTB 未使用本包版本） |
| `init attempt N/4: no NMEA` | 上电窗口错过/供电/时钟异常；`journalctl -u airoha-gnssd -b` |
| 有 NMEA 无定位 | 天线/室内；看 `$GPGSV` 星数 |
| gpsd 不认设备 | `/etc/default/gpsd` 应为 `/dev/ttyGNSS0`，先起 airoha-gnssd |
| 重启后服务报错 | 确认 `/dev/ttyHS1` 存在、模块自动加载（`/etc/modules-load.d/airoha-gps.conf`） |
