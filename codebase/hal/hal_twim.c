/**
 *  hal_twim.c : TWI Master HAL
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
 
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "hal_twim.h"
#include "stdbool.h"
#include "common_util.h"

#if ISR_MANAGER == 1
#include "isr_manager.h"
#endif

static struct {
    uint32_t scl;
    uint32_t sda;
    twim_transfer_t current_transfer;
    void (*handler)(twim_err_t err, twim_transfer_t transfer);
    bool transfer_finished;
    bool on;
    uint8_t evt_mask;
}twim_status;

/** @anchor twim_defines
 * @name Defines for the specific RTC peripheral used for ms timer
 * @{*/
#define TWIM_ID               CONCAT_2(NRF_TWIM,TWIM_USED)
#define TWIM_IRQN             TWIM_IRQN_a(TWIM_USED)
#define TWIM_IRQ_Handler      TWIM_IRQ_Handler_a(TWIM_USED)

#define TWIM_IRQN_a(n)        TWIM_IRQN_b(n)
#define TWIM_IRQN_b(n)        SPIM##n##_SPIS##n##_TWIM##n##_TWIS##n##_SPI##n##_TWI##n##_IRQn

#define TWIM_IRQ_Handler_a(n) TWIM_IRQ_Handler_b(n)
#define TWIM_IRQ_Handler_b(n) SPIM##n##_SPIS##n##_TWIM##n##_TWIS##n##_SPI##n##_TWI##n##_IRQHandler
/** @} */

#define TWIM_EVENT_CLEAR(x)       do{ \
         (x) = 0; \
         (void) (x); \
    }while(0)

static inline void send_event(twim_transfer_t txfr)
{
    //This works because the masks are 1 shifts of the transfer types
    if(twim_status.evt_mask | (1 << txfr))
    {
        twim_status.handler(TWIM_ERR_NONE, twim_status.current_transfer);
    }
}

static void clear_all_events(void)
{
    TWIM_ID->EVENTS_ERROR = 0;
    TWIM_ID->EVENTS_LASTRX = 0;
    TWIM_ID->EVENTS_LASTTX = 0;
    TWIM_ID->EVENTS_RXSTARTED = 0;
    TWIM_ID->EVENTS_STOPPED = 0;
    TWIM_ID->EVENTS_SUSPENDED = 0;
    TWIM_ID->EVENTS_TXSTARTED = 0;
}

static void handle_error(void)
{

    TWIM_ID->INTENCLR = TWIM_INTENCLR_STOPPED_Msk;
    (void)TWIM_ID->INTENCLR;

    twim_err_t err;
    if((TWIM_ID->ERRORSRC & TWIM_ERRORSRC_ANACK_Msk))
    {
        TWIM_ID->ERRORSRC = TWIM_ERRORSRC_ANACK_Msk;
        err = TWIM_ERR_ADRS_NACK;
    }

    if((TWIM_ID->ERRORSRC & TWIM_ERRORSRC_DNACK_Msk))
    {
        TWIM_ID->ERRORSRC = TWIM_ERRORSRC_DNACK_Msk;
        err = TWIM_ERR_DATA_NACK;
    }
    twim_status.handler(err, twim_status.current_transfer);
    /**TODO The stop will not generate an interrupt only when
    its at the end of this function. WHY??? */
    TWIM_ID->TASKS_STOP = 1;
}

