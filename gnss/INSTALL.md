# GNSS 子包安装文档

前置：本包 `kernel/dtb/sm8250-xiaomi-elish-csot.dtb`（含 GNSS 节点）已按
`../kernel/update-boot-image.sh` 写进 boot_b 并重启；内核模块已编译安装。
**不要写 boot_a / Android 分区。**

> **一键安装器**：`../install.sh` 会自动调用 `gnss/install/install.sh` 完成用户态 + gpsd 集成；
> `airoha_gps.ko` 由 `--with-kernel`（或手动 `../kernel/build-and-install.sh`）一并编译安装。

## 0. 部署 DTB 与内核模块

本包 DTB 已包含传感器/指纹/GNSS 全部节点。首次安装或内核/DTB 被升级后：

```bash
# 1) 安装包内已验证 DTB 到系统路径（update-boot-image.sh 从这里取）
sudo install -D -m 644 ../kernel/dtb/sm8250-xiaomi-elish-csot.dtb \
  "/usr/lib/linux-image-$(uname -r)/qcom/sm8250-xiaomi-elish-csot.dtb"
sudo install -D -m 644 ../kernel/dtb/sm8250-xiaomi-elish-csot.dtb \
  "/boot/dtb-$(uname -r)/qcom/sm8250-xiaomi-elish-csot.dtb"
# 2) 校验 + 备份并重建 boot_b（会写 boot_b，绝不写 boot_a；详见 kernel/update-boot-image.sh）
../kernel/dtb/verify-combined-dtb.sh "/usr/lib/linux-image-$(uname -r)/qcom/sm8250-xiaomi-elish-csot.dtb"
sudo ../kernel/update-boot-image.sh
sudo reboot
```

重启后确认：

```bash
# DTB 必须含 QUP5 UART + bcm4775 节点
ls /proc/device-tree/bcm4775
cat /proc/device-tree/soc@0/geniqup@9c0000/i2c@994000/compatible | tr '\0' '\n'   # qcom,geni-uart

# 运行中的 DTB 校验（含 GNSS 检查）
sudo ../kernel/dtb/verify-combined-dtb.sh
```

编译并安装 `airoha_gps.ko`（与运行内核同版本源码树，例如 `/var/tmp/kbuild-src`）：

```bash
make -C /var/tmp/kbuild-src M="$PWD/kernel" modules
sudo install -D -m 644 kernel/airoha_gps.ko "/lib/modules/$(uname -r)/extra/airoha_gps.ko"
sudo depmod -a
echo airoha_gps | sudo tee /etc/modules-load.d/airoha-gps.conf
sudo modprobe airoha_gps
ls -l /dev/airoha_gps
```

若内核树里没有 `airoha_gps` 相关符号，按 `kernel/README.md` 生成；模块只依赖
`gpio`/`pinctrl`/`of` 公共 API，主线 6.18 无需补丁。

## 1. 安装守护进程与 gpsd 集成

```bash
sudo ./install/install.sh
```

脚本会：

1. `make -C userspace` 编译 `airoha-gnssd`；
2. 安装到 `/usr/local/sbin/airoha-gnssd` 与 `/etc/systemd/system/airoha-gnssd.service`；
3. 备份 `/etc/default/gpsd` 并改为 `DEVICES="/dev/ttyGNSS0"`（`USBAUTO=false`、`-n`）；
4. 安装 gpsd 的 `After/Requires/PartOf=airoha-gnssd.service` drop-in；
5. `systemctl daemon-reload && enable --now airoha-gnssd`，并重启 gpsd。

依赖：`gpsd`（`sudo apt install gpsd gpsd-clients`），编译只需 `gcc`/`make`。

## 2. 验证

安装脚本同时会配置 GeoClue2 标准位置 API（`gps-nmea-share` → `/run/gps-share.sock`）；
GNOME Maps 等标准地图应用通过 GeoClue 取位置。`verify.sh` 会检查 bridge、socket、
geoclue 配置与服务状态。

```bash
sudo ./install/verify.sh
# 或手动：
systemctl status airoha-gnssd gpsd gps-nmea-share geoclue
ls -l /dev/ttyGNSS0 /run/gps-share.sock
cat /run/airoha-gnssd.state
gpspipe -w -n 5          # DEVICES 出现 /dev/ttyGNSS0；TPV 正常
monitor-sensor           # 罗盘 heading（与 GNSS 互不影响）
```

室内无星时 `gpspipe` 的 `mode` 为 1、GSV 星数为 0，属正常；到窗边/室外应出现
`mode:2/3` 和经纬度。GNSS 时间在未定位前可能是 2019 默认日期（无 RTC 电池），
属正常。

## 3. 开机自启

`airoha-gnssd.service` 已 enable；`gpsd` 通过 drop-in 排在其后。开机约 1 秒内
启动并进入 1 Hz 输出，与传感器栈（M2 后约 22 秒才可用）互不依赖。

## 4. 回退

```bash
sudo ./install/uninstall.sh
# 或手动：
sudo systemctl disable --now airoha-gnssd
sudo rm -f /usr/local/sbin/airoha-gnssd /etc/systemd/system/airoha-gnssd.service
sudo rm -rf /etc/systemd/system/gpsd.service.d/airoha-gnssd.conf
sudo cp /etc/default/gpsd.bak-airoha /etc/default/gpsd   # 如需恢复旧 gpsd 配置
sudo systemctl daemon-reload
```

停止服务时守护进程会通过 `/dev/airoha_gps` 关闭 GNSS LDO；不会改动 flash 与 Android。
