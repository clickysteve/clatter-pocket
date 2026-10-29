#!/usr/bin/env python3
"""Flash usage of an FM-1 link, from the linker map (sdk.map).

    python3 tools/flash-map.py build/fm1-stock/tree/build/dx7/sdk.map [--symbols N] [--filter TEXT]
    python3 tools/flash-map.py --diff STOCK.map OTHER.map   (Flash symbols of STOCK absent from
                                                            OTHER, by input file, with sizes)

Counts every input section placed in an output section that is stored in Flash: .text
(executed in place) and the load images of .ram0_data, .boot_info and .cache_ram_data
(copied to RAM at boot). .ram0_bss is RAM only and listed separately. Totals are given per
input file (archive member, application object, or the LTO object that holds all
application code), and optionally per symbol.

Copyright (C) 2026 The CLATTER contributors. SPDX-License-Identifier: GPL-3.0-only
"""
import collections
import re
import sys

FLASH = {'.text', '.ram0_data', '.boot_info', '.cache_ram_data'}
RAM_ONLY = {'.ram0_bss'}


def parse(path):
    """Yield (output_section, input_section, address, size, file, [symbols])."""
    out = None
    entries = []
    pending = None
    started = False
    with open(path, encoding='utf-8', errors='replace') as f:
        for line in f:
            line = line.rstrip('\n')
            if line.startswith('Linker script and memory map'):
                started = True
                continue
            if not started:
                continue
            m = re.match(r'^(\.[\w.]+|/DISCARD/)\s*(0x[0-9a-f]+)?', line)
            if m and not line.startswith(' '):
                out = m.group(1)
                continue
            # " .text.name  0xaddr  0xsize  file"  or " .text.name" then the numbers on the next line
            m = re.match(r'^ (\.[^\s]+|COMMON)\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(.+)$', line)
            if m:
                entries.append([out, m.group(1), int(m.group(2), 16), int(m.group(3), 16), m.group(4).strip(), []])
                pending = None
                continue
            m = re.match(r'^ (\.[^\s]+|COMMON)$', line)
            if m:
                pending = m.group(1)
                continue
            m = re.match(r'^\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(.+)$', line)
            if m and pending:
                entries.append([out, pending, int(m.group(1), 16), int(m.group(2), 16), m.group(3).strip(), []])
                pending = None
                continue
            m = re.match(r'^\s+(0x[0-9a-f]+)\s+([A-Za-z_][\w.$]*)$', line)
            if m and entries and entries[-1][3]:
                entries[-1][5].append((int(m.group(1), 16), m.group(2)))
    return entries


def short(name):
    name = name.replace('\\', '/')
    m = re.search(r'lib/([^/]+\.a)\((.+)\)$', name)
    if m:
        return f'{m.group(1)}({m.group(2)})'
    if 'lto-llvm' in name:
        return 'LTO: application code (app/*.c, LTO units)'
    return name.split('/')[-1]


def flash_symbols(path):
    """{(input file, symbol): bytes} for every Flash-resident symbol."""
    out = collections.Counter()
    for o, sec, addr, size, f, syms in parse(path):
        if o not in FLASH or not size:
            continue
        syms = sorted(syms)
        if not syms:
            out[(short(f), sec)] += size
        for i, (a, n) in enumerate(syms):
            end = syms[i + 1][0] if i + 1 < len(syms) else addr + size
            out[(short(f), n)] += end - a
    return out


def diff(a_path, b_path):
    a, b = flash_symbols(a_path), flash_symbols(b_path)
    present = {n for _, n in b}
    gone = collections.defaultdict(list)
    for (f, n), size in a.items():
        if n not in present:
            gone[f].append((n, size))
    total = sum(sz for v in gone.values() for _, sz in v)
    print(f'Flash symbols of {a_path}\nabsent from {b_path}: {total} bytes\n')
    for f, items in sorted(gone.items(), key=lambda kv: -sum(sz for _, sz in kv[1])):
        print(f'{sum(sz for _, sz in items):8d}  {f} ({len(items)} symbols)')
        for n, size in sorted(items, key=lambda t: (-t[1], t[0])):
            print(f'{size:8d}    {n}')


def main():
    args = sys.argv[1:]
    if not args:
        raise SystemExit(__doc__)
    if args[0] == '--diff':
        return diff(args[1], args[2])
    path = args[0]
    nsym = int(args[args.index('--symbols') + 1]) if '--symbols' in args else 0
    filt = args[args.index('--filter') + 1] if '--filter' in args else None
    entries = [e for e in parse(path) if e[3]]
    flash = collections.Counter()
    ram = collections.Counter()
    by_out = collections.Counter()
    symbols = []
    for out, sec, addr, size, f, syms in entries:
        key = short(f)
        if out in FLASH:
            flash[key] += size
            by_out[out] += size
            # symbol sizes: distance to the next symbol in this input section
            syms = sorted(syms)
            for i, (a, n) in enumerate(syms):
                end = syms[i + 1][0] if i + 1 < len(syms) else addr + size
                symbols.append((end - a, n, out, key))
            if not syms:
                symbols.append((size, sec, out, key))
        elif out in RAM_ONLY:
            ram[key] += size
    total = sum(flash.values())
    print(f'{path}\nFlash-resident bytes: {total}  ' + '  '.join(f'{k} {v}' for k, v in sorted(by_out.items())))
    print(f'\n{"bytes":>8}  flash by input file')
    for k, v in flash.most_common():
        if not filt or filt in k:
            print(f'{v:8d}  {k}')
    print(f'\n{"bytes":>8}  RAM-only (.ram0_bss) by input file')
    for k, v in ram.most_common(25):
        if not filt or filt in k:
            print(f'{v:8d}  {k}')
    if nsym:
        print(f'\n{"bytes":>8}  largest flash symbols')
        for s, n, out, key in sorted(symbols, reverse=True)[:nsym]:
            if not filt or filt in key:
                print(f'{s:8d}  {n}  [{out}, {key}]')


if __name__ == '__main__':
    main()
