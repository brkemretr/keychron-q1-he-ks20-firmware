/* Copyright 2024 @ Keychron (https://www.keychron.com)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "quantum.h"
#include "analog_matrix.h"
#include "keymap_introspection.h"
#include "raw_hid.h"
#include "eeprom.h"
#include "eeprom_he.h"
#include "usb_main.h"
#include <stdio.h>
#include "profile.h"
#include "sqrt.h"
#include "game_controller_common.h"

#ifndef CAL_SAMPL_CNT
#    define CAL_SAMPL_CNT 8
#endif

#ifndef REF_ZERO_TRAVEL
#    define REF_ZERO_TRAVEL 3121
#endif
#ifndef REF_FULL_TRAVEL
#    define REF_FULL_TRAVEL 1940
#endif
#define REF_RANGE (REF_ZERO_TRAVEL - REF_FULL_TRAVEL)

#ifndef CONST_A1
#    define CONST_A1 426.88962
#endif
#ifndef CONST_B1
#    define CONST_B1 -0.48358
#endif
#ifndef CONST_C1
#    define CONST_C1 2.04637e-4
#endif
#ifndef CONST_D1
#    define CONST_D1 -2.99368e-8
#endif

#define TRAVEL_POLYNOMIAL(x) (CONST_A1 + CONST_B1 * x + CONST_C1 * x * x + CONST_D1 * x * x * x)

enum {
    CALIB_OFF = 0,
    CALIB_ZERO_TRAVEL_POWER_ON,
    CALIB_ZERO_TRAVEL_MANUAL,
    CALIB_FULL_TRAVEL_MANUAL,
    CALIB_SAVE_AND_EXIT,
    CALIB_CLEAR,
    CALIB_MAX,
};

enum {
    CALI_ZERO_TRAVEL = 0x01 << 0,
    CALI_FULL_TRAVEL = 0x01 << 1,
};

enum {
    AUTO_CALIB_OFF = 0,
    AUTO_CALIB_NEXT_LOOP,
    AUTO_CALIB_ZERO_TRAVEL,
    AUTO_CALIB_FULL_TRAVEL,
    AUTO_CALIB_FINISHED,
};

enum {
    AMC_GET_VERSION = 0x01,

    AMC_GET_PROFILES_INFO = 0x10,
    AMC_SELECT_PROFILE,
    AMC_GET_PROFILE_RAW,
    AMC_SET_PROFILE_NAME,
    AMC_SET_TRAVAL,
    AMC_SET_ADVANCE_MODE,
    AMC_SET_SOCD,
    AMC_RESET_PROFILE = 0x1E,
    AMC_SAVE_PROFILE  = 0x1F,
    AMC_GET_CURVE = 0x20,
    AMC_SET_CURVE,
    AMC_GET_GAME_CONTROLLER_MODE,
    AMC_SET_GAME_CONTROLLER_MODE,

    AMC_GET_REALTIME_TRAVEL = 0x30,

    AMC_CALIBRATE = 0x40,
    AMC_GET_CALIBRATE_STATE,
    AMC_GET_CALIBRATED_VALUE,
};

extern const matrix_row_t analog_matrix_mask[];
extern const matrix_row_t okmc_matrix[MATRIX_ROWS];
extern matrix_row_t virtual_matrix[MATRIX_ROWS];

extern bool regular_trigger_action(analog_key_t *key);
extern bool okmc_action(analog_key_t *key);
extern bool rapid_trigger_action(analog_key_t *key);
extern bool toggle_action(analog_key_t *key);
extern bool xinput_update(analog_key_t *key);
extern bool joystick_update(analog_key_t *key);
extern void socd_action(void);

static calibrated_value_t calib_values[MATRIX_ROWS][MATRIX_COLS];
static calibrated_value_t saved_calib_values[MATRIX_ROWS][MATRIX_COLS];
static analog_key_t       analog_key_matrix[MATRIX_ROWS][MATRIX_COLS];
static uint16_t           rev_ema[MATRIX_ROWS][MATRIX_COLS];
static uint16_t           rev_rest_val[MATRIX_ROWS][MATRIX_COLS];
static uint16_t           rev_rest_cnt[MATRIX_ROWS][MATRIX_COLS];
static analog_key_t       col_saved_key[MATRIX_ROWS];
static uint16_t           col_saved_ema[MATRIX_ROWS];

static uint16_t      calibrate_values[MATRIX_ROWS][MATRIX_COLS][CAL_SAMPL_CNT];
static calibration_t auto_calib[MATRIX_ROWS][MATRIX_COLS];
static uint8_t       cali_state = CALIB_OFF;
static uint8_t       last_cali_state;
static uint8_t       cur_calib = 0;
traval_config_t      regular;
static float         scale_factor[MATRIX_ROWS][MATRIX_COLS];
static matrix_row_t  calib_state_matrix[MATRIX_ROWS];
// Mark invalid key on abnormal value of manual zero travel calibration, for manufacturing use
static matrix_row_t  manual_calib_zero_invalid[MATRIX_ROWS];

uint8_t calibrated, rapid_actuation, rapid_sensitivity;

static uint32_t calib_ind_timer = 0;
static uint8_t  last_calib_row  = 0xFF;
static uint8_t  last_calib_col  = 0xFF;

uint32_t debug_interval = 0;

uint8_t  analog_matrix_get_travel(uint8_t row, uint8_t col) {
    return analog_key_matrix[row][col].travel;
}

// 17-point saturation-compensation LUT mapping normalized ADC (0..1024, in 16 steps of 64)
// to travel units (0..240, where 6 units = 0.1mm, 240 units = 4.0mm).
// Compensates for the Q1 HE Hall sensor op-amp compression near the 3.3V (4094) rail:
// - 0.0mm..1.0mm (0..60) spans the first ~48% of active ADC range (high resolution for 0.2mm-1.0mm)
// - 1.0mm..2.5mm (60..150) spans ~48%..82% of active ADC range
// - 2.5mm..3.8mm (150..228) spans ~82%..98.5% of active ADC range (true bottom-out required for 3.8mm)
static const uint8_t ks20_travel_lut[17] = {
      0,  10,  18,  26,  35,  44,  53,  63,
     74,  86,  99, 114, 131, 151, 176, 206, 240
};

static uint8_t convert_to_travel(uint8_t row, uint8_t col, uint16_t value) {
    if ((analog_matrix_mask[row] & (0x01 << col)) == 0) return 0;

    calibrated_value_t *p_calib = &calib_values[row][col];

    uint16_t zero = p_calib->zero_travel;
    uint16_t full = p_calib->full_travel;
    if (full <= zero || full < 4050) full = 4094;

    // Pure KS-20 Architecture: All switches rest around 3680-4020 and increase toward 4094 on press
    int32_t full_delta = (int32_t)full - (int32_t)zero;
    if (full_delta < 40) full_delta = DEFAULT_REV_FULL_RANGE;

    int32_t raw_delta = (int32_t)value - (int32_t)zero;

    // Keychron-style proportional resting deadzone (~8% of full range, clamped to 8..22 ADC counts):
    // Absorbs thermal drift, power rail noise, and natural finger resting weight
    int32_t dead_zone = (full_delta * 8) / 100;
    if (dead_zone < 8) dead_zone = 8;
    if (dead_zone > 22) dead_zone = 22;
    if (raw_delta <= dead_zone) return 0;

    // Preserve full upper ADC resolution up to full - 1 (4093..4094) so 3.8mm requires true bottom-out
    int32_t eff_range = full_delta - dead_zone - 1;
    if (eff_range < 20) eff_range = 20;

    uint32_t norm = ((uint32_t)(raw_delta - dead_zone) << 10) / (uint32_t)eff_range;
    if (norm >= 1024) return FULL_TRAVEL_UNIT * TRAVEL_SCALE;

    uint32_t idx  = norm >> 6;
    uint32_t frac = norm & 0x3F;
    uint32_t y0   = ks20_travel_lut[idx];
    uint32_t y1   = ks20_travel_lut[idx + 1];
    uint16_t travel = (uint16_t)(y0 + (((y1 - y0) * frac + 32) >> 6));
    if (travel > FULL_TRAVEL_UNIT * TRAVEL_SCALE) travel = FULL_TRAVEL_UNIT * TRAVEL_SCALE;

    return travel & 0xFF;
}

void update_key_config(uint8_t row, uint8_t col) {
    analog_key_t            *p_key;
    analog_key_config_t     *p_key_cfg;
    analog_matrix_profile_t *cur_prof = profile_get_current();
    extern matrix_row_t      analog_raw_matrix[MATRIX_ROWS];

    p_key     = &analog_key_matrix[row][col];
    p_key_cfg = &cur_prof->key_config[row][col];
    // For debug purpose
    p_key->r = row;
    p_key->c = col;

    // Update basic mode
    if (p_key_cfg->mode == AKM_GLOBAL)
        p_key->mode = cur_prof->global.mode;
    else
        p_key->mode = p_key_cfg->mode;

    //  Override mode if advance mode setting exists
    if (p_key_cfg->adv_mode != 0) {
        p_key->mode = p_key_cfg->adv_mode;
    }

    // Update actuaction point
    if (p_key_cfg->act_pt == 0)
        p_key->regular.actn_pt = cur_prof->global.act_pt;
    else
        p_key->regular.actn_pt = p_key_cfg->act_pt;

    // Update deactuaction point
    p_key->regular.deactn_pt = p_key->regular.actn_pt - 3 > 0 ? p_key->regular.actn_pt - 3 : 0;

    // Update rapid trigger sensitivity
    if (p_key_cfg->rpd_trig_sen == 0)
        p_key->rpd_trig_sen = cur_prof->global.rpd_trig_sen;
    else
        p_key->rpd_trig_sen = p_key_cfg->rpd_trig_sen;

    // Update rapid trigger release sensitivity
    if (p_key_cfg->rpd_trig_sen_deact == 0)
        p_key->rpd_trig_sen_rls = cur_prof->global.rpd_trig_sen_deact == 0 ? cur_prof->global.rpd_trig_sen : cur_prof->global.rpd_trig_sen_deact;
    else
        p_key->rpd_trig_sen_rls = p_key_cfg->rpd_trig_sen_deact;

    /* Scale by TRAVEL_SCALE */
    p_key->regular.actn_pt *= TRAVEL_SCALE;
    p_key->regular.deactn_pt *= TRAVEL_SCALE;
    p_key->rpd_trig_sen *= TRAVEL_SCALE;
    p_key->rpd_trig_sen_rls *= TRAVEL_SCALE;

    // Keychron original hysteresis & stability engineering for reverse-polarity (KS-20) switches
    if (calib_values[row][col].full_travel > calib_values[row][col].zero_travel ||
        calib_values[row][col].zero_travel >= REV_POLARITY_ZERO_THRESHOLD) {
        if (p_key->regular.actn_pt < 2 * TRAVEL_SCALE) {
            p_key->regular.actn_pt = 2 * TRAVEL_SCALE;
        }
        if (p_key->regular.actn_pt > 39 * TRAVEL_SCALE) {
            p_key->regular.actn_pt = 39 * TRAVEL_SCALE;
        }
        // Keychron hysteresis calculation:
        // - For ultra-short actuation (0.2mm - 0.3mm, <= 18 units): deactn_pt = 0 (releases cleanly at travel == 0)
        // - For 0.4mm - 1.0mm (24 - 60 units): deactn_pt = actn_pt - 18 (Keychron standard 0.30mm hysteresis)
        // - For > 1.0mm (> 60 units): deactn_pt = actn_pt - 24 (0.40mm hysteresis to absorb mid-stroke spring ringing)
        if (p_key->regular.actn_pt <= 3 * TRAVEL_SCALE) {
            p_key->regular.deactn_pt = 0;
        } else if (p_key->regular.actn_pt <= 10 * TRAVEL_SCALE) {
            p_key->regular.deactn_pt = p_key->regular.actn_pt - 3 * TRAVEL_SCALE;
        } else {
            p_key->regular.deactn_pt = p_key->regular.actn_pt - 4 * TRAVEL_SCALE;
        }
        if (p_key->rpd_trig_sen < 2 * TRAVEL_SCALE) {
            p_key->rpd_trig_sen = 2 * TRAVEL_SCALE;
        }
        if (p_key->rpd_trig_sen_rls < 2 * TRAVEL_SCALE) {
            p_key->rpd_trig_sen_rls = 2 * TRAVEL_SCALE;
        }
    }

    // Re-evaluate key state immediately when actuation point changes so a key never stays stuck
    if (p_key->value > 0) {
        p_key->travel = convert_to_travel(row, col, p_key->value);
    }
    if (p_key->travel == 0 || p_key->travel < p_key->regular.deactn_pt) {
        p_key->state = AKS_REGULAR_RELEASED;
        analog_raw_matrix[row] &= ~(0x01 << col);
    }

    // Update advance mode information
    if (p_key_cfg->adv_mode == AKM_DKS && p_key_cfg->okmc_idx < OKMC_COUNT) {
        p_key->okmc_idx = p_key_cfg->okmc_idx;
    } else if (p_key_cfg->adv_mode == AKM_GAMEPAD && p_key_cfg->js_axis < GC_BUTTON_MAX && p_key_cfg->js_axis != GC_MAX) {
        p_key->js_axis = p_key_cfg->js_axis;
    } else if (p_key_cfg->adv_mode == AKM_TOGGLE) {
        p_key->hold = 0;
    } else
        p_key_cfg->adv_mode = 0;
}

