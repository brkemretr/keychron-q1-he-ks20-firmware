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

### 1. 3-Tier Bidirectional Top-Stop Baseline Tracker (`0.2mm` Thermal & Stem-Seating Drift Recovery)
- On the Q1 HE PCB with reverse-polarity KS-20 switches, PCB warm-up (from RGB LEDs/MCU) and mechanical stem seating after a keystroke can shift a switch's physical top-stop resting ADC either downward **or** upward by `20..65` counts while remaining inside the physical resting band (`< 4030`).
- Implements a **3-tier stillness-verified top-stop tracker**:
  - **Tier 1 (Inside Deadzone):** Adapts `zero_travel` after `250ms` of `±1` ADC stillness while released inside the deadzone.
  - **Tier 2 (Warm-Up / Stem-Seating Recovery):** When a released key drifts slightly above the deadzone (`raw_delta <= 80`, `ADC < 4030`), re-zeros `zero_travel` after `600ms` of rock-solid `±1` count top-stop stillness.
  - **Tier 3 (`0.2mm` Stuck-At-Top-Stop Guard):** If a key at ultra-shallow actuation (`0.1mm–0.3mm`) gets trapped by upward resting drift at the physical top-stop (`ADC < 4025`) with motionless `±1` count stability for `1.5s` (impossible for a human finger holding a key mid-stroke), it automatically re-zeros and releases the key.

### 2. 17-Point Op-Amp Rail-Saturation Compensation LUT (`0.2mm – 3.8mm` True Linear Travel)
- Because reverse-polarity KS-20 switches push the Q1 HE's Hall sensor op-amp toward its `3.30V` (`4094`) supply rail, the first `1.0mm` of physical travel spans `~50%` of the ADC range while the final millimeter (`3.0mm → 4.0mm`) compresses into the top `7%` (`4075..4094`).
- Implements an `8%` proportional resting deadzone (`8..22` ADC counts), a `1`-count top margin (preserving `4080..4093` for deep bottom-out), and a **17-point piecewise-linear saturation-compensation LUT (`ks20_travel_lut`)**:
  - `0.2mm – 1.0mm` maps to `15% – 48%` of active ADC swing (`~36..115` counts above `zero`), providing rock-solid shallow stability.
  - `3.8mm` maps to `98%` of active ADC swing (`4089..4093`), ensuring `3.8mm` only triggers at true physical bottom-out.

### 3. Global Actuation Profile Fix (`profile.c`) & Full `0.2mm – 3.8mm` Support
- Fixes a bug in Keychron's [`profile_set_traval()`](./patches/keychron_q1_he_ks20_support.patch) where changing Global Actuation in Keychron Launcher updated the profile struct but never called `update_key_config(r, c)` or cleared per-key overrides in RAM.
- Removes the `3.5mm` hard clamp in `update_key_config()`, supporting the full `0.1mm – 3.8mm` range in Keychron Launcher and immediately re-evaluating key state when actuation distance is increased.

### 4. Unpopulated Matrix Pin Masking & Native 3-Sample Hardware ADC Debounce
- Masks unpopulated/floating matrix coordinates (`(5,3)`, `(5,7)`, `(4,11)`, `(0,14)`, `(3,14)`) via `analog_matrix_mask` in both `matrix_read_rows_on_col()` and `update_raw_value()` so floating pins never disturb column debounce.
- Preserves Keychron's native 3-pass `ANALOG_DEBOUCE_TIME` hardware ADC verification and `sym_eager_pk` 5ms post-transition lockout without rolling back the EMA filter of neighboring keys on the same column.

### 5. Keychron-Engineered Hysteresis & Rapid Trigger Bottom Deadzone
- **Regular Trigger Hysteresis (`update_key_config`):**
  - `0.2mm – 0.3mm` actuation: `deactn_pt = 6` (`0.10mm` release threshold above deadzone).
  - `0.4mm – 1.0mm` actuation: `0.25mm` (`15` units) hysteresis.
  - `> 1.0mm` actuation: `0.35mm` (`21` units) hysteresis.
- **Rapid Trigger Rail-Saturation Protection:** Enforces Keychron's `BOTTOM_DEAD_ZONE` (`3.8mm`) ceiling guard and a minimum `0.30mm` window in the deep compressed zone (`> 2.0mm`), preventing op-amp rail saturation from causing phantom rapid releases/re-triggers.

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
