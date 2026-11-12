#!/usr/bin/env python3
"""就地修改 DTB 中 /fingerprint 的 firmware-name 属性（保持长度，其余字节不变）。

用法: patch-firmware-name.py <input.dtb> <output.dtb> [新名字]
默认把 firmware-name 从 "fingerpr" 改成 "gfenu"。
只改该属性的值，新值不足原长度时用 NUL 补齐（对 of_property_read_string /
libfprint 的 g_strstrip / udev $attr{} 都是 C 字符串，尾部 NUL 安全）。
"""
import struct, sys

def align4(n):
    return (n + 3) & ~3

def patch(src, dst, new_name="gfenu"):
    d = bytearray(open(src, "rb").read())
    magic, tot, off_struct, off_str, off_rsv, ver, last, bc, size_str, size_struct = \
        struct.unpack(">10I", d[:40])
    assert magic == 0xD00DFEED, "not a DTB"
    p = off_struct
    stack = []
    patched = None
    while True:
        tok, = struct.unpack(">I", d[p:p+4]); p += 4
        if tok == 1:  # BEGIN_NODE
            e = d.index(b"\x00", p)
            name = d[p:e].decode()
            p = align4(e + 1)
            stack.append(name)
        elif tok == 2:  # END_NODE
            stack.pop()
        elif tok == 3:  # PROP
            ln, noff = struct.unpack(">II", d[p:p+8]); p += 8
            pn = d[off_str + noff: d.index(b"\x00", off_str + noff)].decode()
            if stack and stack[-1] == "fingerprint" and pn == "firmware-name":
                old = bytes(d[p:p+ln])
                new = new_name.encode() + b"\x00" * (ln - len(new_name))
                assert len(new) == ln, "new name longer than existing property"
                d[p:p+ln] = new
                patched = (old, new)
            p = align4(p + ln)
        elif tok == 4:  # NOP
            pass
        elif tok == 9:  # END
            break
        else:
            raise SystemExit("bad FDT token %d" % tok)
    assert patched, "fingerprint/firmware-name property not found"
    open(dst, "wb").write(d)
    print("patched %r -> %r (length kept)" % patched)

if __name__ == "__main__":
    src, dst = sys.argv[1], sys.argv[2]
    patch(src, dst, sys.argv[3] if len(sys.argv) > 3 else "gfenu")
