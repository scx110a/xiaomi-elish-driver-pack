# airoha_gps.ko — AG3335 电源/时钟/pinctrl 控制

从 Android 下游 `drivers/airoha/airoha_gps_driver.c` 移植到主线 6.18 的精简版，
只负责电源与引脚状态，不做数据传输：

| `/dev/airoha_gps` 写入 | 动作 |
|---|---|
| `OPEN` | GPS LDO（TLMM GPIO46）拉高 |
| `CLOSE` | GPS LDO 拉低 |
| `DF` | 选择 `gps-enable-active` pinctrl（GPS_EN=GPIO158 有效 + PMIC GPS clock） |
| `DI` | 选择 `gps-enable-suspend` |

关键点：`gps-ldo` 不是标准 `-gpios` 属性，驱动用 `of_get_named_gpio`；DTB 节点
`/bcm4775` 需提供 `gps-ldo` 与 `pinctrl-0/1/2`（本包 DTB 已包含）。

## 编译安装

```bash
# 需要与运行内核完全同版本的源码树（本机历史路径 /var/tmp/kbuild-src）
make -C /var/tmp/kbuild-src M="$PWD" modules
sudo install -D -m 644 airoha_gps.ko "/lib/modules/$(uname -r)/extra/airoha_gps.ko"
sudo depmod -a
echo airoha_gps | sudo tee /etc/modules-load.d/airoha-gps.conf
sudo modprobe airoha_gps
ls -l /dev/airoha_gps
```

不需要任何内核源码补丁；依赖 `CONFIG_GPIOLIB`/`CONFIG_PINCTRL`/`CONFIG_OF`。

若模块目录里仍是旧 `.ko` 会出现 `no gps-ldo GPIO` probe 失败，必须安装修复版。

## 注意

- 已安装的模块与内核 ABI 绑定；升级内核后需按同样步骤重编（详见
  `../docs/UPGRADE-IMPACT-ALL.md`）。
- 该模块与传感器 fastrpc / 指纹驱动无冲突，可同时加载。