void update_travel_configs(void) {
    for (uint8_t r = 0; r < MATRIX_ROWS; r++) {
        for (uint8_t c = 0; c < MATRIX_COLS; c++) {
            update_key_config(r, c);
        }
    }
}

static void update_default_travel(void) {
    // clang-format off
    for (uint8_t r = 0; r < MATRIX_ROWS; r++) {
        for (uint8_t c = 0; c < MATRIX_COLS; c++) {
            if ((analog_matrix_mask[r] & (0x01<<c)) == 0) continue;

            if (saved_calib_values[r][c].zero_travel == 0)
                saved_calib_values[r][c].zero_travel = DEFAULT_ZERO_TRAVEL_VALUE;

            if (saved_calib_values[r][c].full_travel == 0) {
                if (saved_calib_values[r][c].zero_travel < REV_POLARITY_ZERO_THRESHOLD) {
                    saved_calib_values[r][c].full_travel = saved_calib_values[r][c].zero_travel - DEFAULT_FULL_RANGE;
                } else {
                    saved_calib_values[r][c].full_travel = 4094;
                }
            }
        }
    }
    // clang-format on
}

static inline void update_scale_factor(uint8_t row, uint8_t col) {
    calibrated_value_t *p_calib = &calib_values[row][col];

    if (calibrated | CALI_FULL_TRAVEL) {
        int32_t full_delta = abs((int32_t)p_calib->full_travel - (int32_t)p_calib->zero_travel);
        if (full_delta < 20) {
            scale_factor[row][col] = 1.0f;
            return;
        }
        int32_t x = (int32_t)REF_ZERO_TRAVEL - full_delta;
        if (x < POLYNOMIAL_MIN_X) x = POLYNOMIAL_MIN_X;
        if (x >= REF_ZERO_TRAVEL) {
            scale_factor[row][col] = 1.0f;
            return;
        }

        float full_travel      = (TRAVEL_POLYNOMIAL(x) - TRAVEL_POLYNOMIAL(REF_ZERO_TRAVEL));
        scale_factor[row][col] = (full_travel > 0.001f) ? (FULL_TRAVEL_UNIT / full_travel) : 1.0f;
    } else
        scale_factor[row][col] = 1.0f;
}

