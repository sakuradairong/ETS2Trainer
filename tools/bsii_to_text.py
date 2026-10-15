#!/usr/bin/env python3
"""BSII v3 -> SiiNunit text converter (dev exploration tool).

Port of cpp/src/bsii.cpp decodeImpl for research; the C++ implementation
remains the production path.  Usage: python bsii_to_text.py <in.bsii> <out.sii>
"""
import struct
import sys

TOKEN_DIGITS = "0123456789abcdefghijklmnopqrstuvwxyz_"


class R:
    def __init__(self, b):
        self.d = b
        self.p = 0

    def take(self, n):
        if self.p + n > len(self.d):
            raise EOFError("premature end")
        v = self.d[self.p:self.p + n]
        self.p += n
        return v

    def u8(self):
        return self.take(1)[0]

    def u16(self):
        return struct.unpack("<H", self.take(2))[0]

    def u32(self):
        return struct.unpack("<I", self.take(4))[0]

    def u64(self):
        return struct.unpack("<Q", self.take(8))[0]

    def f32(self):
        return struct.unpack("<f", self.take(4))[0]

    def s(self):
        return self.take(self.u32()).decode("utf-8", "replace")

    def remaining(self):
        return len(self.d) - self.p


def anon_id(v):
    if v == 0:
        return "_nameless.0"
    out = "_nameless"
    first = True
    for shift in range(48, -1, -16):
        part = (v >> shift) & 0xFFFF
        if first and part == 0:
            continue
        out += (".%x" if first else ".%04x") % part
        first = False
    return out


def decode_token(raw):
    n = raw & ~(1 << 63)
    if n == 0:
        return ""
    chars = []
    while n:
        digit = n % 38
        chars.append(TOKEN_DIGITS[digit - 1])
        n //= 38
    return "".join(chars)


def complex_id(r):
    ln = r.u8()
    if ln == 0:
        return "null"
    if ln == 255:
        return anon_id(r.u64())
    parts = []
    for _ in range(ln):
        p = decode_token(r.u64())
        parts.append(p)
    return ".".join(parts)


def scalar(r, t):
    if t == 0x01:
        return '"%s"' % r.s().replace("\\", "\\\\").replace('"', '\\"')
    if t == 0x03:
        tok = decode_token(r.u64())
        return tok if tok else '""'
    if t == 0x05:
        return "&%08x" % struct.unpack("<I", r.take(4))[0]
    if t == 0x07:
        a, b = r.f32(), r.f32()
        return "(%s, %s)" % (a, b)
    if t == 0x09:
        a, b, c = r.f32(), r.f32(), r.f32()
        return "(%s, %s, %s)" % (a, b, c)
    if t == 0x11:
        a, b, c = struct.unpack("<iii", r.take(12))
        return "(%d, %d, %d)" % (a, b, c)
    if t == 0x17:
        a, b, c, d = r.f32(), r.f32(), r.f32(), r.f32()
        return "(%s; %s, %s, %s)" % (a, b, c, d)
    if t == 0x19:
        x, y, z, hb = r.f32(), r.f32(), r.f32(), r.f32()
        qw, qx, qy, qz = r.f32(), r.f32(), r.f32(), r.f32()
        ib = int(hb)
        bx = ((ib & 0xFFF) - 2048) * 512
        bz = (((ib >> 12) & 0xFFF) - 2048) * 512
        return "(%s, %s, %s) (%s; %s, %s, %s)" % (x + bx, y, z + bz, qw, qx, qy, qz)
    if t == 0x25:
        return str(struct.unpack("<i", r.take(4))[0])
    if t in (0x27, 0x2F):
        return str(r.u32())
    if t == 0x2B:
        return str(r.u16())
    if t == 0x31:
        return str(struct.unpack("<q", r.take(8))[0])
    if t == 0x33:
        return str(r.u64())
    if t == 0x35:
        return "true" if r.u8() else "false"
    if t == 0x37:
        raise NotImplementedError("enum in arrays not needed for research")
    if t in (0x39, 0x3B, 0x3D):
        return complex_id(r)
    raise ValueError("unknown field type 0x%x" % t)


ARRAY_ELEM = {0x02: 0x01, 0x04: 0x03, 0x06: 0x05, 0x0A: 0x09, 0x12: 0x11,
              0x18: 0x17, 0x1A: 0x19, 0x26: 0x25, 0x28: 0x27, 0x2C: 0x2B,
              0x32: 0x31, 0x34: 0x33, 0x36: 0x35, 0x3A: 0x39, 0x3C: 0x3B}
SCALARS = {0x01, 0x03, 0x05, 0x07, 0x09, 0x11, 0x17, 0x19, 0x25, 0x27,
           0x2B, 0x2F, 0x31, 0x33, 0x35, 0x37, 0x39, 0x3B, 0x3D}


def main():
    src, dst = sys.argv[1], sys.argv[2]
    b = open(src, "rb").read()
    assert b[:4] == b"BSII" and struct.unpack("<I", b[4:8])[0] == 3
    r = R(b)
    r.p = 8
    out = ["SiiNunit\n{"]
    schemas = {}
    while True:
        block = r.u32()
        if block == 0:
            if r.u8() == 0:
                break
            sid = r.u32()
            name = r.s()
            fields = []
            enums = {}
            while True:
                ftype = r.u32()
                if ftype == 0:
                    break
                fname = r.s()
                if ftype == 0x37:
                    emap = {}
                    for _ in range(r.u32()):
                        k = r.u32()
                        emap[k] = r.s()
                    enums[len(fields)] = emap
                fields.append((ftype, fname))
            schemas[sid] = (name, fields, enums)
            continue
        name, fields, enums = schemas[block]
        uid = complex_id(r)
        out.append("%s : %s {" % (name, uid))
        for i, (ftype, fname) in enumerate(fields):
            if ftype in ARRAY_ELEM:
                count = r.u32()
                out.append("%s: %d" % (fname, count))
                et = ARRAY_ELEM[ftype]
                for j in range(count):
                    out.append("%s[%d]: %s" % (fname, j, scalar(r, et)))
            else:
                if ftype == 0x37:
                    k = r.u32()
                    val = enums.get(i, {}).get(k, "?%d" % k)
                else:
                    val = scalar(r, ftype)
                out.append("%s: %s" % (fname, val))
        out.append("}")
    out.append("}")
    assert r.remaining() == 0, "trailing bytes"
    open(dst, "w", encoding="utf-8", newline="\n").write("\n".join(out) + "\n")
    print("wrote %s (%d units)" % (dst, len([l for l in out if l.endswith(" {")])))


if __name__ == "__main__":
    main()
