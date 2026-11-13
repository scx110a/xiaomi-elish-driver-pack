# AG3335 主机协议（从 Android `anld` 逆向 + 实机验证）

> 结论级资料，供后续维护/扩展参考。来源：`/vendor/bin/hw/airoha.hardware.anld`
> 反汇编（`.gnu_debugdata` 可还原 2579 个符号）与本机实测。详细报告见开发工作区
> `gnss-notes/`（RE-handshake / RE-brom / RE-image / research-web-*）。

## 1. 硬件与电源

| 项 | 值 |
|---|---|
| 芯片 | Airoha AG3335（固件版本 `AG3335M_V1.3.0.tms_1_20210628`） |
| UART | QUP5 `0x994000` → `/dev/ttyHS1`，**921600 8N1**（主机侧不需要流控） |
| GPS LDO | TLMM GPIO46，驱动里 `gps-ldo`，高有效 |
| GPS_EN | TLMM GPIO158，pinctrl `gps-enable-active/suspend` |
| GPS clock | PMIC PM8150 GPIO3 `func1`（`gps-clk-default`） |
| 电源控制 | `/dev/airoha_gps` 写 `OPEN`/`CLOSE`（LDO）、`DF`/`DI`（active/idle pinctrl） |

`airoha_gps.ko` 只提供上述 4 条命令，不参与数据传输；数据由 `airoha-gnssd`
在用户态完成（与 Android anld 同构，Linux 没有 GNSS 输入子系统）。

## 2. 启动链与波特率（关键！）

```
上电
 → BROM：115200 打印 F1/V0/00/U0/T0 + "Leaving the BROM"
 → 从 SPI flash 0x08003000 加载 ag3335_bootloader.bin，切到 921600
 → 打印 set CP10/CP11、hal_clock_set_pll_dcm_init、hal_flash_init
        hf_fsys_ck=…、hf_fsfc_ck=…
        "Jump to addr 0x8013000"
 → 运行 gnss_demo.bin（0x08013000）固件：
        $PAIR001,021,0*38
        $PAIR021,AG3335M_V1.3.0.tms_1_20210628,S,N,…*0C
        $TMS_info_init, …
 → 等待主机在 ~1 s 内发 $PAIR002（否则静默，需重新上下电）
```

> 旧文档的“停在 BROM / 需要运行时加载固件”是因为在 **115200** 监听：921600 的
> bootloader/firmware 输出被 115200 采样成乱码。芯片一直是从 flash 正常启动的。

## 3. PAIR ASCII 协议

- 帧：`$PAIR<3位命令>[,参数…]*<XOR 校验>\r\n`；
- 校验 = `$` 与 `*` 之间所有字节的 XOR，两位大写十六进制；
- 应答：`$PAIR001,<命令号>,<结果>*xx`（`0`=成功，非 0=失败）。

| 命令 | 含义 | 实测 |
|---|---|---|
| `$PAIR002*38` | 打开 DSP（**仅此不够**） | `$PAIR001,002,0*39`；随后 NMEA 1 Hz |
| `$PAIR066,1,1,1,1,0,0*3A` | 启用 GPS+GLONASS+Galileo+BDS | ACK; 必须后跟 `$PAIR004` 才切换星座 |
| `$PAIR062,<n>,1*cs` | 使能标准 NMEA 语句（0=GGA…6=ZDA） | ACK |
| `$PAIR086,0*28` | 关闭二进制调试日志 | ACK |
| `$PAIR004*3E` | hot start（重启导航） | ACK；随后重新搜星/等星历 |
| `$PAIR021*39` | 查询版本 | `$PAIR021,AG3335M_V1.3.0….tms_1_20210628,…` |
| `$PAIR050,1000*12` | 定位间隔 1000 ms | ACK 0（默认已是 1 Hz） |
| `$PAIR062,…` | NMEA 语句/频率配置 | 本机实测 ACK 0；具体参数组合需查 Airoha PAIR 文档 |

固件同时输出大量调试句（`$DACFOC_info`、`$TCFOC`、`$CFOA_info`、
`$flash_tms_info`、`$dopa/b/c/d`、`$TMS_info_init` 等），`airoha-gnssd` 会丢弃。

**初始化窗口**：实测 0.6 s / 0.8 s 发送可成功，≥1.0 s 无响应；
对应 vendor XML 的 `wake_duration=1000`。守护进程在上电后 0–1.3 s 内每 60 ms
重发 `$PAIR002`，失败自动上下电重试（最多 4 次）。

## 4. Android `anld` 下载流程（仅修复 flash 时需要，日常不用）

`AirohaDownloader::start()`（0x2d928）：

```
openTTY("/dev/ttyHS1", O_RDWR|O_NONBLOCK, B115200)
→ handShake()          # BROM：A0→5F, 0A→F5, 50→AF, 05→FA
                       # 对应日志 HDL_START_CMD1..4（不是运行时启动命令！）
→ disableWDT()         # D2, A2 08 00 00, 00 00 00 01, …
→ sendDA()             # D7, addr=0x04204000, size, 1024B/块, 16-bit XOR 校验
→ jumpToDA()           # 00 D5, addr, 状态
→ syncWithDA()         # 3F/F3/C0/0C00、power latch、DA Raise MCU、切 921600、
                       # 18 字节 flash info（mfg/dev/mount/base/size）
→ formatFlash()        # D4 00 + base/size（整片 4 MiB）
→ downloadChip(configList, 4)  # B2 写镜像
→ close(tty)；下载线程随后 CLOSE 断电
```

镜像表（`configList` @0x8ffb8，24B/项：path, backup, boot 标志, flash 地址）：

| 顺序 | 文件 | flash 地址 | boot 标志 |
|---|---|---|---|
| 1 | partition_table.bin | 0x08000000 | 0 |
| 2 | gnss_demo.bin | 0x08013000 | 0 |
| 3 | ag3335_bootloader.bin | 0x08003000 | **1 → 末尾 0x5A** |
| 4 | gnss_config.bin | 0x083FF000 | 0 |

B2 包协议：`B2 + addr + 4K 对齐大小 + 0x1000` → 5A/5A；每 4 KiB 数据 + 32-bit
byte-sum（大端）→ 0x69；全部发完 → 0x5A → 总校验和 → 0x5A → 末字节
（bootloader=0x5A，其余=0xA5）→ 0x5A。

跳过逻辑：只比较 `/vendor/etc/gnss/*.bin` 与 `/data/vendor/airoha/*_backup.bin`，
不读 flash；因此 Android 无法发现 flash 损坏。

## 5. 公开参考实现

- Quectel GNSSTool `mtk_brom_3335.cpp`（GPL-3.0）：完整 BROM/DA/刷写；仓库带 DA 二进制。
- Quectel LC29H/L89 升级指南：AG3335 平台刷写规范（逐字节、ACK、校验）。
- dimhoff/mt333x-fw-utils（BSD-3）：MT3333 同款 4 步握手。
- satpulse `configs/gpsmsg/airoha/ag33xx.toml`：AG33xx PAIR 命令表。
