# Keychron Q1 HE — Gateron KS-20 / Reverse-Polarity Firmware

[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue.svg)](LICENSE)
[![QMK Firmware](https://img.shields.io/badge/QMK-Compatible-brightgreen.svg)](https://qmk.fm)
[![Keychron Launcher](https://img.shields.io/badge/Keychron%20Launcher-Supported-blue.svg)](https://launcher.keychron.com)
[![VIA Compatible](https://img.shields.io/badge/VIA-Compatible-yellow.svg)](https://usevia.app)

QMK firmware patches and pre-compiled binaries for the **Keychron Q1 HE (ISO Knob & ANSI Knob)** providing native support for **Gateron KS-20 magnetic switches** (such as **Magnetic Jade**, **Jade Pro**, **Orchid**, **White**, and other reverse-polarity Hall Effect switches).

All modified source code is included in [`src/`](src/) under the GNU General Public License v2.0.

---

## Technical Background: Stock Firmware vs. KS-20

The Keychron Q1 HE comes standard with **Gateron Jupiter** magnetic switches. Swapping them for popular reverse-polarity switches like **Gateron Magnetic Jade or KS-20 Orchid** breaks stock firmware due to several hardware and firmware factors:

1. **Reversed Magnetic Polarity:**
   - **Jupiter switches:** As the stem is pressed down, the ADC reading **decreases** (from ~3120 down to ~1940).
   - **KS-20 switches:** As the stem is pressed down, the ADC reading **increases** (from ~3700–4010 up to ~4094).
   - Stock polynomial lookup tables and calibration logic assume decreasing voltage and interpret KS-20 switches as stuck or invalid.
2. **Esc Key Bootmagic Lock (DFU Mode):**
   - On plug-in, the stock bootloader checks whether Esc is pressed by looking for an ADC drop. With KS-20, its high resting ADC (~3950) causes the keyboard to detect a false Esc press, trapping the board in STM32 DFU bootloader mode on every startup.
3. **Resting Thermal Drift & High-Baseline Keys (F-Row):**
   - Hall sensor readings drift by +15–30 ADC counts as the PCB warms up from MCU and RGB LED operation.
   - On high-baseline rows (the F1–F12 function row rests at ADC ~3980–4010), unhandled resting drift exceeds shallow actuation thresholds (e.g. 0.2mm), leading to ghost-pressing when untouched.

---

## Physical Hardware Limitations (KS-20 on Q1 HE PCB)

Users installing KS-20 switches on a Q1 HE should be aware of two physical constraints:

### 1. Mechanical Travel Difference: 3.5mm vs. 4.0mm
- **Stock Gateron Jupiter:** Has **4.0mm** of total mechanical travel. Keychron Launcher's slider (0.2mm – 3.8mm) was built around this 4.0mm baseline.
- **Gateron KS-20 Switches:** Have a factory total mechanical travel of **3.5 ± 0.1 mm**. The switch physically stops at 3.5mm and mechanically cannot reach 3.8mm or 4.0mm.

### 2. Op-Amp Rail Saturation (~2.6mm – 2.8mm)
- **High-Flux N52 Magnet:** KS-20 stems contain powerful neodymium magnets (up to 700 Gauss at bottom-out).
- **PCB Op-Amp Rail Clipping:** The Q1 HE's analog amplifier circuit was engineered for Jupiter's reverse-polarity weaker flux. When a KS-20 magnet approaches the sensor, the amplifier output reaches its positive power supply rail (**3.30V = ADC 4094**) around **~2.6mm – 2.8mm** of physical stroke.
- **Saturation Zone (2.8mm → 3.5mm):** From ~2.8mm down to the mechanical bottom-out at 3.5mm (the final ~0.7mm), the ADC reading is flatlined at `4094` (zero voltage change). No firmware can detect physical stem movement in this final 0.7mm because the hardware amplifier is saturated.
- **Why 3.8mm Triggers Early:** When setting 3.8mm in Keychron Launcher, the firmware expects an ADC reading of ~4088. This reading is reached at ~2.5mm – 2.7mm of physical stroke. Consequently, the switch triggers while you still feel ~0.8mm – 1.0mm of mechanical stem travel left before bottoming out.

### Recommended Actuation Settings in Keychron Launcher

| Profile | Recommended Setting | Behavior |
| :--- | :--- | :--- |
| **Competitive / Esports** | `0.3 mm – 0.8 mm` | Ultra-fast response with zero misclicks. Stable with resting drift tracking. |
| **Balanced / Daily Typing** | `1.2 mm – 1.8 mm` | Standard mechanical feel, eliminates accidental actuation when resting fingers. |
| **Deep Actuation** | `2.0 mm – 2.5 mm` | Triggers near the end of the measurable linear range. |
| **Not Recommended** | `> 2.6 mm` (e.g. `3.0 mm – 3.8 mm`) | Falls into the op-amp saturation zone; triggers early with ~0.8mm travel remaining. |
| **Rapid Trigger** | `0.1 mm – 0.4 mm` | Optimal performance within the linear `0.2 mm – 2.5 mm` stroke window. |

---

## Engineering Implementation & Source Code

All modified source files are located in [`src/`](src/):

- [`src/keyboards/keychron/common/analog_matrix/analog_matrix.c`](src/keyboards/keychron/common/analog_matrix/analog_matrix.c)
- [`src/keyboards/keychron/common/analog_matrix/analog_matrix.h`](src/keyboards/keychron/common/analog_matrix/analog_matrix.h)
- [`src/keyboards/keychron/common/analog_matrix/analog_matrix_scan.c`](src/keyboards/keychron/common/analog_matrix/analog_matrix_scan.c)
- [`src/keyboards/keychron/common/analog_matrix/action_rapid_trigger.c`](src/keyboards/keychron/common/analog_matrix/action_rapid_trigger.c)
- [`src/keyboards/keychron/common/analog_matrix/profile.c`](src/keyboards/keychron/common/analog_matrix/profile.c)
- [`src/keyboards/keychron/q1_he/config.h`](src/keyboards/keychron/q1_he/config.h)

### Key Architectural Fixes:

1. **Released-Only Upward Resting Drift Tracking:**
   - In [`analog_matrix.c`](src/keyboards/keychron/common/analog_matrix/analog_matrix.c), `track_resting_baseline()` adapts `zero_travel` to slow upward thermal drift (+25..30 counts) only when a key is in `AKS_REGULAR_RELEASED` state, below its actuation point, and motionless within ±2 ADC counts for 1000 consecutive scans (1.0 second).
   - Never touches `zero_travel` while a key is `AKS_REGULAR_PRESSED`. Holding or feathering a key mid-stroke is never re-zeroed or force-released.
2. **Keychron Auto-Calibration State Machine:**
   - Re-enabled and adapted Keychron's native 4-state auto-calibration state machine (`AUTO_CALIB_FULL_TRAVEL` -> `AUTO_CALIB_ZERO_TRAVEL`) for KS-20 reverse polarity.
   - Baseline calibration updates only after a verified full bottom-out (`>= 4080`) followed by a fast snap return to the top stop within `< 200ms` across multiple cycles (`confidence >= 12`).
3. **17-Point Saturation Compensation LUT (`ks20_travel_lut`):**
   - Implements an 8% proportional resting deadzone (8..22 ADC counts), a 1-count top margin, and a 17-point piecewise-linear LUT linearizing the ADC response across the KS-20 op-amp curve.
4. **Preserved Stock Keychron Debounce & Hysteresis Architecture:**
   - Stock 3-pass hardware ADC debounce (`ANALOG_DEBOUCE_TIME` in [`analog_matrix_scan.c`](src/keyboards/keychron/common/analog_matrix/analog_matrix_scan.c)) with unpopulated matrix pin masking.
   - Schmitt-trigger raw ADC deadband (`abs(last_val - value) < 3` in `update_raw_value()`).
   - Stock hysteresis curve (0.2–0.3mm: release at deadzone; 0.4–1.0mm: 0.30mm; >1.0mm: 0.40mm) and Rapid Trigger `BOTTOM_DEAD_ZONE` protection.
5. **Keychron Launcher Global Actuation Fix:**
   - In Keychron's stock [`profile.c`](src/keyboards/keychron/common/analog_matrix/profile.c), `profile_set_traval()` previously failed to call `update_key_config()` when `global == true`. This bug left all 82 keys in RAM at default 2.0mm when setting global actuation. Fixed to update all keys immediately in RAM.

---

## Pre-compiled Binaries

Pre-compiled and EEPROM-protected binaries are available in [`binaries/`](binaries/) and on the [Releases](https://github.com/brkemretr/keychron-q1-he-ks20-firmware/releases) page:

| Model | Layout | Binary File |
| :--- | :--- | :--- |
| **Keychron Q1 HE ISO Knob** | ISO (UK / Nordic / German / etc.) | [`binaries/keychron_q1_he_iso_encoder_via_ks20_patched.bin`](binaries/keychron_q1_he_iso_encoder_via_ks20_patched.bin) |
| **Keychron Q1 HE ANSI Knob** | ANSI (US Layout) | [`binaries/keychron_q1_he_ansi_encoder_via_ks20_patched.bin`](binaries/keychron_q1_he_ansi_encoder_via_ks20_patched.bin) |

> **Note on EEPROM Padding:** These binaries include 0xFF padding on Sector 1 (`0x4000..0x8000`), preventing flashing tools from wiping the STM32F401 emulated EEPROM sector.

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

## Building from Source

To compile from source:

1. Clone Keychron's QMK firmware repository:
   ```bash
   git clone https://github.com/Keychron/qmk_firmware.git -b hall_effect_playground
   cd qmk_firmware
   ```
2. Apply the unified patch:
   ```bash
   git apply /path/to/patches/keychron_q1_he_ks20_support.patch
   ```
3. Compile:
   ```bash
   # For ISO Knob:
   make keychron/q1_he/iso_encoder:via

   # For ANSI Knob:
   make keychron/q1_he/ansi_encoder:via
   ```
4. Apply the EEPROM protection padding:
   ```bash
   python tools/apply_eeprom_patch.py keychron_q1_he_iso_encoder_via.bin keychron_q1_he_iso_encoder_via_ks20_patched.bin
   ```

---

## License

This project is licensed under the **GNU General Public License v2.0 (GPL-2.0)** to remain fully compliant with QMK Firmware. See the [LICENSE](LICENSE) file for details.
