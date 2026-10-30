#!/usr/bin/env python3
"""Pack the LXR firmware's constant tables losslessly for the FM-1 build.

    make packed-tables      (builds build/host/dump_tables, runs this, writes the C file)
    python3 tools/pack-lxr-tables.py build/host/lxr_tables.bin engine/lxr_tables_packed.c

Input: the tables as compiled (host/dump_tables.c). Output: engine/lxr_tables_packed.c,
which engine/lxr_tables_unpack.c decodes into RAM at start-up. Format: see
lxr_tables_unpack.c. For each table the predictor order (0, 1 or 2) giving the smallest
output is chosen, and the residuals go through an adaptive binary range coder. The sawtooth is
exactly odd in every row, so only its first 513 values of each are packed. The tool decodes its
own output and stops unless every value round-trips.

Copyright (C) 2026 The CLATTER contributors. SPDX-License-Identifier: GPL-3.0-only
"""
import struct
import sys
import zlib

PROB_BITS, RATE = 12, 4
LENGTHS = 19        # bit lengths of a zigzag value, 0..18
SIZES = 12          # contexts from the size of the last two residuals
SAW_ROWS, SAW_ROW, SAW_KEPT = 11, 1024, 513

TABLES = [  # name, count, width
    ('sine_table', 4097, 2),
    ('triTable', 11 * 1024, 2),
    ('sawTable', 11 * 1024, 2),
    ('recTable', 11 * 1024, 2),
    ('transientData', 12 * 2205, 1),
]


def zig(v):
    return (v << 1) ^ (v >> 63) if v < 0 else v << 1


def residuals(values, order):
    out, x1, x2 = [], 0, 0
    for x in values:
        p = 2 * x1 - x2 if order == 2 else x1 if order == 1 else 0
        out.append(x - p)
        x2, x1 = x1, x
    return out


def bit_length(z):
    return z.bit_length()


class Encoder:
    """An LZMA-style binary range coder, with its carry handling."""

    def __init__(self):
        self.low, self.range, self.cache, self.pending = 0, 0xffffffff, 0, 1
        self.out = bytearray()

    def shift(self):
        if self.low < 0xff000000 or self.low >= 1 << 32:
            byte = self.cache
            while True:
                self.out.append((byte + (self.low >> 32)) & 0xff)
                byte = 0xff
                self.pending -= 1
                if not self.pending:
                    break
            self.cache = (self.low >> 24) & 0xff
        self.pending += 1
        self.low = (self.low & 0x00ffffff) << 8

    def normalise(self):
        while self.range < 1 << 24:
            self.range = (self.range << 8) & 0xffffffff
            self.shift()

    def bit(self, probs, i, b):
        bound = (self.range >> PROB_BITS) * probs[i]
        if b:
            self.range = bound
            probs[i] += ((1 << PROB_BITS) - probs[i]) >> RATE
        else:
            self.low += bound
            self.range -= bound
            probs[i] -= probs[i] >> RATE
        self.normalise()

    def raw(self, b):
        self.range >>= 1
        if b:
            self.low += self.range
        self.normalise()

    def done(self):
        for _ in range(5):
            self.shift()
        return bytes(self.out[1:])  # the first byte is always 0: the decoder starts after it


class Decoder:
    """Mirror of lxr_tables_unpack.c, to check the output before writing it."""

    def __init__(self, data):
        self.data, self.pos, self.range, self.code = data, 4, 0xffffffff, int.from_bytes(data[:4], 'big')

    def normalise(self):
        while self.range < 1 << 24:
            self.range = (self.range << 8) & 0xffffffff
            self.code = ((self.code << 8) | self.data[self.pos]) & 0xffffffff
            self.pos += 1

    def bit(self, probs, i):
        bound = (self.range >> PROB_BITS) * probs[i]
        if self.code < bound:
            self.range = bound
            probs[i] += ((1 << PROB_BITS) - probs[i]) >> RATE
            b = 1
        else:
            self.code -= bound
            self.range -= bound
            probs[i] -= probs[i] >> RATE
            b = 0
        self.normalise()
        return b

    def raw(self):
        self.range >>= 1
        b = 1 if self.code >= self.range else 0
        if b:
            self.code -= self.range
        self.normalise()
        return b


def new_probs():
    return [1 << (PROB_BITS - 1)] * (SIZES * LENGTHS + LENGTHS)


