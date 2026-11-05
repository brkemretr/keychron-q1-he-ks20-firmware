/* Copyright 2023 @ lokher (https://www.keychron.com)
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

/******************************************************************************
 *
 *  Filename:      lpm_stm32f401.c
 *
 *  Description:   Contains low power mode implementation
 *
 ******************************************************************************/

#include "quantum.h"
#include <usb_main.h>
#include "wireless.h"
#include "lpm.h"
#include "lpm_stm32f401.h"
#include "config.h"
#ifdef ANANLOG_MATRIX
#    include "analog_matrix.h"
#endif

static pm_t          power_mode           = PM_RUN;
static volatile bool lpm_rtc_wakeup_fired = false;
static bool          lpm_woke_by_input    = false;

bool lpm_woke_from_input(void) {
    return lpm_woke_by_input;
}

#if HAL_USE_RTC && defined(ANANLOG_MATRIX)
static void lpm_rtc_wakeup_cb(RTCDriver *rtcp, rtcevent_t event) {
    (void)rtcp;
    if (event == RTC_EVENT_WAKEUP) {
        lpm_rtc_wakeup_fired = true;
    }
}
#endif

bool lpm_set(pm_t mode) {
    bool ret = true;

    switch (mode) {
        case PM_SLEEP:
            /* Wake source: Any interrupt or event */
            if (power_mode != PM_RUN)
                ret = false;
            else
                SCB->SCR &= ~SCB_SCR_SLEEPDEEP_Msk;
            break;

        case PM_STOP:
            /* Wake source: Reset pin, all I/Os, BOR, PVD, PVM, RTC, LCD, IWDG,
               COMPx, USARTx, LPUART1, I2Cx, LPTIMx, USB, SWPMI */
            if (power_mode != PM_RUN)
                ret = false;
            else {
                SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk;
                PWR->CR |=
#if STOP_MODE_MAIN_REGULATOR_LOW_VOLTAGE
                    PWR_CR_MRLVDS |
#endif
#if STOP_MODE_LOW_POWER_REGULATOR_LOW_VOLTAG
                    PWR_CR_LPLVDS |
#endif
#if STOP_MODE_FLASH_POWER_DOWN
                    PWR_CR_FPDS |
#endif
#if STOP_MODE_LOW_POWER_DEEPSLEEP
                    PWR_CR_LPDS |
#endif
                    0;
            }
            break;

        case PM_STANDBY:
            if (power_mode != PM_RUN)
                ret = false;
            else {
                SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk;
            }
            break;

        default:
            break;
    }
    power_mode = mode;

    return ret;
}

void enter_power_mode(pm_t mode) {
    lpm_woke_by_input = false;
    enter_power_mode_kb(mode);
#if STM32_HSE_ENABLED
    /* Switch to HSI */
    RCC->CFGR = (RCC->CFGR & (~STM32_SW_MASK)) | STM32_SW_HSI;
    while ((RCC->CFGR & RCC_CFGR_SWS) != (STM32_SW_HSI << 2))
        ;

    /* Set HSE off  */
    RCC->CR &= ~RCC_CR_HSEON;
    while ((RCC->CR & RCC_CR_HSERDY))
        ;

    /* To avoid power consumption of floating GPIO */
    palSetLineMode(H0, PAL_MODE_INPUT_PULLDOWN);
    palSetLineMode(H1, PAL_MODE_INPUT_PULLDOWN);
#endif

#if HAL_USE_RTC && defined(ANANLOG_MATRIX)
    if (mode == PM_STOP) {
#    ifdef BT_MODE_SELECT_PIN
        uint8_t init_bt_pin = readPin(BT_MODE_SELECT_PIN);
#    endif
#    ifdef P2P4_MODE_SELECT_PIN
        uint8_t init_p2p4_pin = readPin(P2P4_MODE_SELECT_PIN);
#    endif
#    ifdef USB_POWER_SENSE_PIN
        uint8_t init_usb_pin = readPin(USB_POWER_SENSE_PIN);
#    endif
#    ifdef ANALOG_MATRIX_WAKEUP_PIN
        uint8_t init_wkup_pin = readPin(ANALOG_MATRIX_WAKEUP_PIN);
#    endif

        // Configure periodic RTC wakeup (~90ms at 32kHz LSI / 16 = 2kHz -> 180 ticks)
        // so reverse-polarity (North-pole down) KS-20 switches reliably wake the MCU from PM_STOP
        RTCD1.rtc->ISR &= ~RTC_ISR_WUTF;
        extiClearGroup1(EXTI_MASK1(STM32_RTC_WKUP_EXTI));
        RTCWakeup wakeupspec = { .wutr = (0U << 16) | 180U };
        rtcSetCallback(&RTCD1, lpm_rtc_wakeup_cb);
        rtcSTM32SetPeriodicWakeup(&RTCD1, &wakeupspec);

        while (true) {
            lpm_rtc_wakeup_fired = false;
            SCB->SCR |= SCB_SCR_SLEEPDEEP_Msk;
            __WFI();
            SCB->SCR &= ~SCB_SCR_SLEEPDEEP_Msk;

            if (readPin(LKBT51_INT_INPUT_PIN) == 0) {
                break;
            }
#    ifdef USB_POWER_SENSE_PIN
            if (readPin(USB_POWER_SENSE_PIN) != init_usb_pin || usb_power_connected()) {
                break;
            }
#    endif
#    ifdef BT_MODE_SELECT_PIN
            if (readPin(BT_MODE_SELECT_PIN) != init_bt_pin) {
                break;
            }
#    endif
#    ifdef P2P4_MODE_SELECT_PIN
            if (readPin(P2P4_MODE_SELECT_PIN) != init_p2p4_pin) {
                break;
            }
#    endif
#    ifdef ENCODER_SWITCH_PIN
            if (readPin(ENCODER_SWITCH_PIN) == 0) {
                lpm_woke_by_input = true;
                break;
            }
#    endif
#    ifdef ANALOG_MATRIX_WAKEUP_PIN
            if (readPin(ANALOG_MATRIX_WAKEUP_PIN) != init_wkup_pin) {
                lpm_woke_by_input = true;
                break;
            }
#    endif
            if (!lpm_rtc_wakeup_fired) {
                lpm_woke_by_input = true;
                break;
            }
            if (analog_matrix_lpm_any_key_pressed()) {
                lpm_woke_by_input = true;
                break;
            }
        }

        rtcSTM32SetPeriodicWakeup(&RTCD1, NULL);
        rtcSetCallback(&RTCD1, NULL);
    } else
#endif
    {
        __WFI();
        SCB->SCR &= ~SCB_SCR_SLEEPDEEP_Msk;
    }

    writePinLow(BLUETOOTH_INT_OUTPUT_PIN);
    stm32_clock_init();
    writePinHigh(BLUETOOTH_INT_OUTPUT_PIN);
}

void usb_power_connect(void) {}

void usb_power_disconnect(void) {}
