# Keychron Q1 HE — Gateron KS-20 / Reverse-Polarity Firmware

[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue.svg)](LICENSE)
[![QMK Firmware](https://img.shields.io/badge/QMK-Compatible-brightgreen.svg)](https://qmk.fm)
[![Keychron Launcher](https://img.shields.io/badge/Keychron%20Launcher-Supported-blue.svg)](https://launcher.keychron.com)
[![VIA Compatible](https://img.shields.io/badge/VIA-Compatible-yellow.svg)](https://usevia.app)

QMK firmware patches and pre-compiled binaries for the **Keychron Q1 HE (ISO Knob & ANSI Knob)** providing support for **Gateron KS-20 magnetic switches** (such as **Magnetic Jade**, **Jade Pro**, **Orchid**, **White**, and other reverse-polarity Hall Effect / TMR switches).

All modified source code is included in [`src/`](src/) under the GNU General Public License v2.0.

---

## Technical Background: Stock Hardware vs. KS-20

The first-generation Keychron Q1 HE comes standard with **Gateron Jupiter** magnetic switches. Swapping them for popular aftermarket switches like **Gateron KS-20 / Magnetic Jade** breaks stock firmware due to hardware and firmware constraints:

1. **Opposite Magnetic Polarity:**
   - **Factory Q1 HE (Jupiter / Nebula):** The sensor circuitry is biased for **South-Pole Down (SPD)** magnets. Pressing down causes ADC readings to **decrease** (from ~3120 down to ~1940).
   - **Gateron KS-20 / Magnetic Jade:** These switches feature **North-Pole Down (NPD)** orientation. When placed into an SPD-biased board, pressing down causes the voltage to **increase** toward the 3.3V power supply rail (from ~3570–3850 up to ~4094).
   - Stock polynomial lookup tables and calibration routines expect decreasing voltage and interpret KS-20 switches as stuck or invalid.
   *(Note: Newer Keychron models shipping with "Ultra Fast Lime" switches are factory-configured for native NPD and run NPD switches without polarity patches).*
2. **Esc Key Bootmagic Lock (DFU Mode):**
   - On plug-in, the stock bootloader checks whether Esc is pressed by looking for an ADC drop. With KS-20 resting at high ADC, the keyboard detects a false Esc press on power-up, trapping the board in STM32 DFU bootloader mode.
3. **Resting Thermal Drift:**
   - Hall/TMR sensors naturally drift by +15–30 ADC counts as the PCB warms up. On high-baseline keys, unhandled drift can cross shallow actuation thresholds (e.g. 0.2mm), causing ghost-presses when untouched.
4. **Wireless Deep Sleep (`PM_STOP`) Wake-Up Failure:**
   - In wireless modes (2.4 GHz / Bluetooth), the Q1 HE powers down the 3.3V Hall sensor rail (`C13`) during deep sleep (`PM_STOP`) and relies on a low-power magnetic comparator on pin `C5` (`ANALOG_MATRIX_WAKEUP_PIN`) to wake the MCU. Because that comparator is hardware-biased for South-Pole Down magnets, North-Pole Down KS-20 switches cannot trigger the hardware wake-up interrupt. Furthermore, calling cold-boot initialization routines (`CALIB_ZERO_TRAVEL_POWER_ON` and `Esc` DFU check) upon `lpm_wakeup()` swallows wake-up keystrokes or locks the board into USB DFU mode while wireless.
5. **Switch Mixing Limitation:**
   - **All switches on the board must currently be KS-20 / reverse-polarity.** A mixed setup (some stock Jupiter and some KS-20) is not reliably supported due to opposing travel curves, calibration conflicts, and bootloader Esc detection constraints.

---

## Hardware Telemetry & Limitations: What the Measurements Show

To understand how KS-20 switches actually behave on the Q1 HE, here are the real-world measurements taken across unmodded and hardware-spaced setups.

### 1. Mechanical Travel vs. TMR Rail Saturation
- **Physical Travel:** Gateron KS-20 / Magnetic Jade switches have a physical stroke of **~3.5 mm** (not a full 4.0 mm).
- **Op-Amp Rail Saturation (~4094 ADC):** 
  Because KS-20 magnets have high flux density, the sensor circuit reaches positive rail saturation (**3.30V = 4094 counts**) before the stem completes its full mechanical stroke.
  - **Stock / Unmodded Board:** Baseline rests at **~3820 ADC**. The 4094 ceiling is reached at roughly **~2.5 mm – 2.6 mm** of travel. The usable dynamic range is compressed to **~240 ADC counts** (~7% of the 3.3V rail). From 2.6mm to 3.5mm (the last ~0.9mm), the sensor is saturated and cannot measure further movement.
  - **Why Deep Actuation (e.g. 3.8mm) Triggers Early:** When setting 3.8mm in Keychron Launcher, the firmware expects an ADC near 4088–4094. Because saturation occurs around 2.6mm–2.9mm, the key actuates while physical travel remains before bottom-out.

---

## Hardware Mod: 1.0mm Plate Elevation & 3D-Printable Acoustic Spacer Pad

To investigate whether the 240-count compressed range could be expanded, we tested elevating the switch plate by **1.0 mm** above the PCB. To eliminate the resulting 1.0 mm hollow air gap beneath the switches and improve the keyboard's sound profile (acoustics) and switch seating stability, custom **3D-printable 1.0 mm under-switch spacer / acoustic pad models** are included in [`3d_print/`](3d_print/).

### 3D Print Files (`3d_print/`):
| File | Format | Description |
| :--- | :--- | :--- |
| [`3d_print/Q1-HE_PCB_PLATE.stl`](3d_print/Q1-HE_PCB_PLATE.stl) | `.stl` | 1.0 mm PCB / under-switch acoustic spacer pad mesh for universal slicers |
| [`3d_print/Q1-HE_SPACER_PLATE.3mf`](3d_print/Q1-HE_SPACER_PLATE.3mf) | `.3mf` | Pre-configured 3MF project file for 1.0 mm spacer & acoustic pad printing |

- **Recommended Materials:**
  - **TPU (95A):** Best for acoustic dampening ("thocky" / muted bottom-out), vibration absorption, and a compliant Poron/silicone-like under-switch pad feel while maintaining the 1.0 mm spacing.
  - **PETG:** Ideal for a firmer, dimensionally rigid 1.0 mm mechanical spacer that still fills the sub-switch cavity and prevents hollow case resonance.
- **Recommended Slicer Settings:** `0.20 mm` layer height (5 solid layers = exact `1.00 mm` thickness), `100%` infill, no supports required.

### Measured Telemetry Results (1.0mm Elevation):
| Metric | Stock Unmodded | 1.0mm Plate Spacer Pad | Difference |
| :--- | :--- | :--- | :--- |
| **Resting Baseline (Zero)** | `~3780 – 3850 ADC` | `~3568 – 3610 ADC` | **-210 to -240 ADC counts lower** |
| **Bottom-Out Peak** | `4094 ADC` | `4094 ADC` | Hits rail ceiling |
| **Total Usable Dynamic Range (Delta)** | `~230 – 250 ADC` | `~510 – 526 ADC` | **+119% (2.2x resolution improvement!)** |
| **Voltage Band Utilized** | ~7% of 3.3V rail | ~16% of 3.3V rail | Significant SNR gain |

### Key Takeaway from the 1.0mm Spacer Pad Mod:
1. **Significant Resolution, Stability & Acoustic Gain:**
   Increasing the plate-to-PCB distance by 1.0 mm more than doubles the usable ADC delta (from ~240 to ~526 counts), dramatically improving Rapid Trigger precision and SNR, while the 3D-printed TPU/PETG pad fills the sub-switch void so switches sit flush without hollow acoustic reverb.
2. **Bottom-Out Saturation Persists:**
   Even with the 1.0 mm spacer pad, the sensor **still reaches the 4094 ceiling** at roughly **~2.9 mm – 3.0 mm** of stroke. While this shifts the blind zone deeper (leaving only the final ~0.5 mm saturated), it **does not completely eliminate saturation**.
   - Completely avoiding 4094 saturation would require elevating the plate by ~1.8 mm – 2.0 mm, which is physically impractical as it breaks PCB-mount stabilizers and exceeds standoff screw lengths.
   - Therefore, even with the 1.0 mm spacer pad, setting actuation to 3.8 mm in software will actuate at ~2.9–3.0 mm, slightly ahead of true mechanical bottom-out.

---

## Recommended Actuation Settings in Keychron Launcher

Based on verified telemetry across unmodded and spaced setups:

| Profile | Recommended Setting | Real-World Behavior |
| :--- | :--- | :--- |
| **Esports / Fast Response** | `0.4 mm – 0.8 mm` | Instantaneous response. Stable against resting drift. |
| **Balanced / Daily Typing** | `1.2 mm – 1.8 mm` | Standard mechanical feel, eliminates accidental actuation when resting fingers. |
| **Deep Actuation (Maximum Practical)** | `2.0 mm – 2.5 mm` | Triggers within the verified linear sensing window. |
| **Bottom-Out Warning** | `> 2.6 mm` (e.g. `3.0 mm – 3.8 mm`) | Enters or borders the op-amp rail saturation zone. The key will actuate before mechanical bottom-out is fully reached. |
| **Rapid Trigger** | `0.15 mm – 0.35 mm` | Clean, chatter-free rapid trigger within the `0.3 mm – 2.5 mm` active stroke zone. |

---

## Engineering Implementation & Source Code

All modified source files are located in [`src/`](src/):

- [`src/keyboards/keychron/common/analog_matrix/analog_matrix.c`](src/keyboards/keychron/common/analog_matrix/analog_matrix.c)
- [`src/keyboards/keychron/common/analog_matrix/analog_matrix.h`](src/keyboards/keychron/common/analog_matrix/analog_matrix.h)
- [`src/keyboards/keychron/common/analog_matrix/analog_matrix_scan.c`](src/keyboards/keychron/common/analog_matrix/analog_matrix_scan.c)
- [`src/keyboards/keychron/common/analog_matrix/action_rapid_trigger.c`](src/keyboards/keychron/common/analog_matrix/action_rapid_trigger.c)
- [`src/keyboards/keychron/common/analog_matrix/profile.c`](src/keyboards/keychron/common/analog_matrix/profile.c)
- [`src/keyboards/keychron/common/wireless/lpm.c`](src/keyboards/keychron/common/wireless/lpm.c)
- [`src/keyboards/keychron/common/wireless/lpm.h`](src/keyboards/keychron/common/wireless/lpm.h)
- [`src/keyboards/keychron/common/wireless/lpm_stm32f401.c`](src/keyboards/keychron/common/wireless/lpm_stm32f401.c)
- [`src/keyboards/keychron/q1_he/config.h`](src/keyboards/keychron/q1_he/config.h)

### Architectural Features:

1. **Released-Only Upward Resting Drift Tracking:**
   - `track_resting_baseline()` in [`analog_matrix.c`](src/keyboards/keychron/common/analog_matrix/analog_matrix.c) adapts `zero_travel` to ambient thermal drift only when a key is in `AKS_REGULAR_RELEASED` state, below its actuation point, and motionless within ±2 ADC counts for 1000 consecutive scans (1.0s).
   - Never modifies baseline while a key is `AKS_REGULAR_PRESSED`. Holding or feathering keys mid-stroke is completely protected from false re-zeroing.
2. **Keychron Auto-Calibration State Machine:**
   - Adapted Keychron's native multi-state calibration (`auto_caliration_check()`) for reverse-polarity voltage curves, requiring full bottom-out followed by a snap release into the resting zone.
3. **17-Point Piecewise Saturation LUT (`ks20_travel_lut`):**
   - Incorporates dynamic baseline scaling (`full - zero`), an 8% resting deadzone, and a 17-point non-linear lookup table to linearize the compressed voltage curve.
4. **Preserved Stock Debounce & Deadband:**
   - Retains Keychron's 3-pass hardware ADC debounce loop (`ANALOG_DEBOUCE_TIME`).
   - Retains the Schmitt-trigger deadband (`abs(last_val - value) < 3`) to reject spring bounce and sensor jitter.
5. **Keychron Launcher Global Profile Bugfix:**
   - Fixed an upstream bug in [`profile.c`](src/keyboards/keychron/common/analog_matrix/profile.c) where `profile_set_traval()` failed to update active keys in RAM when applying a global actuation value.
6. **Wireless Deep Sleep (`PM_STOP`) Hardware RTC Wake-Up & Fast Warm Resume:**
   - Configures the STM32F401 hardware RTC Periodic Wakeup Timer (`RTCD1` at ~90 ms intervals) in [`lpm_stm32f401.c`](src/keyboards/keychron/common/wireless/lpm_stm32f401.c) to briefly probe the Hall sensor matrix (`analog_matrix_lpm_any_key_pressed()`) during `PM_STOP`, bypassing the South-pole-only `C5` hardware comparator limitation.
   - Implements fast warm re-initialization in `analog_matrix_init()` on wake-up (preserving calibrated `zero_travel` baselines in RAM, skipping the 60 ms `CALIB_ZERO_TRAVEL_POWER_ON` window, preventing `Esc` wake-up from triggering USB DFU mode, and immediately waking the RGB backlight and `wireless_connect()`).

---

## Pre-compiled Binaries

Pre-compiled and EEPROM-protected binaries are available in [`binaries/`](binaries/) and on the [Releases](https://github.com/brkemretr/keychron-q1-he-ks20-firmware/releases) page:

| Model | Layout | Binary File |
| :--- | :--- | :--- |
| **Keychron Q1 HE ISO Knob** | ISO (UK / Nordic / German / etc.) | [`binaries/keychron_q1_he_iso_encoder_via_ks20_patched.bin`](binaries/keychron_q1_he_iso_encoder_via_ks20_patched.bin) |
| **Keychron Q1 HE ANSI Knob** | ANSI (US Layout) | [`binaries/keychron_q1_he_ansi_encoder_via_ks20_patched.bin`](binaries/keychron_q1_he_ansi_encoder_via_ks20_patched.bin) |

> **EEPROM Protection:** These binaries include 0xFF padding on Sector 1 (`0x4000..0x8000`), instructing flashing tools (Keychron Launcher / QMK Toolbox) not to overwrite the STM32F401 emulated EEPROM sector, preserving VIA dynamic layouts and calibration data.

---

## How to Flash

### Method 1: Keychron Launcher Web App (Recommended)
1. Open [Keychron Launcher](https://launcher.keychron.com) in Chrome or Edge.
2. Enter DFU Bootloader mode:
   - Unplug the keyboard.
   - Hold down the **Esc** key.
   - Plug the USB cable back in, then release **Esc**.
3. In Keychron Launcher, open the **Firmware Update** tab.
4. Click **Select local file** and select the `.bin` file matching your layout.
5. Click **Start Update** and wait for the flash process to finish (~15 seconds).
6. Disconnect and reconnect the USB cable once.

### Method 2: QMK Toolbox
1. Open [QMK Toolbox](https://github.com/qmk/qmk_toolbox/releases).
2. Open the downloaded `.bin` file.
3. Put the board into DFU mode (hold Esc while plugging in).
4. Click **Flash**.

---

## Diagnostic Tool

A real-time telemetry inspection tool is included in [`tools/monitor_keys.py`](tools/monitor_keys.py). It reads raw ADC counts, calibrated baseline zero, travel distance, and key state over Raw HID (protocol 0xA9 / command 0x30):

```bash
python tools/monitor_keys.py
```

---

## License

This project is licensed under the **GNU General Public License v2.0 (GPL-2.0)** to remain fully compliant with QMK Firmware. See the [LICENSE](LICENSE) file for details.
