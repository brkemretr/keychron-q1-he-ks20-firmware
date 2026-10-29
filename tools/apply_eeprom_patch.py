#!/usr/bin/env python3
"""
Applies 0xFF padding to Sector 1 (0x4000..0x8000) of compiled Keychron Q1 HE firmware.
This prevents flashing tools (Keychron Launcher / QMK Toolbox) from overwriting
the emulated EEPROM sector on the STM32F401 MCU.
"""

import sys
import os

def patch_bin(input_path, output_path):
    with open(input_path, 'rb') as f:
        data = bytearray(f.read())

    print(f"Original size: {len(data)} bytes")
    if len(data) >= 0x8000:
        data[0x4000:0x8000] = b'\xFF' * 0x4000
        print("Patched 0x4000..0x8000 with 0xFF padding.")
    else:
        print("Warning: File smaller than 32KB, padding to 32KB")
        data.extend(b'\xFF' * (0x8000 - len(data)))
        data[0x4000:0x8000] = b'\xFF' * 0x4000

    with open(output_path, 'wb') as f:
        f.write(data)

    print(f"Saved patched binary to: {output_path}")

if __name__ == '__main__':
    if len(sys.argv) < 3:
        print("Usage: python apply_eeprom_patch.py <input.bin> <output_patched.bin>")
        sys.exit(1)
    patch_bin(sys.argv[1], sys.argv[2])
