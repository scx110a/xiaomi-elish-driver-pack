#!/usr/bin/env python3
"""QTI Chromatix ("Parameter Parser V2.0.0") sensor-module .bin 只读解析器。
用法: python3 chromatix_parse.py <com.qti.sensormodule.*.bin>
容器格式(逆向结论, 2026-11-12):
  +0x00 char[24] "QTI Chromatix Header"
  +0x18 u64 total_size       +0x20 u32 version=2
  +0x28 char[32] "Parameter Parser V2.0.0 (...)"
  +0x98 u32 sectionTableOff  +0x9c u32 sectionCount
  sectionTable: {u64 id, u64 fileOff, u64 size} * count
  node table  : 从 0xf0 开始, 每项 0x48 字节:
      char name[24]; u64 f0,f1,f2,f3; u64 dataOff; u64 dataSize; u64 nodeIdx
      dataOff 相对 section[1].fileOff(数据段基址), 数据段按 node 顺序连续排列。
父/子关系在二进制里被展平; 容器 blob 常为"子节点索引(u64)+字面量"混合列表,
其中 u64 值若落在 [Fmin,Fmax] 且能被 byF 解析, 即引用另一个 node。
"""
import struct, sys, re

def parse(path):
    d = open(path, 'rb').read()
    assert d[:20] == b'QTI Chromatix Header', 'not a chromatix bin'
    total = struct.unpack_from('<I', d, 0x1c)[0]
    ver = struct.unpack_from('<I', d, 0x20)[0]
    parser = d[0x28:0x48].split(b'\0')[0].decode()
    st_off, st_cnt = struct.unpack_from('<II', d, 0x98)
    secs = [struct.unpack_from('<QQQ', d, st_off + i * 24) for i in range(st_cnt)]
    DATA = secs[1][1]
    nodes = []
    o = 0xf0
    while o + 0x48 <= len(d):
        nm = d[o:o + 24].split(b'\0')[0]
        if not nm or not all(32 <= b < 127 for b in nm):
            break
        f = struct.unpack_from('<6Q', d, o + 24)
        # f = f0,f1,f2,dataOff,dataSize,nodeIdx  (名字 +24 起 6 个 u64)
        nodes.append(dict(name=nm.decode(), off=o, f0=f[0], f1=f[1], f2=f[2],
                          dataOff=f[3], dataSize=f[4], idx=f[5]))
        o += 0x48
    return dict(path=path, size=len(d), total=total, ver=ver, parser=parser,
                secs=secs, DATA=DATA, nodes=nodes, raw=d)

def blob(m, n):
    return m['raw'][m['DATA'] + n['dataOff']: m['DATA'] + n['dataOff'] + n['dataSize']]

def main():
    m = parse(sys.argv[1])
    print('file', m['path'], 'size', m['size'], hex(m['total']), m['parser'])
    for s in m['secs']:
        print('  section id=%d off=%#x size=%#x' % s)
    print('nodes:', len(m['nodes']), 'data base:', hex(m['DATA']))
    for n in m['nodes']:
        if n['name'] in ('sensorName', 'laneAssign', 'streamConfiguration',
                         'resolutionData', 'resolutionInfo', 'moduleConfiguration',
                         'moduleName', 'sensorSlaveAddress', 'sensorI2CFrequencyMode',
                         'eepromName', 'EEPROMName', 'actuatorName', 'isComboMode',
                         'maxRollDegree', 'maxPitchDegree', 'maxYawDegree'):
            b = blob(m, n)
            u = struct.unpack('<%dQ' % (len(b) // 8), b) if b and len(b) % 8 == 0 else ()
            asc = b.split(b'\0')[0] if b and all(32 <= x < 127 or x == 0 for x in b[:60]) else b''
            print('%-22s idx=%d off=%#x size=%#x u64=%s ascii=%r' %
                  (n['name'], n['idx'], n['dataOff'], n['dataSize'], u, asc))

if __name__ == '__main__':
    main()
