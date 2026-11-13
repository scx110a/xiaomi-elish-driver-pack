# 版本与校验值（合并包 BOM）

> 本包不含编译产物/固件；下列 sha256/md5 用于安装后交叉核对。验证基线：2026-11-11。

## 内核

| 项 | 值 |
|---|---|
| 运行内核 | `6.18.44-current-sm8250` |
| 基线源码 | Linux v6.18.44（tar.xz sha256 `0f72d938f06828e82c90405174fe572287db7bfe089e2fc46572a99a7f240d43`）+ Armbian sm8250 补丁（获取方式见 `kernel/ARMBIAN-BASELINE.md`） |
| 配置 | `kernel/config-used-6.18.44`（TEE/QSEECOM/MDT/BTF/FASTRPC；LOCALVERSION=-current-sm8250） |
| 安装后 fastrpc.ko sha256 | `fe2cd7ba1899f516b7858e914af21132122505457da425557d8647c64b640134` |
| 安装后 goodix_fp_spi.ko sha256 | `e433ae598fd7a9ce0ca637d284497671d8f7c78663ad9a5da739e87049821c36` |
| 传感器补丁 | `kernel/patches/0001-fastrpc-sdsp-vmid-and-sensors-pd.patch`（2 hunk） |
| LPASS pinctrl 补丁 | `kernel/patches/0002-pinctrl-lpass-lpi-defer-on-clock-timeout.patch`（1 hunk，独立问题） |
| 内核集成 | `kernel/include/`（3 头文件）+ `kernel/INTEGRATION.md`；权威系列补丁 `kernel/patches/fingerprint/qseecom-tee-series.patch`（14 文件） |
| boot hook 参考 | `kernel/boot/zz-update-abl-kernel`（系统 hook 副本）；统一用 `kernel/update-boot-image.sh` |
| QCOM_MDT_LOADER Kconfig | `kernel/patches/0003-qcom-mdt-loader-user-selectable.patch` |
| boot 镜像更新 | `kernel/update-boot-image.sh`（预检+备份+回读校验） |
| 联合 DTB（随包） | `kernel/dtb/sm8250-xiaomi-elish-csot.dtb`，sha256 `14909d42d6a1935f6dd49ff8b4700e0adf4164024c6f0c25f6ba94b6ad3ee4a8`（模型级，无本机私有信息；**2026-11-12 加入 GNSS：QUP5 UART + bcm4775 + PMIC GPS clock pinctrl**） |
| initramfs hook | `kernel/initramfs/hooks/xiaomi-elish-firmware`（修复版，跳过 slpi 符号链接/禁用文件） |
| DTB 关键项 | `qcom,vmids=<3 15 5 6>`；`/fingerprint` `firmware-name="gfenu"`、`compatible="goodix,gf3626"` |

## bioauth（可选组件，默认不装）

| 项 | 值 |
|---|---|
| 组件 | 通用生物认证 PAM 栈（PAM module + root daemon + GTK4 单窗口 helper） |
| 源码 | `bioauth/`（同源快照；含 tests / doctor / install脚本 / 审计与 UAC 文档） |
| 安装 | 默认不装；`--with-bioauth`，PAM 接入用 `--bioauth-sudo` / `--bioauth-pkexec` |
| 依赖 | fprintd/libfprint（本包指纹栈）；构建需 libpam0g-dev、python3-gi、gir1.2-gtk-4.0 |
| 验证 | `bioauth-doctor` 31 项；`make check` / `check-tty` / `check-gui` |
| 回滚 | `sudo bioauth/scripts/install-bioauth.sh --rollback`（PAM 备份在 `/root/bioauth-sudo-backup-*`） |

## 传感器用户态

| 组件 | 版本/来源 | 安装后 sha256（参考） |
|---|---|---|
| hexagonrpc | tag v0.3.2（commit `b7eff23`）+ warm-attach 补丁 | `/opt/hexrpc-032b/bin/hexagonrpcd` = `ec9c370e4ddb95196e4bf56a5001a93aad67381d56e1eccc935b90fe54ea6312` |
| libssc | 0.4.4（`v0.4.4-9-g4e4c6c8`） | `/usr/local/lib/aarch64-linux-gnu/libssc.so.2`（见原包记录） |
| iio-sensor-proxy | commit `ca0d5b0`（3.9-1）+ 三处补丁 | `/usr/libexec/iio-sensor-proxy` = `c6e707c4e01d2461ce182e5bc5b69c17a1449b77def5939eb2a21e5cb306d49d` |

