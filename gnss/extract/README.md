# GNSS flash 修复（仅在不修不可时使用）

**日常定位不需要这里的内容。** 本机 GNSS flash 已含正确 vendor 镜像，芯片能从
flash 启动；只有在 flash 内容损坏/被擦除、芯片确实回到 BROM 下载模式时才需要重刷。
重刷有风险（整片 4 MiB 擦除），且必须按顺序/时序操作。

## 需要的文件

来自你自己的 Android vendor 镜像（路径示例：`<android-vendor.img 路径>`）：

```
/vendor/etc/gnss/partition_table.bin
/vendor/etc/gnss/ag3335_bootloader.bin
/vendor/etc/gnss/gnss_demo.bin
/vendor/etc/gnss/gnss_config.bin
/vendor/etc/gnss/slave_da_UART.bin
```

提取示例（只读挂载/`debugfs`，不要写 Android 分区）：

```bash
sudo mkdir -p /tmp/gnss-vendor
for f in partition_table.bin ag3335_bootloader.bin gnss_demo.bin gnss_config.bin slave_da_UART.bin; do
  sudo debugfs -R "dump /etc/gnss/$f /tmp/gnss-vendor/$f" <android-vendor.img 路径>
done
sudo chown -R "$USER:" /tmp/gnss-vendor
```

安装顺序（Android anld 的 `configList`）：partition_table → gnss_demo →
ag3335_bootloader（B2 末字节 0x5A）→ gnss_config；写前先 `D4 00` 整片格式化。

## 参考实现

协议与刷写步骤不要自己重写，直接用公开实现并逐字节对照：

- Quectel `GNSSTool` / `QGNSS_BROM/MTK/mtk_brom_3335.cpp`（GPL-3.0）；
- Quectel LC29H/L89 升级指南（AG3335 平台）；
- 本机开发历史（仅记录，不随包）：`brom_download.py`（`android_sync` /
  `full_android` stage）与工作区 `gnss-notes/RE-brom.md`、`RE-image.md`。

刷写流程细节见 `../docs/PROTOCOL.md` §4。
