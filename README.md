# Keychron Q1 HE — Gateron KS-20 / Reverse-Polarity Magnetic Switch Firmware

[![QMK Firmware](https://img.shields.io/badge/QMK-Compatible-brightgreen.svg)](https://qmk.fm)
[![Keychron Launcher](https://img.shields.io/badge/Keychron%20Launcher-Supported-blue.svg)](https://launcher.keychron.com)
[![VIA Compatible](https://img.shields.io/badge/VIA-Compatible-yellow.svg)](https://usevia.app)
[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-orange.svg)](https://www.gnu.org/licenses/gpl-2.0.html)

This repository provides enhanced, esports-grade QMK firmware for the **Keychron Q1 HE (ISO Knob & ANSI Knob)** with native support for **Gateron KS-20 magnetic switches** (such as **Gateron Orchid**, **Magnetic Jade**, **Jade Pro**, **White**, and other reverse-polarity Hall Effect switches).

---

## 📖 The Story: Why This Project Exists

The **Keychron Q1 HE** is a magnificent piece of keyboard hardware — full CNC machined aluminum chassis, gasket mount, and premium build quality. However, the stock Gateron Jupiter switches left a lot to be desired in terms of acoustics, bottom-out sound, and the ultra-smooth feel offered by the latest wave of magnetic switches (like the Gateron Magnetic Jade and KS-20 Orchid).

Like many others, I assumed the Q1 HE was fundamentally incompatible with newer magnetic switches due to hardware or firmware barriers. In fact, I even went ahead and **bought an entirely new keyboard** just to enjoy the acoustic and performance benefits of modern HE switches!

Yet, I couldn't bring myself to give up on the Q1 HE. Its build quality, weight, and acoustic potential were simply too good to sit unused on a shelf. Driven by pure curiosity and the question: *"Is it truly impossible, or can we actually reverse-engineer the firmware to make this beast compatible with KS-20 switches?"*, I set out on this journey. The result is a fully functional, competitive, and native KS-20 experience on the Q1 HE.

---

## ⚡ The Problem: Why Stock Firmware Breaks with KS-20 Switches

The Keychron Q1 HE comes standard with **Gateron Jupiter** magnetic switches. If you swap them out for popular switches like **Gateron Magnetic Jade or KS-20 Orchid**, several severe issues emerge:

1. **Reversed Magnetic Polarity:**
   - **Jupiter switches:** As you press down, the ADC reading **decreases** (from ~3000 down to ~1200).
   - **KS-20 switches:** As you press down, the ADC reading **increases** (from ~3700–3950 up to ~4094).
   - The stock polynomial LUT and calibration interpret the switch as stuck or broken.
2. **Esc Key Bootmagic Lock (DFU Mode):**
   - On plug-in, the stock bootloader expects Esc's ADC to drop when pressed. With KS-20, its high resting ADC (~3950) causes the keyboard to think Esc is held down, trapping the board in DFU bootloader mode indefinitely.
3. **Ghost Pressing & Stuck Keys below 1.5mm Actuation:**
   - Analog Hall sensors experience natural thermal drift (±10–25 ADC counts throughout the day).
   - When actuation distance is set to 0.2mm–0.7mm, any small resting drift causes the key to actuate by itself at rest and lock into a permanent `PRESSED` state.
4. **Machine-Gun Rapid Trigger Stuttering:**
   - When spamming keys rapidly in gaming (e.g., strafing in CS2/Valorant), releasing the switch snaps the stem upward against the upper housing stop.
   - The mechanical rebound bounce (approx 0.08–0.15mm) falsely triggers Rapid Trigger near the top stop, causing repeated phantom inputs.

---

## 🚀 Key Improvements & Engineering Fixes

### 1. One-Way Release-Direction Baseline Recovery + Keychron `auto_caliration_check` State Machine
- **Zero Mid-Stroke Interference (`track_resting_baseline`):** Strictly adapts `zero_travel` **downward only** (`value < zero_travel`) after `500ms` of stability **only while the key is in `AKS_REGULAR_RELEASED` state**. Never shifts `zero_travel` upward on a timer and never force-releases a held or feathered key.
- **Keychron Dynamic Release-Velocity Auto-Calibration (`auto_caliration_check`):** Adapts Keychron's native 4-state auto-calibration state machine (`AUTO_CALIB_FULL_TRAVEL` -> `AUTO_CALIB_ZERO_TRAVEL`) for reverse-polarity KS-20 switches. Only updates resting baseline when a key is pressed to true bottom-out (`>= 4080`) and snaps rapidly back to the physical top-stop (`< 4025`, full stroke `>= 85` counts) within `< 200ms` (`AUTO_CALIB_VALID_RELASING_TIME`) across multiple verified cycles (`confidence >= 12`).

### 2. 17-Point Op-Amp Rail-Saturation Compensation Curve (`0.2mm – 3.8mm` True Linear Travel)
- Because reverse-polarity KS-20 switches push the Q1 HE's Hall sensor op-amp toward its `3.30V` (`4094`) supply rail, the first `1.0mm` of physical travel spans `~50%` of the ADC range while the final millimeter (`3.0mm → 4.0mm`) compresses into the top `7%` (`4075..4094`).
- Implements a proportional resting deadzone (`8%` of active swing, clamped to `8..22` ADC counts), a `1`-count top margin (preserving `4080..4093` for deep bottom-out), and a **17-point convex saturation-compensation LUT (`ks20_travel_lut`)**:
  - `0.2mm – 1.0mm` maps to `15% – 48%` of active ADC swing (`~36..115` counts above `zero`), expanding shallow ADC resolution by **2.5x–3x** to match Keychron's stock 4th-degree polynomial LUT behavior.
  - `3.8mm` maps to `98%` of active ADC swing (`4089..4093`), ensuring `3.8mm` only triggers at true physical bottom-out.

### 3. Restored Keychron 3-Layer Hardware & Software Debounce Architecture
- **Native 3-Sample Hardware ADC Debounce (`ANALOG_DEBOUCE_TIME`):** Every state transition (`RELEASED <-> PRESSED`) is verified across 3 consecutive hardware `adcConvert` reads in `analog_matrix_scan.c`, identical to Keychron's stock implementation, while masking unpopulated pins via `analog_matrix_mask`.
- **Schmitt-Trigger Raw ADC Deadband (`update_raw_value`):** Preserves Keychron's raw ADC deadband (`abs(k->last_val - smooth_val) < 3`) across active travel so 1–2 count sensor fluctuations never jitter `travel` while holding a key mid-stroke.
- **QMK `sym_eager_pk` Per-Key Lockout (`info.json`):** Enables 5ms post-transition bounce lockout with **0ms eager press/release latency**.

### 4. Keychron-Engineered Hysteresis & Rapid Trigger Bottom Deadzone
- **Regular Trigger Hysteresis (`update_key_config`):**
  - `0.2mm – 0.3mm` actuation: `deactn_pt = 0` (must return to top deadzone to release, preventing micro-chatter).
  - `0.4mm – 1.0mm` actuation: `0.30mm` (`18` units) hysteresis.
  - `> 1.0mm` actuation: `0.40mm` (`24` units) hysteresis.
- **Rapid Trigger Rail-Saturation Protection (`action_rapid_trigger.c`):** Preserves Keychron's `BOTTOM_DEAD_ZONE` (`3.8mm`) ceiling guard and enforces a minimum `0.30mm` window in the deep compressed zone (`> 2.0mm`), preventing op-amp rail saturation from causing phantom rapid releases/re-triggers.
- **Global Actuation Profile Sync (`profile.c`):** Fixes Keychron's `profile_set_traval()` so Global Actuation updates in Keychron Launcher immediately update all 82 keys in RAM across the full `0.2mm – 3.8mm` range.

### 6. 4-Tap Exponential Moving Average (EMA) Noise Filter
- 4-tap integer EMA filter (`rev_ema = (rev_ema * 3 + (val << 2)) >> 2`).
- Drastically suppresses ADC power rail and analog multiplexer white noise from ±5 counts down to < 0.5 counts.
- Realized in pure integer arithmetic with **< 1 ms latency** at 1000 Hz matrix scan rate.

### 7. Forced USB Full N-Key Rollover (`FORCE_NKRO`) & Seamless Launcher/VIA Integration
- Forces true full N-Key Rollover on USB, eliminating multi-key blocking or Ghosting.
- Full compatibility with the official [Keychron Launcher](https://launcher.keychron.com) web app and [VIA](https://usevia.app) across the entire `0.2mm – 3.8mm` actuation range.

---

## 📥 Pre-compiled Binaries (Ready to Flash)

Pre-compiled and EEPROM-patched binaries are provided in the [`binaries/`](./binaries/) directory:

| Keyboard Model | Layout | Binary File |
| :--- | :--- | :--- |
| **Keychron Q1 HE ISO Knob** | ISO (UK / Nordic / German / etc.) | [`binaries/keychron_q1_he_iso_encoder_via_ks20_patched.bin`](./binaries/keychron_q1_he_iso_encoder_via_ks20_patched.bin) |
| **Keychron Q1 HE ANSI Knob** | ANSI (US Layout) | [`binaries/keychron_q1_he_ansi_encoder_via_ks20_patched.bin`](./binaries/keychron_q1_he_ansi_encoder_via_ks20_patched.bin) |

> [!IMPORTANT]
> These binaries include the **0x4000–0x8000 Sector 1 padding patch**. This is crucial for STM32F401 boards so that flashing does not wipe or corrupt the emulated EEPROM sector.

---

## 🛠️ How to Flash

### Method 1: Via Keychron Launcher Web App (Recommended)
1. Open Google Chrome or any Chromium-based browser and navigate to **[Keychron Launcher](https://launcher.keychron.com)**.
2. Put your keyboard into DFU Bootloader Mode:
   - Unplug the USB cable.
   - Hold down the **Esc** key.
   - Plug the USB cable back in, then release **Esc**.
3. In Keychron Launcher, go to the **Firmware Update** tab.
4. Click **Select local file** and choose the corresponding `.bin` file from the [`binaries/`](./binaries/) folder.
5. Click **Start Update** and wait for the flash process to finish (approx 10–20 seconds).
6. Once rebooted, disconnect and reconnect the keyboard once. All keys will auto-zero within 200ms!

### Method 2: Via QMK Toolbox
1. Download and launch **[QMK Toolbox](https://github.com/qmk/qmk_toolbox/releases)**.
2. Open the downloaded `.bin` file.
3. Put the board into DFU mode (hold Esc while plugging in).
4. Click **Flash**.

---

## 🧪 Real-Time Telemetry Diagnostic Tool

If you want to view the live ADC readings, baseline zero, travel distance in millimeters, and key state for your switches, a Python script is included in `tools/`:

```bash
python tools/monitor_keys.py
```

Sample output:
```text
Key     | ADC Val  | Zero     | Delta    | Travel       | State    
-----------------------------------------------------------------
ESC     | 3933     | 3933     | 0        | 0.00 mm ( 0) | RELEASED 
1       | 3949     | 3949     | 0        | 0.00 mm ( 0) | RELEASED 
Q       | 3904     | 3904     | 0        | 0.00 mm ( 0) | RELEASED 
W       | 3861     | 3861     | 0        | 0.00 mm ( 0) | RELEASED 
SPACE   | 3758     | 3758     | 0        | 0.00 mm ( 0) | RELEASED 
```

---

## 🔧 Building from Source

To compile the firmware yourself:

1. Clone Keychron's QMK firmware repository:
   ```bash
   git clone https://github.com/Keychron/qmk_firmware.git -b hall_effect_playground
   cd qmk_firmware
   ```
2. Apply the unified patch:
   ```bash
   git apply /path/to/patches/keychron_q1_he_ks20_support.patch
   ```
3. Compile the target layout:
   ```bash
   # For ISO Knob:
   make keychron/q1_he/iso_encoder:via

   # For ANSI Knob:
   make keychron/q1_he/ansi_encoder:via
   ```
4. Apply the EEPROM padding patch using our bundled script:
   ```bash
   python tools/apply_eeprom_patch.py keychron_q1_he_iso_encoder_via.bin keychron_q1_he_iso_encoder_via_ks20_patched.bin
   ```

---

## 📜 License & Acknowledgements

- Based on [Keychron QMK Firmware](https://github.com/Keychron/qmk_firmware).
- Licensed under the **GNU General Public License v2.0 (GPL-2.0)**.
- Special thanks to the open-source keyboard community and magnetic switch enthusiasts worldwide.
