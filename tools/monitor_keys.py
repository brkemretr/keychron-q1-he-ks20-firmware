#!/usr/bin/env python3
"""
Inspects live real-time Hall Effect telemetry (ADC raw values, zero baseline,
travel distance in mm, and key state) directly over Raw HID on Windows.
"""

import ctypes
from ctypes import wintypes
import time
import sys

kernel32 = ctypes.windll.kernel32

def open_device():
    # Enumerate Keychron Q1 HE Raw HID interface (Usage Page 0xFF60, Usage 0x61, MI_01)
    # Typically: \\?\hid#vid_3434&pid_0b11&mi_01#...
    import subprocess
    ps_script = "(Get-PnpDevice -PresentOnly | Where-Object { $_.InstanceId -like 'HID\\VID_3434&PID_0B1*MI_01*' }).InstanceId"
    out = subprocess.check_output(["powershell", "-NoProfile", "-Command", ps_script]).decode(errors="ignore").strip()
    if not out:
        print("Keychron Q1 HE (VID: 0x3434, PID: 0x0B10/0x0B11, MI_01) not found!")
        return None
    out = out.splitlines()[0].strip()
    
    # Format Windows device interface path
    instance = out.replace('\\', '#')
    path = f"\\\\?\\{instance}#{{4d1e55b2-f16f-11cf-88cb-001111000030}}"
    handle = kernel32.CreateFileW(path, 0xC0000000, 3, None, 3, 0, None)
    if handle == -1:
        print(f"Failed to open device handle: {path}")
        return None
    return handle

def get_key_telemetry(handle, row, col):
    req = bytearray(33)
    req[0] = 0
    req[1] = 0xA9
    req[2] = 0x30 # AMC_GET_REALTIME_TRAVEL
    req[3] = row
    req[4] = col
    w = wintypes.DWORD()
    kernel32.WriteFile(handle, (ctypes.c_char * 33).from_buffer(req), 33, ctypes.byref(w), None)
    res = bytearray(33)
    r = wintypes.DWORD()
    kernel32.ReadFile(handle, (ctypes.c_char * 33).from_buffer(res), 33, ctypes.byref(r), None)

    val = res[8] | (res[9] << 8)
    zero = res[10] | (res[11] << 8)
    full = res[12] | (res[13] << 8)
    travel_raw = res[7]
    state = "PRESSED" if res[14] == 0x11 else "RELEASED"
    mm = travel_raw / 60.0
    return {
        'row': row, 'col': col,
        'val': val, 'zero': zero, 'full': full,
        'delta': val - zero, 'travel': travel_raw, 'mm': mm, 'state': state
    }

if __name__ == '__main__':
    h = open_device()
    if not h:
        sys.exit(1)
    
    test_keys = [
        (0, 0, 'ESC'), (1, 1, '1'), (1, 2, '2'), (2, 1, 'Q'), (2, 2, 'W'),
        (2, 3, 'E'), (3, 1, 'A'), (3, 2, 'S'), (3, 3, 'D'), (5, 6, 'SPACE')
    ]
    print(f"{'Key':<7} | {'ADC Val':<8} | {'Zero':<8} | {'Delta':<8} | {'Travel':<12} | {'State':<9}")
    print("-" * 65)
    for r, c, name in test_keys:
        info = get_key_telemetry(h, r, c)
        print(f"{name:<7} | {info['val']:<8} | {info['zero']:<8} | {info['delta']:<8} | {info['mm']:4.2f} mm ({info['travel']:2d}) | {info['state']:<9}")
    
    kernel32.CloseHandle(h)
