/**
 *  tssp_detect.c : TSSP detector driver
 *  Copyright (C) 2019  Appiko
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *  
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "tssp_detect.h"
#include "hal_gpio.h"
#include "common_util.h"
#include "hal_clocks.h"
#include "nrf_util.h"
#include "stddef.h"
#include "log.h"

#if ISR_MANAGER == 1
#include "isr_manager.h"
#endif

/** RTC used by this module */
#define TSSP_DETECT_RTC_USED CONCAT_2(NRF_RTC, RTC_USED_TSSP_DETECT)

/** EGU channel used by this module */
#define TSSP_DETECT_EGU_USED CONCAT_2(NRF_EGU,EGU_USED_TSSP_DETECT)

/** Channel 1 of PPI is used here for RTC */
#define PPI_CHANNEL_USED_RTC PPI_CH_USED_TSSP_DETECT_1

/** Channel 2 of PPI is used here for EGU */
#define PPI_CHANNEL_USED_EGU PPI_CH_USED_TSSP_DETECT_2

/** Channel 2 of GPIOTE is used here */
#define GPIOTE_CHANNEL_USED GPIOTE_CH_USED_TSSP_DETECT

/** Channel 2 of RTC0 is used for window detection */
#define WINDOW_RTC_CHANNEL 2
/** Channel 0 of RTC0 is used for Synchronization */
#define SYNC_ON_RTC_CHANNEL 0
/** Channel 1 of RTC0 is used for Synchronization */
#define SYNC_OFF_RTC_CHANNEL 1
/** Channel 0 of EGU0 is used here */
#define EGU_CHANNEL_USED EGU_CHANNEL_USED_TSSP_DETECT
/** Half of duration for which sensor will be enabled while detecting window */
#define HALF_TSSP_ENABLE_DURATION TSSP_DETECT_TICKS_MS(2)


#ifndef ENABLE
#define ENABLE 1
#endif

#ifndef DISABLE
#define DISABLE 0
#endif

/** Pin number of Enable pin present on TSSP module */
uint32_t tssp_en_pin;
/** Pin number of Rx pin present on TSSP module */
uint32_t tssp_rx_pin;

uint32_t tssp_sync_ms;

/** Flag to check if GPIOTE is required for pulse detection */
static bool is_pulse_detect_req = false;

/** Flag to check if GPIOTE is required for window detection */
static bool is_window_detect_req = false;

/** Function pointer which is to be called if it doesn't detect any pulse in\
 *  given window of time */
void (*missed_handler)(void);

/**Function pointer which is to be called if module detects the pulse*/
void (*detect_handler)(uint32_t ticks);

void tssp_detect_init (tssp_detect_config_t * tssp_detect_config)
{
    tssp_en_pin = tssp_detect_config->rx_en_pin;
    hal_gpio_cfg_output (tssp_en_pin, DISABLE);
    tssp_rx_pin = tssp_detect_config->rx_in_pin;
    hal_gpio_cfg_input (tssp_rx_pin,
                        HAL_GPIO_PULL_UP);

    if(tssp_detect_config->tssp_missed_handler != NULL)
    {
        is_window_detect_req = true;
        missed_handler = tssp_detect_config->tssp_missed_handler;
        TSSP_DETECT_RTC_USED->PRESCALER = ROUNDED_DIV(LFCLK_FREQ, TSSP_DETECT_FREQ) - 1;
        TSSP_DETECT_RTC_USED->CC[WINDOW_RTC_CHANNEL] = TSSP_DETECT_TICKS_MS(tssp_detect_config->window_duration_ticks);
        TSSP_DETECT_RTC_USED->INTENSET |= ENABLE << (WINDOW_RTC_CHANNEL+16);
                    
        NRF_PPI->CH[PPI_CHANNEL_USED_RTC].EEP = (uint32_t) &NRF_GPIOTE->EVENTS_IN[GPIOTE_CHANNEL_USED];
        NRF_PPI->CH[PPI_CHANNEL_USED_RTC].TEP = (uint32_t) &TSSP_DETECT_RTC_USED->TASKS_CLEAR;
        
    }
    else
    {
        is_window_detect_req = false;
    }
    if(tssp_detect_config->tssp_detect_handler != NULL)
    {
        is_pulse_detect_req = true;
        
        detect_handler = tssp_detect_config->tssp_detect_handler;

        TSSP_DETECT_EGU_USED->INTENSET |= ENABLE << EGU_CHANNEL_USED;
        NVIC_SetPriority (SWI0_EGU0_IRQn, APP_IRQ_PRIORITY_HIGHEST);
        NVIC_EnableIRQ (SWI0_IRQn);
        NVIC_ClearPendingIRQ (SWI0_IRQn);

        NRF_PPI->CH[PPI_CHANNEL_USED_EGU].EEP = (uint32_t) &NRF_GPIOTE->EVENTS_IN[GPIOTE_CHANNEL_USED];
        NRF_PPI->CH[PPI_CHANNEL_USED_EGU].TEP = (uint32_t) &TSSP_DETECT_EGU_USED->TASKS_TRIGGER[EGU_CHANNEL_USED];
    }
    else
    {
        is_pulse_detect_req = false;
    }

}