static void update_scale_factors(void) {
    for (uint8_t r = 0; r < MATRIX_ROWS; r++) {
        for (uint8_t c = 0; c < MATRIX_COLS; c++) {
            update_scale_factor(r, c);

        }
    }
}

static void save_calibration_value(uint8_t row, uint8_t col) {
    static uint8_t eeprom_calibrated;

    if (eeprom_calibrated != calibrated) {
        eeprom_calibrated = calibrated;
        he_eeprom_write_block(&calibrated, (void *)(EXTERNAL_EEPROM_OFFSET + OFFSET_CALIBRATION), 1);
    }

    uint32_t offset = (&saved_calib_values[row][col]-&saved_calib_values[0][0])* sizeof(saved_calib_values[0][0]);
    he_eeprom_write_block(&saved_calib_values[row][col], (void *)(EXTERNAL_EEPROM_OFFSET + CALIBRATED_CONFIG_START + offset), sizeof(saved_calib_values[0][0]));

    // Save a copy to emulated EEPROM
    if (!eeconfig_is_kb_datablock_valid()) eeprom_update_dword(EECONFIG_KEYBOARD, (EECONFIG_KB_DATA_VERSION));

    uint32_t addr = EECONFIG_BASE_SIZE + OFFSET_CALIBRATION;
    eeprom_update_byte((uint8_t *)addr, calibrated);

    addr = EECONFIG_BASE_SIZE + CALIBRATED_CONFIG_START + offset;
    eeprom_update_block(&saved_calib_values[row][col], (uint8_t *)addr, sizeof(saved_calib_values[0][0]));
}

static void save_calibration_values(void) {
    // Save to external EEPROM
    static uint8_t eeprom_calibrated;

    if (eeprom_calibrated != calibrated) {
        eeprom_calibrated = calibrated;
        he_eeprom_write_block(&calibrated, (void *)(EXTERNAL_EEPROM_OFFSET + OFFSET_CALIBRATION), 1);
    }

    he_eeprom_write_block(saved_calib_values, (void *)(EXTERNAL_EEPROM_OFFSET + CALIBRATED_CONFIG_START), sizeof(saved_calib_values));

    // Save a copy to emulate EEPROM
    if (!eeconfig_is_kb_datablock_valid()) eeprom_update_dword(EECONFIG_KEYBOARD, (EECONFIG_KB_DATA_VERSION));

    uint32_t addr = EECONFIG_BASE_SIZE + OFFSET_CALIBRATION;
    eeprom_update_byte((uint8_t *)addr, calibrated);

    addr = EECONFIG_BASE_SIZE + CALIBRATED_CONFIG_START;
    eeprom_update_block(saved_calib_values, (uint8_t *)addr, sizeof(saved_calib_values));

    update_default_travel();
    memcpy(calib_values, saved_calib_values, sizeof(saved_calib_values));
    update_travel_configs();
    update_scale_factors();
}

