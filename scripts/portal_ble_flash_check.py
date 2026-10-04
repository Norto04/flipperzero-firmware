#!/usr/bin/env python3
"""Require two free flash pages below the selected Full BLE stack."""

import sys
from pathlib import Path

from flipper.assets.coprobin import CoproBinary
from fwsize import Main

root = Path(__file__).resolve().parent.parent
sections = Main()._get_elf_sections(str(root / "build/f7-firmware-C/firmware.elf"))
radio = CoproBinary(
    str(root / "lib/stm32wb_copro/firmware/stm32wb5x_BLE_Stack_full_fw.bin")
).get_flash_load_addr()
full_image = (
    root / "lib/stm32wb_copro/firmware/stm32wb5x_BLE_Stack_full_fw.bin"
).read_bytes()
packages = list((root / "dist/f7-C").glob("*/update.fuf"))
if not packages or any(
    (p.parent / "radio.bin").read_bytes() != full_image for p in packages
):
    sys.exit("Update package must contain the exact Full BLE radio image")
free = radio - sections[".free_flash"][1]
print(f"Full BLE stack flash margin: {free} bytes (minimum 8192)")
sys.exit(0 if free >= 8192 else 1)
