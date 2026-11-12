# libssc（SSC/QMI 传感器库）

- 版本：**v0.4.4**
- 源码：https://codeberg.org/mmcallister/libssc
- 本包不含源码补丁（使用上游原版）。

## 编译

依赖（Ubuntu 24.04）：`build-essential meson ninja-build pkg-config libglib2.0-dev
libqmi-glib-dev libqrtr-glib-dev libprotobuf-c-dev protobuf-c-compiler`。

```bash
git clone https://codeberg.org/mmcallister/libssc
cd libssc
git checkout v0.4.4        # 或对应 0.4.4 tag/commit
meson setup build --prefix=/usr/local -Dintrospection=false
ninja -C build
sudo ninja -C build install
sudo ldconfig
```

安装结果（本机验证）：

```
/usr/local/lib/aarch64-linux-gnu/libssc.so -> libssc.so.2
/usr/local/lib/aarch64-linux-gnu/libssc.so.2
/usr/local/include/libssc/...
```

验证：

```bash
pkg-config --modversion libssc     # 期望 0.4.4
```

iio-sensor-proxy 的 meson 通过 `dependency('libssc', version: '>=0.2.1')` 找到它；
若找不到，确认 `/usr/local/lib/aarch64-linux-gnu/pkgconfig/libssc.pc` 存在、
`PKG_CONFIG_PATH` 包含该目录（通常 `ninja install` + `ldconfig` 后即可）。

## 说明

- 该库通过 QRTR/QMI 连接 SSC（sensor core，service 0x190）并枚举传感器；
  registry 初始化需要 hexagonrpcd 提供的文件服务先就绪。
- 本机不再使用 distro 包的 libssc；iio-sensor-proxy 必须链接这份 0.4.4。