static bool calibrate(void) {
    static uint32_t calib_timer = 0;

    if (cali_state == CALIB_OFF) return false;

    bool update = false;

    if (cali_state == CALIB_FULL_TRAVEL_MANUAL) {
        bool done = true;

        for (uint8_t r = 0; r < MATRIX_ROWS; r++)
            for (uint8_t c = 0; c < MATRIX_COLS; c++) {
                if ((analog_matrix_mask[r] & (0x01<<c)) == 0) continue;

                uint32_t sum = 0;
                for (uint8_t i = 0; i < CAL_SAMPL_CNT; i++) {
                    if (calibrate_values[r][c][i] > VALID_ANALOG_RAW_VALUE_MIN)
                        sum += calibrate_values[r][c][i];
                    else {
                        sum = 0;
                        break;
                    }
                }

                if (sum && calib_values[r][c].zero_travel != 0) {
                    uint16_t avg  = sum / CAL_SAMPL_CNT;
                    uint16_t zero = calib_values[r][c].zero_travel;
                    uint16_t full = calib_values[r][c].full_travel;
                    int32_t  diff = (int32_t)avg - (int32_t)zero;
                    bool     updated_key = false;

                    if (diff < 0 && zero < REV_POLARITY_ZERO_THRESHOLD) {
                        // Normal polarity (e.g. KS-37B: zero ~3065, ADC decreases when pressed)
                        if ((full == 0 && (zero - avg) > MIN_FULL_TRAVEL_RANGE) ||
                            (full != 0 && full < zero && (avg + BOTTOM_JITTER) < full)) {
                            uint16_t adj_full = avg + BOTTOM_JITTER;
                            if (adj_full >= zero) adj_full = avg;
                            saved_calib_values[r][c].zero_travel = zero;
                            saved_calib_values[r][c].full_travel = calib_values[r][c].full_travel = adj_full;
                            updated_key = true;
                        }
                    } else if (diff > 0 && zero >= REV_POLARITY_ZERO_THRESHOLD) {
                        // Reverse polarity (e.g. KS-20: zero ~3830..4010, ADC increases toward 4094 when pressed)
                        // Require avg >= 4080 (or zero + 65 for high-zero switches) so keys only register full travel at true bottom-out
                        uint16_t min_bot = (zero > 4010) ? (zero + 65) : 4080;
                        if (min_bot > 4092) min_bot = 4092;
                        if (avg >= min_bot &&
                            ((full == 0 && (avg - zero) >= MIN_REV_FULL_TRAVEL_RANGE) ||
                             (full != 0 && full > zero && avg > full))) {
                            uint16_t adj_full = avg;
                            if (adj_full > 4094) adj_full = 4094;
                            saved_calib_values[r][c].zero_travel = zero;
                            saved_calib_values[r][c].full_travel = calib_values[r][c].full_travel = adj_full;
                            updated_key = true;
                        }
                    }

                    if (updated_key) {
                        calib_state_matrix[r] |= (0x01 << c);
                        calib_ind_timer = timer_read32();
                        last_calib_row  = r;
                        last_calib_col  = c;
                    }
                }

                // Check if calibration is finished
                if (calib_values[r][c].full_travel == 0) {
                    done = false;
                }
            }

        if (++cur_calib >= CAL_SAMPL_CNT) cur_calib = 0;

        // Set the timer to delay exit calibrating so that we can get enough samples of last key
        if (done && calib_timer == 0) calib_timer = timer_read32();

        if (calib_timer && timer_elapsed32(calib_timer) > 3000) {
            calib_timer = 0;
            update      = true;
            calibrated |= CALI_FULL_TRAVEL;
            profile_indication_enable();
        }
        if (!update) return false;
    } else if (cali_state == CALIB_ZERO_TRAVEL_POWER_ON || cali_state == CALIB_ZERO_TRAVEL_MANUAL) {
        if (cur_calib + 1 < CAL_SAMPL_CNT) {
            ++cur_calib;
            return false;
        }

        for (uint8_t r = 0; r < MATRIX_ROWS; r++)
            for (uint8_t c = 0; c < MATRIX_COLS; c++) {
                if ((analog_matrix_mask[r] & (0x01 << c)) == 0) continue;

                uint32_t sum = 0;
                for (uint8_t i = 0; i < CAL_SAMPL_CNT; i++)
                    sum += calibrate_values[r][c][i];

                uint16_t avg_val = sum / CAL_SAMPL_CNT;

                if (cali_state == CALIB_ZERO_TRAVEL_MANUAL) {
                    // Polarity-agnostic manual zero travel check: accept any valid 12-bit sensor reading
                    if (avg_val > VALID_ANALOG_RAW_VALUE_MAX || avg_val < VALID_ANALOG_RAW_VALUE_MIN) {
                        manual_calib_zero_invalid[r] |= 0x01 << c;
                    } else {
                        // Store raw resting value without directional deadzone shift;
                        // deadzone is enforced directionally in convert_to_travel()
                        calib_values[r][c].zero_travel = avg_val;
                    }
                } else {
                    // CALIB_ZERO_TRAVEL_POWER_ON: adopt current power-on resting ADC in RAM so cold/warm boot
                    // never false-triggers bootmagic DFU or ghost-presses, while preserving valid EEPROM calibration
                    bool key_invalid = (avg_val > VALID_ANALOG_RAW_VALUE_MAX || avg_val < VALID_ANALOG_RAW_VALUE_MIN);
                    if (!key_invalid) {
                        bool is_rev_key = (avg_val >= REV_POLARITY_ZERO_THRESHOLD);
                        if (is_rev_key) {
                            if (avg_val < 4050) {
                                calib_values[r][c].zero_travel = avg_val;
                            }
                            if (calib_values[r][c].full_travel <= calib_values[r][c].zero_travel ||
                                calib_values[r][c].full_travel < 4075) {
                                calib_values[r][c].full_travel = 4094;
                            }
                            update_scale_factor(r, c);
                            update_key_config(r, c);
                        } else {
                            if (abs((int32_t)avg_val - (int32_t)calib_values[r][c].zero_travel) > 15) {
                                calib_values[r][c].zero_travel = avg_val;
                                calib_values[r][c].full_travel = avg_val - DEFAULT_FULL_RANGE;
                                update_scale_factor(r, c);
                                update_key_config(r, c);
                            }
                        }
                    }
                }
            }

        if (cali_state == CALIB_ZERO_TRAVEL_MANUAL) {
            cur_calib  = 0;
            cali_state = CALIB_FULL_TRAVEL_MANUAL;
        } else {
            for (uint8_t r = 0; r < MATRIX_ROWS; r++) {
                for (uint8_t c = 0; c < MATRIX_COLS; c++) {
                    if ((analog_matrix_mask[r] & (0x01 << c)) == 0) continue;
                    if ((calibrated & CALI_ZERO_TRAVEL) == 0 ||
                        saved_calib_values[r][c].zero_travel < REV_POLARITY_ZERO_THRESHOLD ||
                        saved_calib_values[r][c].zero_travel > 4050) {
                        saved_calib_values[r][c].zero_travel = calib_values[r][c].zero_travel;
                    }
                }
            }
            calibrated |= CALI_ZERO_TRAVEL;
            cali_state = CALIB_OFF;
        }
    } else if (cali_state == CALIB_SAVE_AND_EXIT) {
        if (last_cali_state == CALIB_FULL_TRAVEL_MANUAL) {
            update = true;
            for (uint8_t r = 0; r < MATRIX_ROWS; r++) {
                for (uint8_t c = 0; c < MATRIX_COLS; c++) {
                    if ((analog_matrix_mask[r] & (0x01 << c)) == 0) continue;
                    if (calib_values[r][c].full_travel == 0 || calib_values[r][c].full_travel <= calib_values[r][c].zero_travel) {
                        if (saved_calib_values[r][c].zero_travel != 0) {
                            calib_values[r][c].zero_travel = saved_calib_values[r][c].zero_travel;
                        }
                        calib_values[r][c].full_travel = 4094;
                    }
                    saved_calib_values[r][c].zero_travel = calib_values[r][c].zero_travel;
                    saved_calib_values[r][c].full_travel = calib_values[r][c].full_travel;
                }
            }
        }
        cali_state = CALIB_OFF;
    }

    if (update) {
        save_calibration_values();

        switch (cali_state) {
            case CALIB_ZERO_TRAVEL_POWER_ON:
            case CALIB_FULL_TRAVEL_MANUAL:
                cali_state = CALIB_OFF;
                break;

            default:
                break;
        }

    } else if (cali_state == CALIB_ZERO_TRAVEL_POWER_ON) {
        cali_state = CALIB_OFF;
    }

    return true;
}

void calibration_validate(void) {
    for (uint8_t r = 0; r < MATRIX_ROWS; r++)
        for (uint8_t c = 0; c < MATRIX_COLS; c++) {
            if ((analog_matrix_mask[r] & (0x01 << c)) == 0) continue;

            uint16_t zero = saved_calib_values[r][c].zero_travel;
            uint16_t full = saved_calib_values[r][c].full_travel;
            bool     is_rev = (zero >= REV_POLARITY_ZERO_THRESHOLD);
            bool     invalid = (zero < VALID_ANALOG_RAW_VALUE_MIN || zero > VALID_ANALOG_RAW_VALUE_MAX ||
                                full < VALID_ANALOG_RAW_VALUE_MIN || full > VALID_ANALOG_RAW_VALUE_MAX);
            if (!invalid) {
                if (is_rev) {
                    if (full <= zero || (full - zero) < MIN_REV_FULL_TRAVEL_RANGE || full < MIN_REV_BOTTOM_OUT_ADC) invalid = true;
                } else {
                    if (full >= zero || (zero - full) < MIN_FULL_TRAVEL_RANGE) invalid = true;
                }
            }

            if (invalid) {
                if (is_rev && zero < MIN_REV_BOTTOM_OUT_ADC) {
                    saved_calib_values[r][c].full_travel = 4094;
                } else {
                    saved_calib_values[r][c].zero_travel = DEFAULT_ZERO_TRAVEL_VALUE;
                    saved_calib_values[r][c].full_travel = DEFAULT_ZERO_TRAVEL_VALUE - DEFAULT_FULL_RANGE;
                }
            }
        }
}

