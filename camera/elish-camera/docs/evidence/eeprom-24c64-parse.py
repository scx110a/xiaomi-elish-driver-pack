#!/usr/bin/env python3
"""OV13B10 后摄 24c64 EEPROM (8KB) 校准数据解析器 —— 逆向结论 (2026-11-12)。
用法: python3 eeprom-24c64-parse.py <自己设备读取的 8KB EEPROM dump>
已验证部分:
  * 0x0000-0x006B  header: 模块信息/AF/AWB 等标定字段区(未完全解码, 0xFFFF=未写入哨兵)
  * 0x006C-0x0753  LSC 表: 4 通道 x (17x13=221) mesh, 通道交错(stride 4), 大端 u16,
                   中心=1023, 四角 232-370, 平滑径向 rolloff, 4 通道相关系数 0.999
  * 0x0754-0x0EC8  0xFF 填充
  * 0x0EC9-0x12BD  第二标定块(~1013B, 结构未定, 疑似 17 列 mesh/多条件标定, 待验证)
  * 0x1FA1-0x1FA3  3 字节标记 80 00 91 (待验证)
  * 其余 0xFF
可复现: 本脚本只读, 不改 EEPROM。把 LSC 与 flat-field 图对比即可硬件验证。
"""
import sys, struct

def parse(path):
    d = open(path, 'rb').read()
    assert len(d) == 8192, 'expect 8KB 24c64 image, got %d' % len(d)
    # --- LSC: 4 channels x 221, interleaved stride 4, big-endian u16 ---
    LSC_OFF, MESH, CH = 0x6c, 17 * 13, 4
    raw = [struct.unpack_from('>H', d, LSC_OFF + 2 * i)[0] for i in range(MESH * CH)]
    chans = [raw[i::CH] for i in range(CH)]           # chans[c][node]
    def corr(a, b):
        ma = sum(a) / len(a); mb = sum(b) / len(b)
        da = sum((x - ma) ** 2 for x in a) ** .5
        db = sum((y - mb) ** 2 for y in b) ** .5
        return sum((x - ma) * (y - mb) for x, y in zip(a, b)) / (da * db)
    return d, chans, corr

def dump_grid(c):
    for r in range(13):
        print('   ' + ' '.join('%4d' % c[r * 17 + cc] for cc in range(17)))

if __name__ == '__main__':
    d, chans, corr = parse(sys.argv[1])
    print('LSC @0x6C: 4ch x 17x13, interleaved BE u16, 1023=nominal(center)')
    print('  min/max per ch:', [(min(c), max(c)) for c in chans])
    print('  corr ch0-1=%.4f ch0-2=%.4f ch0-3=%.4f' % (corr(chans[0], chans[1]), corr(chans[0], chans[2]), corr(chans[0], chans[3])))
    print('  channel0 17x13 grid:')
    dump_grid(chans[0])
    print()
    print('header 0x00-0x6c:', d[:0x6c].hex(' '))
    print('block D 0x0ec9..0x12be:', d[0xec9:0xec9 + 64].hex(' '), '...')
    print('marker 0x1fa1:', d[0x1fa1:0x1fa4].hex(' '))
