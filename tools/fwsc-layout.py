#!/usr/bin/env python3
"""Read the flash layout recorded in an FM-1 update package (.fwsc).

    python3 tools/fwsc-layout.py PACKAGE.fwsc [...]

Reverses fm1_open's sdk/Package.cs (Wrap, BuildFlash): removes the product bytes interleaved
into the first 940 bytes, deciphers the UFW header and file records, finds flash.bin, and
deciphers the boot and application directories to print the partition records (app.bin,
VM, PRCT, BTIF, USER, ...) with their flash addresses and sizes, plus the update identity.
Read-only; does not touch any device.

Copyright (C) 2026 The CLATTER contributors. SPDX-License-Identifier: GPL-3.0-only
"""
import struct
import sys

AREA = 0x4000


def cipher(b, begin, length, state):
    for i in range(begin, begin + length):
        b[i] ^= state & 0xff
        state = ((state << 1) ^ (0x1021 if state & 0x8000 else 0)) & 0xffff


def crc(b, begin, length):
    v = 0
    for i in range(begin, begin + length):
        x = ((v >> 8) ^ b[i]) << 8
        for _ in range(8):
            x = ((x << 1) ^ (0x1021 if x & 0x8000 else 0)) & 0xffff
        v = ((v << 8) ^ x) & 0xffff
    return v


def unwrap(data):
    """Undo Wrap's product-byte interleave: one byte after every 47 source bytes, 20 times."""
    out, product = bytearray(), bytearray()
    i = 0
    while len(out) < 940:
        out += data[i:i + 47]
        product.append(data[i + 47])
        i += 48
    out += data[i:]
    ident = ''.join(chr(c - k - 1) for k, c in enumerate(product) if c != 125)
    return out, ident


def record(b, at):
    r = bytearray(b[at:at + 32])
    return r


def parse_record(r):
    crc_, addr, size, attr, last = struct.unpack_from('<HIIHH', r, 2)
    name = bytes(r[16:32]).split(b'\0')[0].split(b'\xff')[0].decode('ascii', 'replace')
    extra = struct.unpack_from('<I', r, 28)[0]
    return name, addr, size, attr, extra


def main():
    for path in sys.argv[1:]:
        raw = open(path, 'rb').read()
        ufw, ident = unwrap(raw)
        header = bytearray(ufw[:64])
        cipher(header, 0, 64, 0xffff)
        total = struct.unpack_from('<I', header, 4)[0]
        chip = bytes(header[16:24]).split(b'\0')[0].decode()
        files = []
        for i in range(2):
            rec = bytearray(ufw[64 + i * 80:64 + i * 80 + 80])
            cipher(rec, 0, 80, 0xffff)
            off, length = struct.unpack_from('<II', rec, 8)
            name = bytes(rec[64:80]).split(b'\0')[0].decode('ascii', 'replace')
            files.append((name, off, length))
        print(f'{path}\n  identity {ident!r}, chip {chip}, ufw {total} bytes, files {files}')
        name, off, length = files[0]
        flash = bytearray(ufw[off:off + length])
        boot = bytearray(flash[:160])
        for p in range(0, 160, 32):
            cipher(boot, p, 32, 0xffff)
        for p in range(32, 160, 32):
            print('  boot ', parse_record(boot[p:p + 32]))
        # application directory: ciphered with PublicCode ^ (p >> 2) per 32-byte record;
        # the public code is in the boot config descriptor (isd_config.ini, chipkey bits)
        for code in (0x980f,):
            head = bytearray(flash[AREA:AREA + 32])
            cipher(head, 0, 32, code)
            if not parse_record(head)[0].startswith('app_area_hea'):
                print(f'  application directory not readable with public code {code:#06x}')
                continue
            payload = parse_record(head)[2]
            # Package.cs ciphers [Area, Area + payload + 64) in 32-byte chunks, state code ^ (p >> 2)
            area = bytearray(flash[AREA:AREA + payload + 64])
            for p in range(0, payload + 64, 32):
                cipher(area, p, min(32, payload + 64 - p), code ^ (p >> 2))
            for p in range(0, 32 * 6, 32):
                print('  app  ', parse_record(area[p:p + 32]))
            for p in (payload, payload + 32):
                print('  rsv  ', parse_record(area[p:p + 32]))
        print(f'  flash.bin {length} bytes (written by the update: 0 .. {length:#x})')


if __name__ == '__main__':
    main()
