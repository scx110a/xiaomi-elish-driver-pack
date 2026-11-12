# 指纹数据/固件提取（本包不附带 TA 镜像）

指纹安装需要一份**设备厂商固件**：Goodix `gfenu` TA（9 个文件，约 4.8 MB，ELF32，Goodix 签名）。
本包不附带它（厂商二进制，且不适合再分发），请从**你自己的设备/镜像**提取。

## 来源（任选）

1. **从 Android 设备 adb（需 root）**
   ```bash
   ./extract-gfenu-ta.sh --adb ./extracted/fingerprint-ta
   ```
   脚本会尝试 `/vendor/firmware_mnt/image`、`/firmware/image`、`/vendor/etc/firmware`。

2. **从 Android firmware 分区镜像**
   ```bash
   ./extract-gfenu-ta.sh /path/to/firmware_mnt.img ./extracted/fingerprint-ta
   ```
   （脚本用 debugfs 只读导出，尝试镜像内 `/image` 与根目录。）

3. **已有目录**：把 `gfenu.mdt` + `gfenu.b00..b07` 放到同一目录后：
   ```bash
   ./extract-gfenu-ta.sh /path/to/dir ./extracted/fingerprint-ta
   ```

## 校验（脚本自动核对）

| 文件 | sha256 |
|---|---|
| gfenu.mdt | `8339a3475b3897a8fcce2ab9626a9239b52f2b234782b98b8945588db4640dab` |
| gfenu.b00 | `d983c8496f741a798d9a9895ea5860f712199f23764dd536944cdf8539250b61` |
| gfenu.b01 | `ed70572089e239656009828558132ad55dc30567cd7592a726a261ecec3154f4` |
| gfenu.b02 | `9033afda9aa483aac6926f9fd6d94e9b35cc32b9ca411973f1df88d4e0c9f43f` |
| gfenu.b03 | `09c1a80b0478d226f00d8c171a5d954429150e568eaec3ccc5d43d96e926a9ae` |
| gfenu.b04 | `4668a9259b02f284bb010787fa6b701a2f4fd4e3bd4147395f03977830686a35` |
| gfenu.b05 | `06babd5d76f8f678a550e493f616832ec09e1362b135b8278e4113aaae41b5f7` |
| gfenu.b06 | `e4a21e79da3a5e3000008691c1ad77910d0f699ca08235c886833864342eed26` |
| gfenu.b07 | `0a78d00cc7aab1b43b56f0c16e07ca8a209219ea623195d59235779ba168d8a0` |

安装：`sudo cp <提取目录>/gfenu.* /lib/firmware/ && sudo chmod 0644 /lib/firmware/gfenu.*`。
TA 名字必须与 DTB `/fingerprint` 的 `firmware-name` 一致（本项目为 `gfenu`）。
**不要用 `fingerpr.*`**：那是 Android 里给 FPC/旧方案的错误 TA。

## 其他数据

- 指纹模板：录入时由 fprintd/TA 写入 `/var/lib/fprint/...`，属于用户私有数据，**不随包分发**。
- 指纹相关内核源码已随包提供；Armbian 基线获取见 `../kernel/ARMBIAN-BASELINE.md`。
- 传感器侧数据/固件提取见 `sensors/extract/README.md`。
