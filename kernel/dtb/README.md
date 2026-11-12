# 联合 DTB（传感器 vmids + 指纹节点 + GNSS）

统一 DTB 必须同时包含：

1. 传感器：SLPI `glink-edge/fastrpc` 的 `qcom,vmids = <3 15 5 6>`；
2. 指纹：根节点 `/fingerprint`（`compatible="goodix,gf3626"`、`firmware-name="gfenu"`、
   `interrupts=<23 1>`、`reset-gpios=<&tlmm 125 GPIO_ACTIVE_LOW>`、`vdd-supply`=PM8150A L10A、
   pinctrl IRQ gpio23）；
3. GNSS（2026-11-12 新增）：
   - `/soc@0/geniqup@9c0000/i2c@994000`：`compatible="qcom,geni-uart"`、`status="okay"`、
     `pinctrl-0` 指向 `qup-uart5-default-state`（`/soc@0/pinctrl@f100000`，gpio12–15，function `qup5`）；
   - 根节点 `/bcm4775`：`compatible="bcm4775"`、`gps-ldo=<&tlmm 46 GPIO_ACTIVE_HIGH>`、
     `pinctrl-0`=PMIC `gps-clk-default`（`/soc@0/spmi@c440000/pmic@0/gpio@c000/gps-clk-default`）、
     `pinctrl-1/2`=`gps-enable-active`/`gps-enable-suspend`（TLMM GPIO158）。

本目录的 DTB 已包含以上全部改动；2026-11-12 又合并了前摄 OV8856/PM8008/CAMSS 节点（见 `camera/README.md`）。原 DTB 说明：（由 `sm8250-xiaomi-elish-csot.dtb` + GNSS 节点生成，
2026-11-12，sha256 `d3c2f8b5eddb5eca91ad2648ade11a542c16fd34a9dd471549b28bc43d39e153`）。

## 传感器真正依赖的是 fastrpc reserved-memory

当前 DTB 里除了 `qcom,vmids`，还有 **`/reserved-memory/fastrpc`（32MB shared-dma-pool）** 以及
slpi/cdsp/adsp 三个 fastrpc 节点的 `memory-region = <&fastrpc>`。这些是 remote heap 必需的；
`qcom,vmids` 在打了补丁的 fastrpc 里已改用候选表，保留只为与历史 DTB 一致。
`verify-combined-dtb.sh` 会同时检查 memory-region 与 reserved-memory。

## 三种做法

1. **复用已有可用 DTB**（最简单）：把之前跑通过指纹+传感器的
   `sm8250-xiaomi-elish-csot.dtb` 恢复回
   `/usr/lib/linux-image-$(uname -r)/qcom/` 和 `/boot/dtb-$(uname -r)/qcom/`。
2. **改内核 DTS 源码重编**（干净重建推荐）：在 elish board DTS/common dtsi 加
   `/fingerprint` 节点与 `qcom,vmids`，`make dtbs`。节点参考
   `fingerprint-node-reference.txt`（含 phandle 对应关系）。
3. **二进制就地修改**：仅限已有属性/等长字符串，例如 `patch-firmware-name.py`
   （`fingerpr`→`gfenu`）。

## 重要：不要用当前内核树的 DTS 重建 DTB

`/var/tmp/kbuild-src` 的 DTS **不是**当前 DTB 的完整源：用它重编会丢 17 个节点/109 个属性
（`/reserved-memory/fastrpc` 32MB、fastrpc memory-region、lid/tablet-mode、panel/ts pinctrl、
uart16、haptics、regulator@88e7000 等），且树内只有 vmids、没有指纹节点。
最小安全方案：直接使用本目录附带的已验证 DTB（或你之前备份的同一份），不要从树内 DTS 重建。

## 禁止 dtc 往返

`dtc -I dtb -O dts` 再 `dtc -I dts -O dtb` 会把 **`regulators-1/ldo10`（L10C，3.3V）** 的
4 字节 `regulator-min/max-microvolt`（`00 32 4b 00`）改写成 3 字节（`02 4b 00`）——语义破坏。
**L10A 是 `regulators-0/ldo10`（3.008V），不受该问题影响**；当前验证 DTB 只新增了 18 个属性，
没有改动任何已有属性。改属性请用等长二进制替换或完整源码重编。

## 校验

```bash
./verify-combined-dtb.sh /usr/lib/linux-image-$(uname -r)/qcom/sm8250-xiaomi-elish-csot.dtb
# 期望：vmids=3 f 5 6；/fingerprint compatible=goodix,gf3626、firmware-name=gfenu；
#       QUP5 UART compatible=qcom,geni-uart / status=okay；/bcm4775 有 gps-ldo 与 pinctrl
```