void hal_twim_init(hal_twim_init_config_t * config)
{
    //Configure as strong drive low and disconnect high with pull-ups
    NRF_GPIO->PIN_CNF[config->scl] =
            ( (GPIO_PIN_CNF_SENSE_Disabled << GPIO_PIN_CNF_SENSE_Pos)
            | (GPIO_PIN_CNF_DRIVE_H0D1     << GPIO_PIN_CNF_DRIVE_Pos)
            | (GPIO_PIN_CNF_PULL_Pullup    << GPIO_PIN_CNF_PULL_Pos)
            | (GPIO_PIN_CNF_INPUT_Connect  << GPIO_PIN_CNF_INPUT_Pos)
            | (GPIO_PIN_CNF_DIR_Input      << GPIO_PIN_CNF_DIR_Pos));
    NRF_GPIO->PIN_CNF[config->sda] =
            ( (GPIO_PIN_CNF_SENSE_Disabled << GPIO_PIN_CNF_SENSE_Pos)
            | (GPIO_PIN_CNF_DRIVE_H0D1     << GPIO_PIN_CNF_DRIVE_Pos)
            | (GPIO_PIN_CNF_PULL_Pullup    << GPIO_PIN_CNF_PULL_Pos)
            | (GPIO_PIN_CNF_INPUT_Connect  << GPIO_PIN_CNF_INPUT_Pos)
            | (GPIO_PIN_CNF_DIR_Input      << GPIO_PIN_CNF_DIR_Pos));

    twim_status.scl = TWIM_ID->PSEL.SCL = config->scl;
    twim_status.sda = TWIM_ID->PSEL.SDA = config->sda;

    TWIM_ID->FREQUENCY = config->frequency;
    TWIM_ID->ADDRESS = config->address;
    //Use EasyDMA
    TWIM_ID->TXD.LIST = TWIM_TXD_LIST_LIST_Msk;

    clear_all_events();
    NVIC_ClearPendingIRQ(TWIM_IRQN);
    NVIC_SetPriority(TWIM_IRQN, config->irq_priority);
    NVIC_EnableIRQ(TWIM_IRQN);

    TWIM_ID->ENABLE = TWIM_ENABLE_ENABLE_Enabled << TWIM_ENABLE_ENABLE_Pos;
    twim_status.evt_mask = config->evt_mask;
    twim_status.handler = config->evt_handler;
    twim_status.transfer_finished = true;
    twim_status.on = true;
}

void hal_twim_uninit(void)
{
    if(twim_status.on == false){
        return;
    }
    twim_status.on = false;
    TWIM_ID->ENABLE = TWIM_ENABLE_ENABLE_Disabled << TWIM_ENABLE_ENABLE_Pos;

    NVIC_ClearPendingIRQ(TWIM_IRQN);
    NVIC_DisableIRQ(TWIM_IRQN);

    //Configure back to standard drive high and low
    NRF_GPIO->PIN_CNF[twim_status.scl] =
            ( (GPIO_PIN_CNF_SENSE_Disabled << GPIO_PIN_CNF_SENSE_Pos)
            | (GPIO_PIN_CNF_DRIVE_S0S1     << GPIO_PIN_CNF_DRIVE_Pos)
            | (GPIO_PIN_CNF_PULL_Disabled    << GPIO_PIN_CNF_PULL_Pos)
            | (GPIO_PIN_CNF_INPUT_Disconnect  << GPIO_PIN_CNF_INPUT_Pos)
            | (GPIO_PIN_CNF_DIR_Input      << GPIO_PIN_CNF_DIR_Pos));
    NRF_GPIO->PIN_CNF[twim_status.sda] =
            ( (GPIO_PIN_CNF_SENSE_Disabled << GPIO_PIN_CNF_SENSE_Pos)
            | (GPIO_PIN_CNF_DRIVE_S0S1     << GPIO_PIN_CNF_DRIVE_Pos)
            | (GPIO_PIN_CNF_PULL_Disabled    << GPIO_PIN_CNF_PULL_Pos)
            | (GPIO_PIN_CNF_INPUT_Disconnect  << GPIO_PIN_CNF_INPUT_Pos)
            | (GPIO_PIN_CNF_DIR_Input      << GPIO_PIN_CNF_DIR_Pos));

    TWIM_ID->ADDRESS = 0;
}

static twim_ret_status initial_txfr_check(void){
    if(twim_status.on == false)
    {
        return TWIM_UNINIT;
    }

    //Starting from a previous transfer where TWIM_TRANSFER_DONE_MSK wasn't set
    if(TWIM_ID->EVENTS_STOPPED)
    {
        twim_status.transfer_finished = true;
        return TWIM_STARTED;
    }

    if(twim_status.transfer_finished == false)
    {
        return TWIM_BUSY;
    }
    return TWIM_STARTED;
}