void tssp_detect_window_detect ()
{
  
    is_window_detect_req = true;
    
    
    TSSP_DETECT_RTC_USED->INTENSET |= ENABLE << (WINDOW_RTC_CHANNEL+16);
    NRF_PPI->CHENSET |= 1 << PPI_CHANNEL_USED_RTC;

    NRF_GPIOTE->EVENTS_IN[GPIOTE_CHANNEL_USED] = 0;
    
    NRF_GPIOTE->CONFIG[GPIOTE_CHANNEL_USED] =
        GPIOTE_CONFIG_MODE_Event << GPIOTE_CONFIG_MODE_Pos|
        GPIOTE_CONFIG_POLARITY_HiToLo << GPIOTE_CONFIG_POLARITY_Pos| 
        ((tssp_rx_pin << GPIOTE_CONFIG_PSEL_Pos) 
         & GPIOTE_CONFIG_PSEL_Msk);

    TSSP_DETECT_RTC_USED->EVENTS_COMPARE[WINDOW_RTC_CHANNEL] = 0;
    (void) TSSP_DETECT_RTC_USED->EVENTS_COMPARE[WINDOW_RTC_CHANNEL];
    
    TSSP_DETECT_RTC_USED->TASKS_START = 1;   
    NVIC_SetPriority (RTC0_IRQn, APP_IRQ_PRIORITY_HIGHEST);
    NVIC_EnableIRQ (RTC0_IRQn);
}

void tssp_detect_pulse_stop ()
{
    is_pulse_detect_req = false;
    if((is_pulse_detect_req == false) && (is_window_detect_req == false))
    {
        NRF_GPIOTE->CONFIG[GPIOTE_CHANNEL_USED] =
            GPIOTE_CONFIG_MODE_Disabled << GPIOTE_CONFIG_MODE_Pos|
            GPIOTE_CONFIG_POLARITY_None << GPIOTE_CONFIG_POLARITY_Pos| 
            ((tssp_rx_pin << GPIOTE_CONFIG_PSEL_Pos) 
             & GPIOTE_CONFIG_PSEL_Msk);

        hal_gpio_pin_write (tssp_en_pin, DISABLE);
    }
    NRF_PPI->CHENCLR |= 1 << PPI_CHANNEL_USED_EGU;
}

void tssp_detect_window_stop (void)
{
    is_window_detect_req = false;
    if((is_pulse_detect_req == false) && (is_window_detect_req == false))
    {
        NRF_GPIOTE->CONFIG[GPIOTE_CHANNEL_USED] =
            GPIOTE_CONFIG_MODE_Disabled << GPIOTE_CONFIG_MODE_Pos|
            GPIOTE_CONFIG_POLARITY_None << GPIOTE_CONFIG_POLARITY_Pos| 
            ((tssp_rx_pin << GPIOTE_CONFIG_PSEL_Pos) 
             & GPIOTE_CONFIG_PSEL_Msk);

        hal_gpio_pin_write (tssp_en_pin, DISABLE);
    }
    TSSP_DETECT_RTC_USED->INTENCLR |= ENABLE << (WINDOW_RTC_CHANNEL+16) | 
                                      ENABLE << (SYNC_ON_RTC_CHANNEL+16) | 
                                      ENABLE << (SYNC_OFF_RTC_CHANNEL+16);
    NRF_PPI->CHENCLR |= 1 << PPI_CHANNEL_USED_RTC;
    NVIC_DisableIRQ  (RTC0_IRQn);

    TSSP_DETECT_RTC_USED->TASKS_CLEAR = 1;
    (void) TSSP_DETECT_RTC_USED->TASKS_CLEAR;
    TSSP_DETECT_RTC_USED->TASKS_STOP = 1;
}

void tssp_detect_pulse_detect ()
{
    is_pulse_detect_req = true;
    TSSP_DETECT_RTC_USED->TASKS_START = 1;
    NRF_GPIOTE->EVENTS_IN[GPIOTE_CHANNEL_USED] = 0;
    
    NRF_GPIOTE->CONFIG[GPIOTE_CHANNEL_USED] =
        GPIOTE_CONFIG_MODE_Event << GPIOTE_CONFIG_MODE_Pos|
        GPIOTE_CONFIG_POLARITY_HiToLo << GPIOTE_CONFIG_POLARITY_Pos| 
        ((tssp_rx_pin << GPIOTE_CONFIG_PSEL_Pos) 
         & GPIOTE_CONFIG_PSEL_Msk);

    hal_gpio_pin_write (tssp_en_pin, ENABLE);

    TSSP_DETECT_EGU_USED->INTENSET |= ENABLE << EGU_CHANNEL_USED;
    NRF_PPI->CHENSET |= 1 << PPI_CHANNEL_USED_EGU;
}