def encode(values, order):
    res = [zig(r) for r in residuals(values, order)]
    if max(res) >= 1 << (LENGTHS - 1):
        raise SystemExit('residual too large')
    e, probs, z1, z2 = Encoder(), new_probs(), 0, 0
    for z in res:
        n, c = bit_length(z), min(SIZES - 1, bit_length(2 * z1 + z2))
        for i in range(n):
            e.bit(probs, c * LENGTHS + i, 1)
        if n < LENGTHS - 1:
            e.bit(probs, c * LENGTHS + n, 0)
        if n > 1:  # below the leading one: the first bit modelled, the rest raw
            e.bit(probs, SIZES * LENGTHS + n, (z >> (n - 2)) & 1)
            for j in range(n - 3, -1, -1):
                e.raw((z >> j) & 1)
        z2, z1 = z1, z
    return e.done()


def decode(data, count, order, width):
    d, probs, out, x1, x2, z1, z2 = Decoder(data + bytes(8)), new_probs(), [], 0, 0, 0, 0
    for _ in range(count):
        n, c = 0, min(SIZES - 1, bit_length(2 * z1 + z2))
        while n < LENGTHS - 1 and d.bit(probs, c * LENGTHS + n):
            n += 1
        z = 1 << (n - 1) if n else 0
        if n > 1:
            z |= d.bit(probs, SIZES * LENGTHS + n) << (n - 2)
            for j in range(n - 3, -1, -1):
                z |= d.raw() << j
        z2, z1 = z1, z
        r = (z >> 1) ^ -(z & 1)
        p = 2 * x1 - x2 if order == 2 else x1 if order == 1 else 0
        x = p + r
        x = ((x + 32768) & 0xffff) - 32768 if width == 2 else ((x + 128) & 0xff) - 128
        out.append(x)
        x2, x1 = x1, x
    return out, d.pos


def main():
    src, dest = sys.argv[1], sys.argv[2]
    blob = open(src, 'rb').read()
    pos = 0
    lines = ['/* GENERATED by tools/pack-lxr-tables.py from the LXR firmware tables (wavetable.c,',
             ' * transientTables.c; Copyright (C) 2012-2026 Erica Synths, GPL-3.0-only with the terms in',
             ' * lxr/upstream/NOTICE.md). Lossless; engine/lxr_tables_unpack.c restores them. */',
             '#include "lxr_tables_packed.h"', '']
    total_in = total_out = 0
    for name, count, width in TABLES:
        size = count * width
        raw = blob[pos:pos + size]
        pos += size
        values = list(struct.unpack(f'<{count}{"h" if width == 2 else "b"}', raw))
        if name == 'sawTable':
            for r in range(SAW_ROWS):
                row = values[r * SAW_ROW:(r + 1) * SAW_ROW]
                if any(row[i] != -row[-i] for i in range(SAW_ROW)):
                    raise SystemExit(f'sawTable row {r} is not odd: the unpacker relies on it')
            values = [v for r in range(SAW_ROWS) for v in values[r * SAW_ROW:r * SAW_ROW + SAW_KEPT]]
            count = len(values)
        packed, order = min(((encode(values, o), o) for o in (0, 1, 2)), key=lambda t: len(t[0]))
        check, used = decode(packed, count, order, width)
        packed = packed[:used]  # what a flush leaves over the end is never read
        if check != values or decode(packed, count, order, width)[0] != values:
            raise SystemExit(f'{name}: round trip failed')
        total_in += size
        total_out += len(packed)
        print(f'{name:14} {size:6d} -> {len(packed):6d} bytes (order {order})')
        body = ',\n'.join(','.join(str(b) for b in packed[i:i + 24]) for i in range(0, len(packed), 24))
        lines += [f'static const uint8_t {name}_data[{len(packed)}] = {{', body, '};',
                  f'const struct lxr_packed_table lxr_packed_{name} = {{{name}_data, {len(packed)}u, {count}u, {width}u, {order}u}};', '']
    vol = struct.unpack('<69I', blob[pos:pos + 276])
    pos += 276
    if pos != len(blob):
        raise SystemExit('unexpected input size')
    crc = zlib.crc32(blob[:pos - 276])  # the tables as the firmware must have them, after unpacking
    lines += [f'const uint32_t lxr_packed_crc = 0x{crc:08x}u;', '']
    floats = ', '.join(f'{struct.unpack("<f", struct.pack("<I", v))[0].hex()}f' for v in vol)
    lines += ['/* transientVolumeTable, verbatim (exact hexadecimal float literals) */',
              '#include "Soundengine/config.h"',
              f'INDTCM const float transientVolumeTable[69] = {{{floats}}};', '']
    open(dest, 'w').write('\n'.join(lines))
    print(f'total          {total_in:6d} -> {total_out:6d} bytes ({100 * total_out / total_in:.1f} %)')


if __name__ == '__main__':
    main()
