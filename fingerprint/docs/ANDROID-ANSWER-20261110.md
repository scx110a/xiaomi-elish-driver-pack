# Android 侧答复（2026-11-10 07:1x，PC 侧 DSH 代查）

> 历史调查记录；相关 Android 侧目录/镜像不随包，结论以 `../NOTES.md`/`README.md` 为准。

针对 `ANDROID-HELP-NEEDED.md` 的 5 项全部查到了。**其中第 1 项发现了一个很可能就是根因的问题。**

---

## ⚠️ 最重要的发现：Android 加载的 TA 是 `gfenu`，不是 `fingerpr`

- 固件里有两套 Goodix TA：`fingerpr.*` 和 `gfenu.*`（`/vendor/firmware_mnt/image/`）
- Android 的 HAL 库 `libgf_ca.so` 里**唯一的 TA 名字符串是 `gfenu`**（`strings /vendor/lib64/libgf_ca.so`）
- 开机 logcat 明确：`[gf_ca_open_session] Loading gfenu Succeed.`
- 所以 Linux 侧一直加载的 `fingerpr` 是**另一个 TA**（`fingerpr` 比 `gfenu` 大：b02 6.1MB vs 1.3MB、b04 4.19MB vs 3.57MB）
- `fingerpr.*` 可能与 ROM 里的 FPC HAL（`fingerprint.fpc.so`）或旧方案有关，当前 Goodix HAL 不用它

→ **建议先把 TA 换成 `gfenu`**（DT firmware-name / 客户端 app 名），再复测。

---

## 初始化序列（logcat 逐条提取，带 token 编号）

```
gf_hal_open
gf_ca_open_session → "Loading gfenu Succeed."
send_hmac_key
  token1  cmd_id=109   ← Linux 侧完全没发过这条
  token2  cmd_id=1000  DETECT_SENSOR
  token3  cmd_id=1001  INIT
  token4  cmd_id=1005  INIT_FINISHED
  token5  cmd_id=1016  IRQ
  token6  cmd_id=1016  IRQ
  token7  cmd_id=1017  SCREEN_ON
```

## 109 的 key 来自 Keymaster

`libgf_ca.so` 里的字符串：

```
get_key_from_keymaster
keymaster64
send_hmac_key
[%s] keymaster start success,km handle=%p, errno=%d
```

即：HAL 在 open 之后先从 Keymaster 取 HMAC key，通过 `send_hmac_key`（cmd 109）发给 TA，之后才发 1000/1001/1005。

**推断：Linux 侧如果没有先发 109、或 HMAC key 不对，TA 会拒绝后续所有命令 → 正好是 `0xFFFFFFFF` 的现象。**

---

## 第 1 项：TA secure storage 状态（已拷贝）

路径 `/data/vendor/fpdump/`（`libgf_hal.so` 里硬编码了 `/data/vendor/fpdump`），5 个对象：

| 文件 | 大小 | md5 |
|---|---|---|
| gf_calibration.so | 23,220 | e79fa77bcb8a17561275f860187ddd40 |
| gf_nav_base.so | 3,256 | 1ad580c1824f0345ab0707b007c36bb0 |
| gf_otp_info.so | 116 | cf5e4eec234de68c8831aa762e6412f2 |
| gf_fdt_base.so | 80 | 48fa4d1fa44357adba6f3bfe84b14c79 |
| auth_token_0.so | 70 | f2d54f771734cb652a8f19fbcb020aa0 |

这些文件是 TA 的私有不透明格式（前 16 字节像 hash/头部），不是明文；直接 seed 给 Linux 的 qseecom file service 目录应该可行（由 TA 自己解密）。已拷到 `android-storage-fpdump/`。

另外注意：`/data/vendor/goodix/`、`/data/vendor/qtee/` 都是空目录；`/data/vendor/tzstorage/` 是其它 TEE 服务的加密存储，与指纹无关。

## 第 2/3 项：qseecomd 日志与 TA 初始化

- dmesg 里 qseecom 只记录了 `cardapp`/`antdtx`/`mlipay` 等 TA 的 load/unload（gfenu 的加载没有出现在 dmesg ring 里）
- logcat 里 `gf_hal`/`gf_ca` 的完整初始化日志已导出：`android-info-20261110/04-logcat-boot.txt`
- 关键日志：open → send_hmac_key(109) → 1000 → 1001 → 1005；其中 1001 完成后：
  - `[gf_hal_milan] update new otp data to HAL storage`
  - `CHIP_TYPE = GF_CHIP_3626ZS1`，OTP: year=21 month=6 day=2 product_id=0xa0
  - `setprop persist.vendor.sys.fp.info] = 0x0000015602a0ffff`
- HAL 尝试读写 `/data/media/0/gf_otp_info`（不存在，报 `GF_ERROR_FILE_NOT_EXIST, errno=1079`）——这是 OTP 的 sdcard 备份文件，不影响 TA

## 第 4 项：TA 镜像校验

- Android `/vendor/firmware_mnt/image/fingerpr.*` 的 md5 == Linux `ta-image/fingerpr.*`（9 个文件全部一致）
- 但当前 HAL 实际加载的是 `gfenu`（md5 见 `ta-image-gfenu/`），两者不是同一个 TA

## 第 5 项：app_id / 会话

- Android 侧相关进程：`vendor.qti.hardware.qseecom@1.0-service`、`qseecomd`
- 内核有 `qseecom-unreg-l`、`qseecom-unload-` 内核线程

---

## 建议的下一步（Linux 侧）

1. **换 TA**：DT `firmware-name` 或客户端 app 名改成 `gfenu`，固件用 `ta-image-gfenu/`
2. **实现 cmd 109**：从 Keymaster 取 HMAC key，open 后先发 `send_hmac_key`（最可疑的根因）
3. **导入存储对象**：把 `android-storage-fpdump/` 种到 qseecom file service 对应目录（可先做"空 vs 种入"对比）
4. 然后重试 `1006 PRE_ENROLL`

## 附：文件位置（历史调查时的工作区目录，不随包）

- `ta-image-gfenu/` — gfenu TA 完整 9 个文件（md5 见内）
- `android-storage-fpdump/` — /data/vendor/fpdump 的 5 个存储对象
- `android-info-20261110/` — 采集的原始文本（01~09）+ 本答复
- 参考：`capture-android-20261110/`（之前抓的运行时命令全量 payload）