传感器数据（不随包）：registry 155 / config 40 / proto 79 / socinfo 6 / SLPI `slpi.mbn` md5 `51c3b3bef20a76d386a516dfbcc72b68`（7,633,828B）。

## GNSS 用户态

| 组件 | 版本/来源 | 说明 |
|---|---|---|
| airoha-gnssd | 本包 `gnss/userspace/`（C，无第三方依赖） | 上电 + `$PAIR002` + NMEA→pty + 自愈；安装后 `/usr/local/sbin/airoha-gnssd` |
| gps-nmea-share | 本包 `gnss/userspace/`（C，无第三方依赖） | gpsd NMEA → GeoClue2 network-nmea Unix socket；安装后 `/usr/local/sbin/gps-nmea-share` |
| airoha_gps.ko | 本包 `gnss/kernel/`（从 Android 下游移植到主线 6.18） | `/dev/airoha_gps` 的 OPEN/CLOSE/DF/DI；与内核 ABI 绑定，需按 `gnss/kernel/README.md` 重编 |
| gpsd | Ubuntu noble 3.25 | 读取 `/dev/ttyGNSS0`（NMEA0183） |
| AG3335 固件 | 设备 GNSS flash 内已有（`ag3335_bootloader.bin`/`gnss_demo.bin`/`gnss_config.bin`/`partition_table.bin`） | **不随包**；正常使用不需要重刷 |

## 指纹用户态

| 组件 | 版本/来源 | 安装后 sha256（参考） |
|---|---|---|
| qsee-supplicant / app-loader | 0.1.1（wrobelda/qsee-supplicant，commit `36e06680`） | `/usr/sbin/qsee-supplicant` = `c81bb42eacee195a80c983d36b9464aadcc00b135a2ec3f6b05d401589fd5371`；`/usr/sbin/qsee-app-loader` = `2d0703086862f04ceb008d0eaf02efffb3cf307fb1b17665a953da62522c92a3` |
| libfprint | wrobelda/libfprint `goodix-qsee`（1.94.100，文档基准 commit `430e24e5`；归档为 tarball，本地以版本+补丁 sha256 锁定）+ 日志补丁 | `/usr/lib/aarch64-linux-gnu/libfprint-2.so.2.0.0` = `614e4194cf7e5723d2b1d9d4e8ac03c121e3647f89d32530b23358ba77251525` |
| fprintd / libpam-fprintd | Ubuntu noble 1.94.3 | — |
| TA gfenu | 从**你自己设备** Android 固件提取（9 文件，约 4.8MB） | 见 `fingerprint/extract/README.md`（脚本自动校验） |
| 驱动源码 | `kernel/qseecom/`、`kernel/mdt_loader.c`、`kernel/qcom_scm.c`、`kernel/goodix_fp_spi/` | 编译验证 |

## 数据/固件校验（安装后）

```bash
# 传感器
md5sum /usr/share/qcom-ssc-elish/slpi.mbn                         # 51c3b3bef20a76d386a516dfbcc72b68
find /usr/share/qcom-ssc-elish/sensors/registry -type f | wc -l   # 155
find /usr/share/qcom-ssc-elish/sensors/config -type f | wc -l     # 40
find /usr/share/qcom-ssc-elish/sensors/proto -type f | wc -l      # 79
find /usr/share/qcom-ssc-elish/socinfo -type f | wc -l            # 6
# 指纹
ls /lib/firmware/gfenu.* | wc -l                                  # 9
sha256sum -c <(cd /lib/firmware && sha256sum gfenu.*)             # 对照 extract/README.md
# 联合 DTB
./kernel/dtb/verify-combined-dtb.sh
```

## 已安装二进制对照（可选）

```bash
sha256sum /lib/modules/$(uname -r)/kernel/drivers/misc/fastrpc.ko \
          /lib/modules/$(uname -r)/extra/goodix_fp_spi.ko \
          /usr/libexec/iio-sensor-proxy \
          /usr/lib/aarch64-linux-gnu/libfprint-2.so.2.0.0 \
          /usr/sbin/qsee-supplicant /usr/sbin/qsee-app-loader
```
