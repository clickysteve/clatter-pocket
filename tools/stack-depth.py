#!/usr/bin/env python3
"""The deepest stack of a call path, from the frames in the linked firmware.

    wine llvm-objdump.exe -d build/fm1-replace/tree/build/dx7/sdk.elf > build/sdk.dis
    python3 tools/stack-depth.py build/sdk.dis pocket_panel_apply pocket_gen_track

Frames are read from each function's own prologue: registers pushed and `sp += -n`. Direct calls and
tail calls are followed. It stops at the SDK's functions, whose depth is not known, and names them.

Copyright (C) 2026 The CLATTER contributors. SPDX-License-Identifier: GPL-3.0-only
"""
import collections
import re
import sys

LABEL = re.compile(r'^((?!\.GJT)[A-Za-z_.$][\w.$]*):\s*$')
INSTRUCTION = re.compile(r'^\s*([0-9a-f]{6,8}):\s')
PUSH = re.compile(r'\[--sp\] = \{([^}]*)\}')
ADJUST = re.compile(r'\bsp \+= (-\d+)')
TARGET = re.compile(r'(?:call|goto)[^<]*<([^:>+ ]+)[^:>]*:\s*[0-9a-f]+\s*>')


def registers(text):
    count = 0
    for part in text.split(','):
        part = part.strip()
        span = re.match(r'r(\d+)-r(\d+)', part)
        count += abs(int(span.group(1)) - int(span.group(2))) + 1 if span else 1
    return count


frames, calls, current, body = collections.Counter(), collections.defaultdict(set), None, 0
for line in open(sys.argv[1], errors='ignore'):
    label = LABEL.match(line)
    if label:
        current, body = label.group(1), 0
        continue
    if not INSTRUCTION.match(line):
        continue
    body += 1
    if body <= 6:
        push = PUSH.search(line)
        if push:
            frames[current] += 4 * registers(push.group(1))
        adjust = ADJUST.search(line)
        if adjust:
            frames[current] += -int(adjust.group(1))
    target = TARGET.search(line)
    if target and target.group(1) != current:
        calls[current].add(target.group(1))

memo, unknown = {}, set()


def depth(function, path=()):
    if function in path:
        return 0, [function + ' (recursion)']
    if function in memo:
        return memo[function]
    if function not in frames and function not in calls:
        unknown.add(function)
    best, route = 0, []
    for callee in calls[function]:
        d, r = depth(callee, path + (function,))
        if d > best:
            best, route = d, r
    memo[function] = (frames[function] + best, [f'{function} {frames[function]}'] + route)
    return memo[function]


for root in sys.argv[2:]:
    total, route = depth(root)
    print(f'{root}: {total} bytes')
    print('   ' + ' > '.join(route))