void auto_calibration_init(void) {
    for (uint8_t r = 0; r < MATRIX_ROWS; r++)
        for (uint8_t c = 0; c < MATRIX_COLS; c++) {
            memset(&auto_calib[r][c], 0, sizeof(auto_calib[0][0]));
        }
}

void auto_caliration_check(uint8_t row, uint8_t col, uint16_t value) {
    calibration_t *     p      = &auto_calib[row][col];
    calibrated_value_t *cur_c  = &calib_values[row][col];
    bool                is_rev = (cur_c->full_travel > cur_c->zero_travel || cur_c->zero_travel >= REV_POLARITY_ZERO_THRESHOLD);

    switch (p->state) {
        case AUTO_CALIB_OFF:
            if (p->pressed) {
                if (is_rev) {
                    if ((int32_t)value < (int32_t)cur_c->zero_travel + 25) {
                        p->pressed = false;
                    } else
                        return;
                } else {
                    if ((int32_t)value > (int32_t)cur_c->zero_travel - (DEFAULT_FULL_RANGE / 5)) {
                        p->pressed = false;
                    } else
                        return;
                }
            }
            p->state      = AUTO_CALIB_NEXT_LOOP; // fall through
            p->confidence = 0;
            break;

        case AUTO_CALIB_NEXT_LOOP:
            if (is_rev) {
                // Only enter full-travel state when key is pressed to true bottom-out (>= 4080 and >= zero + 80)
                if (value >= 4080 && (int32_t)value >= (int32_t)cur_c->zero_travel + 80) {
                    p->value.full_travel = value;
                    p->state             = AUTO_CALIB_FULL_TRAVEL;
                    p->full_travel_time  = timer_read32();
                }
            } else {
                if ((int32_t)value < (int32_t)cur_c->zero_travel - DEFAULT_FULL_RANGE + 100) {
                    p->value.full_travel = value;
                    p->state             = AUTO_CALIB_FULL_TRAVEL;
                    p->full_travel_time  = timer_read32();
                }
            }
            break;

        case AUTO_CALIB_ZERO_TRAVEL:
            if ((!is_rev && value > p->value.zero_travel) || (is_rev && value < p->value.zero_travel)) {
                // Key continues releasing toward top housing stop
                p->value.zero_travel = value;
                p->cycle             = 0;
            } else if (abs((int32_t)p->value.zero_travel - (int32_t)value) <= AUTO_CALIB_ZERO_TRAVEL_JITTER_VALUE) {
                // Key has stopped moving; collect 8 consecutive stationary samples
                calibrate_values[row][col][p->cycle++] = value;
                if (p->cycle >= CAL_SAMPL_CNT) {
                    if (timer_elapsed32(p->full_travel_time) < AUTO_CALIB_VALID_RELASING_TIME) {
                        uint32_t avg_val = 0;
                        for (uint8_t i = 0; i < CAL_SAMPL_CNT; i++)
                            avg_val += calibrate_values[row][col][i];

                        avg_val /= CAL_SAMPL_CNT;
                        p->value.zero_travel = avg_val;

                        int32_t range = abs((int32_t)avg_val - (int32_t)p->value.full_travel);
                        if (is_rev) {
                            // Verify the key snapped all the way back into the physical top-stop resting band (< 4025)
                            if (avg_val < 4025 && range >= 120) {
                                p->confidence += p->calibrated ? 6 : 4;
                            } else if (avg_val < 4025 && range >= 85) {
                                p->confidence += p->calibrated ? 4 : 3;
                            }
                        } else {
                            // Update confidence (normal polarity)
                            if (range > 1100) {
                                p->confidence += p->calibrated ? 12 : 6;
                            } else if (range > 1000) {
                                p->confidence += p->calibrated ? 4 : 3;
                            } else if (range > 900) {
                                p->confidence += p->calibrated ? 3 : 2;
                            }
                        }
                    }

                    p->state = AUTO_CALIB_NEXT_LOOP;
                }
            } else {
                p->state = AUTO_CALIB_NEXT_LOOP;
            }

            if (p->confidence >= 12) {
                p->state = AUTO_CALIB_FINISHED;
                p->calibrated = true;

                uint16_t adj_full = is_rev ? p->value.full_travel : (p->value.full_travel + BOTTOM_JITTER);
                if (is_rev && adj_full > 4094) adj_full = 4094;

                if (abs((int32_t)p->value.zero_travel - (int32_t)calib_values[row][col].zero_travel) > 8 ||
                    (!is_rev && abs((int32_t)adj_full - (int32_t)calib_values[row][col].full_travel) > 30) ||
                    (is_rev && adj_full > calib_values[row][col].full_travel)) {
                    calib_values[row][col].zero_travel = p->value.zero_travel;
                    if (!is_rev || adj_full > calib_values[row][col].full_travel) {
                        calib_values[row][col].full_travel = adj_full;
                    }

                    if (abs((int32_t)saved_calib_values[row][col].zero_travel - (int32_t)calib_values[row][col].zero_travel) > 15 ||
                        abs((int32_t)saved_calib_values[row][col].full_travel - (int32_t)calib_values[row][col].full_travel) > 50) {
                        saved_calib_values[row][col].zero_travel = calib_values[row][col].zero_travel;
                        saved_calib_values[row][col].full_travel = calib_values[row][col].full_travel;
                        save_calibration_value(row, col);
                    }

                    update_scale_factor(row, col);
                    update_key_config(row, col);
                }
            }
            break;

        case AUTO_CALIB_FULL_TRAVEL:
            if (!is_rev) {
                if (value < p->value.full_travel) {
                    // Key continues pressing
                    p->value.full_travel = value;
                    p->full_travel_time  = timer_read32();
                } else if (value - p->value.full_travel <= AUTO_CALIB_FULL_TRAVEL_JITTER_VALUE) {
                    // Key is still pressed, system noise jitter
                    p->full_travel_time = timer_read32();
                } else if (value - p->value.full_travel > AUTO_CALIB_ZERO_TRAVEL_THRESHOLD_VALUE) {
                    // Key releasing detected
                    p->value.zero_travel = value;
                    p->state             = AUTO_CALIB_ZERO_TRAVEL;
                    p->cycle             = 0;
                }
            } else {
                if (value > p->value.full_travel) {
                    // Key continues pressing (reverse polarity)
                    p->value.full_travel = value;
                    p->full_travel_time  = timer_read32();
                } else if (p->value.full_travel - value <= 4) {
                    // Key is still pressed at bottom-out, refresh release timer
                    p->full_travel_time = timer_read32();
                } else if (p->value.full_travel - value > 35) {
                    // Key releasing from bottom-out detected
                    p->value.zero_travel = value;
                    p->state             = AUTO_CALIB_ZERO_TRAVEL;
                    p->cycle             = 0;
                }
            }
            break;

        case AUTO_CALIB_FINISHED:
            if (!is_rev) {
                if (value > p->value.zero_travel + 50) p->state = AUTO_CALIB_OFF;
            } else {
                if (value < p->value.zero_travel + 25) p->state = AUTO_CALIB_OFF;
            }
            break;

        default:
            break;
    }
}

