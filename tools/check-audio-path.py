#!/usr/bin/env python3
"""Everything the audio interrupt can reach must be in SRAM: Flash cannot be read while it is
being erased or programmed, so a fetch from it then would fail. The emulator cannot show this, it
does not stall fetches. So this walks the calls of the linked firmware.

    wine llvm-objdump.exe -d build/fm1-replace/tree/build/dx7/sdk.elf > build/sdk.dis
    python3 tools/check-audio-path.py build/sdk.dis

From the audio entry points it follows every direct call and tail call through SRAM, and reports
any that leaves it. Exit status 1 if one does. The two function pointers on the path are roots
here (pocket_seq_block, the block hook, and pocket_lxr_trigger_now, the sequencer's emit), and
tools/check-firmware.py checks that their targets are in SRAM.

Copyright (C) 2026 The CLATTER contributors. SPDX-License-Identifier: GPL-3.0-only
"""
import collections
import re
import sys

ROOTS = ('clatter_pocket_render', 'clatter_pocket_output_level', 'fm1_output_mix', 'pocket_lxr_render_q23',
         'pocket_seq_block', 'pocket_lxr_trigger_now')
SRAM, FLASH = (0x01c00000, 0x01d00000), (0x02000000, 0x02100000)
# .GJT labels are a function's own jump tables: the code after one is still that function's
LABEL = re.compile(r'^((?!\.GJT)[A-Za-z_.$][\w.$]*):\s*$')
INSTRUCTION = re.compile(r'^\s*([0-9a-f]{6,8}):\s')
TARGET = re.compile(r'(?:call|goto)[^<]*<[^:>]*:\s*([0-9a-f]+)\s*>')

current, start, calls = None, {}, collections.defaultdict(set)
for line in open(sys.argv[1], errors='ignore'):
    label = LABEL.match(line)
    if label:
        current = label.group(1)
        continue
    at = INSTRUCTION.match(line)
    if not at:
        continue
    start.setdefault(current, int(at.group(1), 16))
    target = TARGET.search(line)
    if target:
        calls[current].add(int(target.group(1), 16))

named = {address: name for name, address in start.items()}
missing = [r for r in ROOTS if r not in start]
seen, stack, leaving = set(), [r for r in ROOTS if r in start], set()
while stack:
    function = stack.pop()
    if function in seen:
        continue
    seen.add(function)
    for address in calls[function]:
        name = named.get(address)
        if name is None:  # a branch inside the function
            if FLASH[0] <= address < FLASH[1]:
                leaving.add((function, hex(address)))
        elif FLASH[0] <= address < FLASH[1]:
            leaving.add((function, name))
        elif SRAM[0] <= address < SRAM[1]:
            stack.append(name)

# a walk that stops short would pass for the wrong reason
for function in ('calcDrumVoiceSyncBlock', 'Snare_calcSyncBlock', 'HiHat_calcSyncBlock', 'Cymbal_calcSyncBlock', 'midiParser_ccHandler', 'lockVoice'):
    if function not in seen:
        leaving.add(('the walk', f'{function}, which it never reached'))
print(f'{len(seen)} functions reachable from the audio interrupt, all checked')
for function, target in sorted(leaving):
    print(f'FAIL {function} calls {target}, in Flash')
for root in missing:
    print(f'FAIL no such function: {root}')
sys.exit(1 if leaving or missing else 0)