twim_ret_status hal_twim_tx(uint8_t * tx_ptr, uint32_t tx_len)
{
    twim_ret_status check_val = initial_txfr_check();
    if(check_val != TWIM_STARTED)
    {
        return check_val;
    }

    clear_all_events();
    TWIM_ID->TXD.PTR = (uint32_t) tx_ptr;
    TWIM_ID->TXD.MAXCNT = tx_len;
    TWIM_ID->SHORTS = TWIM_SHORTS_LASTTX_STOP_Msk;
    TWIM_ID->TASKS_STARTTX = 1;

    twim_status.current_transfer = TWIM_TX;
    twim_status.transfer_finished = false;

    //Error is always set, as its required to recover from an error
    TWIM_ID->INTEN = TWIM_INTENSET_ERROR_Msk;
    (void)TWIM_ID->INTEN;
    TWIM_ID->INTENSET = (twim_status.evt_mask & TWIM_TX_DONE_MSK)?
            (TWIM_INTENSET_STOPPED_Msk) : 0 ;

    return TWIM_STARTED;
}

twim_ret_status hal_twim_rx(uint8_t * rx_ptr, uint32_t rx_len)
{
    twim_ret_status check_val = initial_txfr_check();
    if(check_val != TWIM_STARTED)
    {
        return check_val;
    }

    clear_all_events();
    TWIM_ID->RXD.PTR = (uint32_t) rx_ptr;
    TWIM_ID->RXD.MAXCNT = rx_len;
    TWIM_ID->SHORTS = TWIM_SHORTS_LASTRX_STOP_Msk;
    TWIM_ID->TASKS_STARTRX = 1;

    twim_status.current_transfer = TWIM_RX;
    twim_status.transfer_finished = false;

    //Error is always set, as its required to recover from an error
    TWIM_ID->INTEN = TWIM_INTENSET_ERROR_Msk;
    (void)TWIM_ID->INTEN;
    TWIM_ID->INTENSET = (twim_status.evt_mask & TWIM_RX_DONE_MSK)?
            (TWIM_INTENSET_STOPPED_Msk) : 0 ;

    return TWIM_STARTED;
}

twim_ret_status hal_twim_tx_rx(uint8_t * tx_ptr, uint32_t tx_len,
        uint8_t * rx_ptr, uint32_t rx_len)
{
    twim_ret_status check_val = initial_txfr_check();
    if(check_val != TWIM_STARTED)
    {
        return check_val;
    }

    clear_all_events();
    TWIM_ID->TXD.PTR = (uint32_t) tx_ptr;
    TWIM_ID->TXD.MAXCNT = tx_len;
    TWIM_ID->RXD.PTR = (uint32_t) rx_ptr;
    TWIM_ID->RXD.MAXCNT = rx_len;
    TWIM_ID->SHORTS = TWIM_SHORTS_LASTTX_STARTRX_Msk |
            TWIM_SHORTS_LASTRX_STOP_Msk;
    TWIM_ID->TASKS_STARTTX = 1;

    twim_status.current_transfer = TWIM_TX_RX;
    twim_status.transfer_finished = false;

    //Error is always set, as its required to recover from an error
    TWIM_ID->INTEN = TWIM_INTENSET_ERROR_Msk;
    (void)TWIM_ID->INTEN;
    TWIM_ID->INTENSET = (twim_status.evt_mask & TWIM_TX_RX_DONE_MSK)?
            (TWIM_INTENSET_STOPPED_Msk) : 0 ;

    return TWIM_STARTED;
}

uint32_t hal_twim_get_current_adrs(void)
{
    return TWIM_ID->ADDRESS;
}
#if ISR_MANAGER == 1
void hal_twim_Handler (void)
#else
void TWIM_IRQ_Handler(void)
#endif
{
    if(TWIM_ID->EVENTS_ERROR == 1){
#if ISR_MANAGER == 0
        TWIM_EVENT_CLEAR(TWIM_ID->EVENTS_ERROR);
#endif
        handle_error();
    }

    if(TWIM_ID->EVENTS_STOPPED == 1){
#if ISR_MANAGER == 0
        TWIM_EVENT_CLEAR(TWIM_ID->EVENTS_STOPPED);
#endif
        twim_status.transfer_finished = true;

        send_event(twim_status.current_transfer);
    }
}