void analog_matrix_eeconfig_init(void) {
    bool reset_profiles = false;
    if (!eeconfig_is_enabled()) {
        eeconfig_init();
        reset_profiles = true;
    }

    profile_init(reset_profiles);

    uint8_t *buf = (uint8_t *)malloc(EECONFIG_KB_DATA_SIZE);
    memset(buf, 0, EECONFIG_KB_DATA_SIZE);

    eeconfig_read_kb_datablock(buf);

    // Load curve points
    point_t curve[CURVE_POINTS_COUNT];
    memcpy(curve, buf + OFFSET_CURVE_PTS_START, CURVE_POINTS_COUNT * SIZE_OF_POINT_T);
    game_controller_curve_init(curve);
    game_controller_mode_init(buf[OFFSET_GAME_CONTROLLER_MODE_START]);

    // Load calibration data
    calibrated = buf[OFFSET_CALIBRATION];
    memset(calib_values, 0, sizeof(calib_values));
    memset(saved_calib_values, 0, sizeof(saved_calib_values));

    if (calibrated) {
        memcpy(saved_calib_values, buf + CALIBRATED_CONFIG_START, sizeof(saved_calib_values));
    } else {
        uint32_t buf;
        he_eeprom_read_block(&buf, 0, 4); // Magic number

        if (buf != VENDOR_ID) {
            he_eeprom_driver_erase();
            buf = VENDOR_ID;
            he_eeprom_write_block(&buf, 0, 4);
            // cali_state = CALIB_ZERO_TRAVEL;
        } else {
            he_eeprom_read_block(&calibrated, (void *)(EXTERNAL_EEPROM_OFFSET + OFFSET_CALIBRATION), 1);
            if (calibrated) {
                he_eeprom_read_block(saved_calib_values, (void *)(EXTERNAL_EEPROM_OFFSET + CALIBRATED_CONFIG_START), sizeof(calib_values));
                // Save to emulated EEPROM
                uint32_t addr = EECONFIG_BASE_SIZE + OFFSET_CALIBRATION;
                eeprom_update_byte((uint8_t *)addr, calibrated);

                addr = EECONFIG_BASE_SIZE + CALIBRATED_CONFIG_START;
                eeprom_update_block(saved_calib_values, (uint8_t *)addr, sizeof(saved_calib_values));
            }
        }
    }

    // Validate calibrated data (supports both normal KS-37B and reverse-polarity KS-20)
    calibration_validate();
    update_default_travel();
    memcpy(calib_values, saved_calib_values, sizeof(saved_calib_values));

    // Load actuation/deacuation dat
    memset(analog_key_matrix, 0, sizeof(analog_key_matrix));
    memset(rev_ema, 0, sizeof(rev_ema));
    memset(rev_rest_val, 0, sizeof(rev_rest_val));
    memset(rev_rest_cnt, 0, sizeof(rev_rest_cnt));
    update_travel_configs();
    update_scale_factors();

    auto_calibration_init();
    cur_calib  = 0;
    cali_state = CALIB_ZERO_TRAVEL_POWER_ON;

    free(buf);
}

void analog_matrix_init(void) {
    he_eeprom_driver_init();

    analog_matrix_eeconfig_init();

    cur_calib = 0;
    memset(calibrate_values, 0, MATRIX_ROWS * MATRIX_COLS * CAL_SAMPL_CNT * sizeof(calibrate_values[0][0][0]));

    // Wait 30ms for 3.3V analog supply rail and Hall sensors to settle before sampling power-on zero
    wait_ms(30);

    // Settle analog matrix and scan
    for (uint8_t i = 0; i < CAL_SAMPL_CNT; i++)
        matrix_scan();

    // Check if Esc (Row 0, Col 0) is held down on boot for DFU entry
    // On pure KS-20 setup, resting ADC is ~3950. When pressed down, ADC rises to > 4060
    uint16_t esc_adc = analog_key_matrix[0][0].value;
    if (esc_adc >= 4050) {
        // Esc is held down on plug-in! Jump to STM32 DFU bootloader immediately
        bootloader_jump();
    }
}

void analog_matrix_save_col_state(uint8_t col) {
    for (uint8_t r = 0; r < MATRIX_ROWS; r++) {
        col_saved_key[r] = analog_key_matrix[r][col];
        col_saved_ema[r] = rev_ema[r][col];
    }
}

void analog_matrix_restore_col_state(uint8_t col) {
    for (uint8_t r = 0; r < MATRIX_ROWS; r++) {
        analog_key_matrix[r][col] = col_saved_key[r];
        rev_ema[r][col]           = col_saved_ema[r];
    }
}

bool update_raw_value(uint8_t row, uint8_t col, uint16_t value) {
    if ((analog_matrix_mask[row] & (0x01 << col)) == 0) return false;
    if (value < VALID_ANALOG_RAW_VALUE_MIN || value > VALID_ANALOG_RAW_VALUE_MAX) return false;

    if (cali_state) {
        calibrate_values[row][col][cur_calib] = value;
        analog_key_matrix[row][col].value     = value; // for debug
        rev_ema[row][col]                     = value << 2;
        return false;
    }

    analog_key_t *k = &analog_key_matrix[row][col];

    // Initialize EMA filter if needed
    if (rev_ema[row][col] == 0) {
        rev_ema[row][col] = value << 2;
    }

    // 4-tap Exponential Moving Average (EMA) filter:
    // Suppresses ADC power/multiplexer white noise (±3-5 counts) down to < 0.5 count
    rev_ema[row][col] = (rev_ema[row][col] * 3 + (value << 2)) >> 2;
    uint16_t smooth_val = (rev_ema[row][col] + 2) >> 2;

    auto_caliration_check(row, col, smooth_val);

    k->value = smooth_val;

    // Keychron-style input ADC deadband (Schmitt trigger):
    // Rejects spring ringing, stem lateral wobble, and sensor micro-jitter (< 3 smoothed counts)
    // everywhere in active travel, while allowing full return to 0 inside the resting zone
    // and full reach to 4.0mm in the top 2 counts near bottom-out.
    uint16_t zero = calib_values[row][col].zero_travel;
    uint16_t full = calib_values[row][col].full_travel;
    if (full <= zero) full = 4094;
    if (smooth_val > zero + 6 && smooth_val + 2 < full && abs((int32_t)k->last_val - (int32_t)smooth_val) < 3) {
        return false;
    }
    if (k->last_val == smooth_val) return false;

    k->last_val = smooth_val;
    k->travel   = convert_to_travel(row, col, smooth_val);

    if (k->travel == k->last_travel) return false;

    k->last_travel = k->travel;

    bool ret = false;

    switch (k->mode) {
        case AKM_RAPID:
            ret = rapid_trigger_action(k);
            break;
        case AKM_DKS:
            ret = okmc_action(k);
            break;
        case AKM_GAMEPAD:
#if defined(XINPUT_ENABLE)
#    if defined(JOYSTICK_ENABLE)
            if (game_controller_xinput_enabled())
#    endif
                ret = xinput_update(k);

#    if defined(JOYSTICK_ENABLE)
            else
#    endif
#endif
#ifdef JOYSTICK_ENABLE
                ret = joystick_update(k);
#endif
            break;
        case AKM_TOGGLE:
            ret = toggle_action(k);
            break;
        default:
            ret = regular_trigger_action(k);
            break;
    }

    return ret;
}

