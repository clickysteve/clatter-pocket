#!/usr/bin/env python3
"""Checks a built CLATTER Pocket package against the limits the pre-flash audit set.

  python3 tools/check-firmware.py [build/fm1-replace]

Reads the SDK's firmware report and the ELF's symbol table (the link map also lists input
sections the linker discarded, so it cannot show what is absent): image size and headroom, heap, the stock flash
layout, the audit's SRAM and safety properties, and that the interface is linked and the stock
application is not. Prints the numbers; exit status 1 if any check fails.
"""
import json, re, struct, subprocess, sys

LIMIT = 453921          # Package.Create's largest application image (FM1-REPLACEMENT.md)
MARGIN = 16384          # flash kept free for hardware fixes
HEAP_MIN = 49152        # the SDK's minimum
LAYOUT = {"VM": (471040, 32768), "BTIF": (503808, 4096), "USER": (507904, 536576), "key_mac": (1044480, 4096)}

root = sys.argv[1] if len(sys.argv) > 1 else "build/fm1-replace"
report = json.load(open(f"{root}/firmware-report.json"))


def elf_symbols(path):
    d = open(path, "rb").read()
    shoff, count = struct.unpack_from("<I", d, 32)[0], struct.unpack_from("<H", d, 48)[0]
    sections = [struct.unpack_from("<10I", d, shoff + 40 * i) for i in range(count)]
    table = next(x for x in sections if x[1] == 2)
    strings = sections[table[6]][4]
    found = {}
    for at in range(table[4], table[4] + table[5], 16):
        name, value = struct.unpack_from("<II", d, at)
        found[d[strings + name:d.index(b"\0", strings + name)].decode()] = value
    return found


symbols = elf_symbols(f"{root}/tree/build/dx7/sdk.elf")
package = f"{root}/clatter-pocket-fm1-replace-UNVERIFIED.fwsc"
failed = 0


def check(ok, what):
    global failed
    failed += not ok
    print(("ok   " if ok else "FAIL ") + what)


image, heap = report["app_image_bytes"], report["runtime_heap_bytes"]
sections = {s["name"]: s["bytes"] for s in report["sections"]}
check(report["passed"], "the SDK's firmware checks passed")
check(image + MARGIN <= LIMIT, f"image {image}, headroom {LIMIT - image} (keep {MARGIN})")
check(heap >= HEAP_MIN, f"heap {heap}, SDK minimum {HEAP_MIN}")
print(f"     .ram0_data {sections['.ram0_data']}, .ram0_bss {sections['.ram0_bss']}")

layout = subprocess.run([sys.executable, "tools/fwsc-layout.py", package], capture_output=True, text=True).stdout
for name, (offset, size) in LAYOUT.items():
    row = next((l for l in layout.splitlines() if re.search(rf"\b{name}\b", l)), "")
    numbers = [int(n) for n in re.findall(r"\d+", row.replace(",", ""))]
    check(offset in numbers and size in numbers, f"{name} at {offset}, {size} bytes, as stock")

SRAM, FLASH = (0x01c00000, 0x01d00000), (0x02000000, 0x02100000)


def within(symbol, area):
    return area[0] <= symbols.get(symbol, 0) < area[1]


for symbol in ("groove_screen_init", "groove_screen_control", "pocket_panel_draw_band", "pocket_panel_apply"):
    check(within(symbol, FLASH), f"{symbol} linked, in Flash")
check(within("pocket_gen_generate", FLASH) and within("pocket_gen_mutate", FLASH), "the generator in Flash: only a button runs it")
for symbol in ("clatter_pocket_render", "clatter_pocket_output_level", "pocket_lxr_render_q23", "sqrtf",
               "pocket_seq_block", "pocket_lxr_trigger_now"):
    check(within(symbol, SRAM), f"{symbol} in SRAM")
check(within("lxr_packed_sine_table", FLASH), "packed tables in Flash, not SRAM")
for symbol in ("fm1_flash_read", "fm1_flash_write", "fm1_flash_erase", "fm1_flash_map"):
    check(symbol not in symbols, f"not linked: {symbol}")
for symbol in ("fm1_master_gain", "groove_project_apply_edit", "groove_tuning_apply"):
    check(symbol not in symbols, f"stock application absent: {symbol}")
print("result:", "FAILED" if failed else "passed")
sys.exit(1 if failed else 0)
