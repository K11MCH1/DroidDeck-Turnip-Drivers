#!/usr/bin/env python3
"""
Idempotent Adreno 740 (Snapdragon 8 Gen 2) TP_UBWC_FLAG_HINT fixup for freedreno_devices.py.

TPL1_DBG_ECO_CNTL1.TP_UBWC_FLAG_HINT must match between every driver on the device, or
BLIT_OP_SCALE output is corrupted (freedreno_dev_info.h). Mesa leaves it off for the A740 to
match the older v6xx system driver. Newer firmware such as recent Samsung One UI ships a system
driver that sets it, so the UI glitches and flickers next to Turnip. This turns it on for the
A740 entry only (it also covers the Adreno X1-85, which shares that entry).

Only for devices whose system driver sets the hint. Elsewhere the same setting causes the glitch;
the same toggle at runtime is FD_DEV_FEATURES=enable_tp_ubwc_flag_hint=1.

Safe to run multiple times.
"""
import re
import sys

DEVICES_PY = "src/freedreno/common/freedreno_devices.py"
HINT = "GPUProps(enable_tp_ubwc_flag_hint = True)"

with open(DEVICES_PY, "r") as f:
    content = f.read()

anchor = re.search(r'GPUId\(chip_id=0xffff43050a01, name="[^"]*"\)', content, re.IGNORECASE)
if not anchor:
    print("  FATAL: A740 entry (0xffff43050A01) not found", file=sys.stderr)
    sys.exit(1)

# The A740's own A6xxGPUInfo: after its GPUId list, before the next add_gpus.
end = content.find("add_gpus(", anchor.end())
end = len(content) if end < 0 else end
block = content[anchor.end():end]

if HINT in block:
    print("  A740 TP_UBWC_FLAG_HINT already on, skipping")
    sys.exit(0)

props = re.search(r"\[a7xx_base, a7xx_gen2\],", block)
if not props:
    print("  FATAL: A740 props list '[a7xx_base, a7xx_gen2],' not found", file=sys.stderr)
    sys.exit(1)

start = anchor.end() + props.start()
content = (content[:start] + f"[a7xx_base, a7xx_gen2, {HINT}],"
           + content[anchor.end() + props.end():])

try:
    compile(content, DEVICES_PY, "exec")
except SyntaxError as e:
    print(f"  FATAL: syntax error after patching at line {e.lineno}: {e.msg}", file=sys.stderr)
    sys.exit(1)

with open(DEVICES_PY, "w") as f:
    f.write(content)
print("  A740: enable_tp_ubwc_flag_hint = True (8 Gen 2 One UI)")