uint8_t analog_matrix_get_key_mode(uint8_t row, uint8_t col) {
    return analog_key_matrix[row][col].mode;
}

bool analog_matrix_get_key_state(uint8_t row, uint8_t col) {
    analog_key_t *k = &analog_key_matrix[row][col];

    switch (k->mode) {
        case AKM_REGULAR: // fall through
            return (k->state == AKS_REGULAR_PRESSED);

        case AKM_RAPID:
            return (k->state == AKS_REGULAR_PRESSED || k->state == AKS_RAPID_PRESSED);

        case AKM_GAMEPAD:
            return (game_controller_type_enabled() && k->state == AKS_REGULAR_PRESSED);

        case AKM_TOGGLE:
            return k->hold;

        default:
            return false;
    }
}

uint16_t pos2actuation(uint8_t row, uint8_t col, uint8_t point) {
    return 2000;
}

bool set_calibrate(uint8_t *data) {
    uint8_t new_cali_state = data[0];

    switch (new_cali_state) {
        case CALIB_ZERO_TRAVEL_MANUAL:
            cali_state = data[0];
            cur_calib  = 0;
            memset(calibrate_values, 0, MATRIX_ROWS * MATRIX_COLS * CAL_SAMPL_CNT * sizeof(calibrate_values[0][0][0]));
            calib_ind_timer = 0;

            memset(calib_values, 0, sizeof(calib_values));
            memset(rev_ema, 0, sizeof(rev_ema));
            memset(rev_rest_val, 0, sizeof(rev_rest_val));
            memset(rev_rest_cnt, 0, sizeof(rev_rest_cnt));
            memset(calib_state_matrix, 0, sizeof(calib_state_matrix));
            memset(manual_calib_zero_invalid, 0, sizeof(manual_calib_zero_invalid));
            break;

        case CALIB_SAVE_AND_EXIT:
            if (cali_state == CALIB_ZERO_TRAVEL_MANUAL || cali_state == CALIB_FULL_TRAVEL_MANUAL) {
                last_cali_state = cali_state;
                cali_state      = data[0];
            }
            break;

        default:
            return false;
    }

    return true;
}

bool get_realtime_travel(uint8_t *data) {
    uint8_t             i   = 1;
    uint8_t             row = data[0];
    uint8_t             col = data[1];
    extern matrix_row_t analog_raw_matrix[MATRIX_ROWS];

    if (row >= MATRIX_ROWS || col >= MATRIX_COLS) return false;

    data[i++] = row;
    data[i++] = col;
    data[i++] = analog_key_matrix[row][col].travel / TRAVEL_SCALE;
    data[i++] = analog_key_matrix[row][col].travel;
    data[i++] = analog_key_matrix[row][col].value & 0xFF;
    data[i++] = (analog_key_matrix[row][col].value >> 8) & 0xFF;
    data[i++] = calib_values[row][col].zero_travel & 0xFF;
    data[i++] = (calib_values[row][col].zero_travel >> 8) & 0xFF;
    data[i++] = calib_values[row][col].full_travel & 0xFF;
    data[i++] = (calib_values[row][col].full_travel >> 8) & 0xFF;
    data[i++] = analog_raw_matrix[row] & (0x01 << col) ? 0x11 : 0x10;

    return true;
}

static inline void track_resting_baseline(uint8_t row, uint8_t col, uint16_t value) {
    calibrated_value_t *p_calib = &calib_values[row][col];
    analog_key_t       *k       = &analog_key_matrix[row][col];

    if (p_calib->zero_travel == 0 || value < REV_POLARITY_ZERO_THRESHOLD) return;

    // ONE-WAY RELEASE-DIRECTION ONLY BASELINE RECOVERY:
    // On reverse-polarity KS-20 switches, pressing a key ALWAYS increases ADC (value > zero_travel).
    // Never increase zero_travel via a timer during runtime (which could re-zero held/feathered keys mid-stroke).
    // Upward thermal/stem-seating shift is handled strictly by Keychron's bottom-out-to-top-stop
    // release velocity state machine in auto_caliration_check().
    // Here we only lower zero_travel if the switch is released and settles strictly BELOW zero_travel for 500ms.
    if (k->state == AKS_REGULAR_RELEASED && value < p_calib->zero_travel) {
        int32_t diff = (int32_t)value - (int32_t)rev_rest_val[row][col];
        if (abs(diff) <= 1) {
            if (++rev_rest_cnt[row][col] >= 500) {
                rev_rest_cnt[row][col] = 0;
                p_calib->zero_travel   = value;
            }
        } else {
            rev_rest_val[row][col] = value;
            rev_rest_cnt[row][col] = 0;
        }
    } else if (k->state == AKS_REGULAR_RELEASED && value > p_calib->zero_travel &&
               value - p_calib->zero_travel <= 60 && k->travel < k->regular.actn_pt) {
        // RELEASED-ONLY UPWARD THERMAL DRIFT TRACKING (KS-20 on Q1 HE):
        // Untouched keys with a high resting ADC (~4000, only ~95 counts of total swing) warm up by
        // +25..35 counts within minutes, which exceeds the 0.2mm threshold. Follow this slow drift only
        // while the key is RELEASED, below its actuation point, and motionless within ±2 counts for 1000 scans.
        // Never applied to PRESSED keys, so holding/feathering a key is never re-zeroed or force-released.
        int32_t diff = (int32_t)value - (int32_t)rev_rest_val[row][col];
        if (abs(diff) <= 2) {
            if (++rev_rest_cnt[row][col] >= 1000) {
                rev_rest_cnt[row][col] = 0;
                p_calib->zero_travel   = rev_rest_val[row][col];
                k->travel              = convert_to_travel(row, col, value);
                k->last_travel         = k->travel;
            }
        } else {
            rev_rest_val[row][col] = value;
            rev_rest_cnt[row][col] = 0;
        }
    } else {
        rev_rest_val[row][col] = value;
        rev_rest_cnt[row][col] = 0;
    }
}