void tssp_detect_window_sync (uint32_t sync_ms)
{
    tssp_sync_ms = (sync_ms);
    
    uint32_t rtc_counter;
    rtc_counter = TSSP_DETECT_RTC_USED->COUNTER ;
    TSSP_DETECT_RTC_USED->CC[SYNC_ON_RTC_CHANNEL] =
        (rtc_counter + (tssp_sync_ms - HALF_TSSP_ENABLE_DURATION)) ;
    (void)    TSSP_DETECT_RTC_USED->CC[SYNC_ON_RTC_CHANNEL];
    TSSP_DETECT_RTC_USED->INTENSET |= ENABLE << (SYNC_ON_RTC_CHANNEL+16);
    TSSP_DETECT_RTC_USED->EVENTS_COMPARE[SYNC_ON_RTC_CHANNEL] = 0;

    TSSP_DETECT_RTC_USED->CC[SYNC_OFF_RTC_CHANNEL] = HALF_TSSP_ENABLE_DURATION;
    (void)    TSSP_DETECT_RTC_USED->CC[SYNC_OFF_RTC_CHANNEL];
    TSSP_DETECT_RTC_USED->INTENSET |= ENABLE << (SYNC_OFF_RTC_CHANNEL+16);
    TSSP_DETECT_RTC_USED->EVENTS_COMPARE[SYNC_OFF_RTC_CHANNEL] = 0;
    
}

#if ISR_MANAGER == 1
void tssp_detect_swi_Handler (void)
#else
void SWI0_IRQHandler ()
#endif
{
#if ISR_MANAGER == 0
    TSSP_DETECT_EGU_USED->EVENTS_TRIGGERED[EGU_CHANNEL_USED] = 0;
    (void) TSSP_DETECT_EGU_USED->EVENTS_TRIGGERED[EGU_CHANNEL_USED];
#endif
    NRF_PPI->CHENCLR |= 1 << PPI_CHANNEL_USED_EGU;
    detect_handler ( TSSP_DETECT_RTC_USED->COUNTER );
}

#if ISR_MANAGER == 1
void tssp_detect_rtc_Handler (void)
#else
void RTC0_IRQHandler (void)
#endif
{
    if(TSSP_DETECT_RTC_USED->EVENTS_COMPARE[SYNC_ON_RTC_CHANNEL] == 1)
    {
#if ISR_MANAGER == 0
        TSSP_DETECT_RTC_USED->EVENTS_COMPARE[SYNC_ON_RTC_CHANNEL] = 0;
        (void) TSSP_DETECT_RTC_USED->EVENTS_COMPARE[SYNC_ON_RTC_CHANNEL];
#endif
        hal_gpio_pin_write (tssp_en_pin, ENABLE);
        TSSP_DETECT_RTC_USED->CC[SYNC_OFF_RTC_CHANNEL] = HALF_TSSP_ENABLE_DURATION;
        
    }
    if(TSSP_DETECT_RTC_USED->EVENTS_COMPARE[SYNC_OFF_RTC_CHANNEL] == 1)
    {
#if ISR_MANAGER == 0
        TSSP_DETECT_RTC_USED->EVENTS_COMPARE[SYNC_OFF_RTC_CHANNEL] = 0;
        (void) TSSP_DETECT_RTC_USED->EVENTS_COMPARE[SYNC_OFF_RTC_CHANNEL];
#endif
        hal_gpio_pin_write (tssp_en_pin, DISABLE);
        TSSP_DETECT_RTC_USED->CC[SYNC_ON_RTC_CHANNEL] = (tssp_sync_ms - HALF_TSSP_ENABLE_DURATION);
    }
    if(TSSP_DETECT_RTC_USED->EVENTS_COMPARE[WINDOW_RTC_CHANNEL] == 1)
    {
#if ISR_MANAGER == 0
        TSSP_DETECT_RTC_USED->EVENTS_COMPARE[WINDOW_RTC_CHANNEL] = 0;
        (void) TSSP_DETECT_RTC_USED->EVENTS_COMPARE[WINDOW_RTC_CHANNEL];
#endif
        missed_handler ();
        TSSP_DETECT_RTC_USED->TASKS_CLEAR = 1;
        (void) TSSP_DETECT_RTC_USED->TASKS_CLEAR;
    }
}