void analog_matrix_task(void) {
    if (cali_state == CALIB_OFF) {
        for (uint8_t r = 0; r < MATRIX_ROWS; r++) {
            for (uint8_t c = 0; c < MATRIX_COLS; c++) {
                if ((analog_matrix_mask[r] & (0x01 << c)) == 0) continue;
                track_resting_baseline(r, c, analog_key_matrix[r][c].value);
            }
        }
    }
    calibrate();
    profile_indication_timer_check();
    socd_action();
#ifdef JOYSTICK_ENABLE
    extern void joystick_task(void);
    joystick_task();
#endif
#ifdef XINPUT_ENABLE
    extern void xinput_task(void);
    xinput_task();
#endif
}

static void get_calibrate_state(uint8_t *data) {
    uint32_t rows[MATRIX_ROWS] = {0};

    for (uint8_t r = 0; r < MATRIX_ROWS; r++) {
        rows[r] = calib_state_matrix[r];
    }

    data[0] = calibrated;
    data[1] = cali_state;
    for (uint8_t i = 0; i < MATRIX_ROWS; i++)
        memcpy(data + 2 + i * 3, &rows[i], 3);
}

static bool get_calibrated_value(uint8_t row, uint8_t col, uint8_t *data) {
    uint8_t i = 0;

    if (row == 0xFF && col == 0xFF) {
        data[i++] = MATRIX_ROWS;
        data[i++] = 0;
        data[i++] = MATRIX_COLS;
        data[i++] = 0;
    } else if (row >= MATRIX_ROWS || col >= MATRIX_COLS)
        return false;

    if (analog_matrix_mask[row] & (0x01 << col)) {
        uint16_t z = calib_values[row][col].zero_travel;
        uint16_t f = calib_values[row][col].full_travel;
        if (f == 0 && z != 0) f = 4094;
        data[i++] = z & 0xFF;
        data[i++] = (z >> 8) & 0xFF;
        data[i++] = f & 0xFF;
        data[i++] = (f >> 8) & 0xFF;
    } else {
        memset(data + i, 0, 4);
        i += 4;
    }
    memcpy(&data[i], &scale_factor[row][col], sizeof(float));
    i += sizeof(float);

    return true;
}

void analog_matrix_rx(uint8_t *data, uint8_t length) {
    if (data[0] != 0xA9) return;

    uint8_t cmd     = data[1];
    bool    success = true;

    switch (cmd) {
        case AMC_GET_VERSION:
            data[2] = KC_ANALOG_MATRIX_VERSION & 0xFF;
            break;

        case AMC_GET_PROFILES_INFO:
            data[2] = profile_get_current_index();
            data[3] = PROFILE_COUNT;
            data[4] = PROFILE_SIZE & 0xFF;
            data[5] = (PROFILE_SIZE >> 8) & 0xFF;
            data[6] = OKMC_COUNT;
            data[7] = SOCD_COUNT;
            break;

        case AMC_GET_PROFILE_RAW: {
            uint8_t  index  = data[2];
            uint16_t offset = (data[4] << 8) | data[3];
            uint8_t  size   = data[5];
            success         = profile_get_raw_data(index, offset, size, &data[6]);
        } break;

        case AMC_SET_PROFILE_NAME:
            success = profile_set_name(data[2], data[3], &data[4]);
            data[2] = success ? 0 : 1;
            break;

        case AMC_SELECT_PROFILE:
            success = profile_select(data[2], false);
            data[2] = success ? 0 : 1;
            break;

        case AMC_SET_TRAVAL: {
            uint8_t  profile               = data[2];
            uint8_t  mode                  = data[3];
            uint8_t  act_pt                = data[4];
            uint8_t  sens                  = data[5];
            uint8_t  rls_sens              = data[6];
            bool     entire                = data[7];
            uint32_t row_mask[MATRIX_ROWS] = {0};
            if (!entire) {
                for (uint8_t i = 0, j = 8; i < MATRIX_ROWS; i++, j += 3) {
                    memcpy(&row_mask[i], &data[j], 3);
                }
            }

            success = profile_set_traval(profile, mode, act_pt, sens, rls_sens, entire, row_mask);
            data[2] = success ? 0 : 1;
        } break;

        case AMC_SET_ADVANCE_MODE:
            success = profile_set_adv_mode(&data[2]);
            data[2] = success ? 0 : 1;
            break;

        case AMC_SET_SOCD:
            success = profile_set_socd(&data[2]);
            data[2] = success ? 0 : 1;
            break;

        case AMC_GET_REALTIME_TRAVEL:
            success = get_realtime_travel(&data[2]);
            data[2] = success ? 0 : 1;
            break;

        case AMC_RESET_PROFILE:
            success = profile_reset(data[2]);
            update_travel_configs();
            data[2] = success ? 0 : 1;
            break;

        case AMC_SAVE_PROFILE:
            success = profile_save(data[2]);
            data[2] = success ? 0 : 1;
            break;

        case AMC_GET_CURVE:
            game_controller_get_curve(&data[2]);
            break;

        case AMC_SET_CURVE:
            success = game_controller_set_curve((point_t *)&data[2]);
            data[2] = success ? 0 : 1;
            break;

        case AMC_GET_GAME_CONTROLLER_MODE:
            success = game_controller_mode_get(&data[2]);
            data[2] = success ? 0 : 1;
            break;

        case AMC_SET_GAME_CONTROLLER_MODE:
            success = game_controller_mode_set(data[2]);
            break;

        case AMC_CALIBRATE:
            success = set_calibrate(&data[2]);
            data[2] = success ? 0 : 1;
            break;

        case AMC_GET_CALIBRATE_STATE:
            get_calibrate_state(&data[2]);
            break;

        case AMC_GET_CALIBRATED_VALUE:
            success = get_calibrated_value(data[2], data[3], &data[5]);
            data[4] = success ? 0 : 1;
            break;
    }

    raw_hid_send(data, length);
}

void analog_matrix_indicator(void) {
    if (cali_state == CALIB_FULL_TRAVEL_MANUAL) {
        rgb_matrix_set_color_all(150, 0, 150);

        for (uint8_t r = 0; r < MATRIX_ROWS; r++)
            for (uint8_t c = 0; c < MATRIX_COLS; c++) {
                if ((analog_matrix_mask[r] & (0x01 << c)) == 0) continue;

                // clang-format off
                if (calib_state_matrix[r] & (0x01 << c)) {
                    // clang-format on
                    if (last_calib_row == r && last_calib_col == c && timer_elapsed32(calib_ind_timer) < 500) continue;

                    uint8_t index = g_led_config.matrix_co[r][c];
                    rgb_matrix_set_color(index, 0, 255, 0);
                } else if (manual_calib_zero_invalid[r] & (0x01 << c)) {
                    uint8_t index = g_led_config.matrix_co[r][c];
                    rgb_matrix_set_color(index, 255, 0, 0);
                }
            }
        return;
    }
    profile_indication();
}

inline matrix_row_t analog_matrix_get_row(uint8_t row) {
    return virtual_matrix[row];
}

