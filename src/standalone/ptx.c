/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

/* ESB transmitter for a peripheral that owns the radio (no BLE). SHORTS turn
 * the radio around for the ACK, and TIMER2 bounds the wait and spaces out
 * retransmissions. */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/irq.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#include <zephyr/logging/log.h>
#include <hal/nrf_timer.h>

#include <zmk_split_esb/ptx.h>
#include <zmk_split_esb/stats.h>

#include "../phy/esb_phy.h"

LOG_MODULE_DECLARE(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

#define PTX_TIMER NRF_TIMER2
#define PTX_TIMER_IRQn TIMER2_IRQn

#define PIPE CONFIG_ZMK_SPLIT_ESB_PERIPHERAL_ID

/* From the end of our transmission: the receiver's TX ramp-up (~130 us)
 * plus preamble and address (~24 us at 2 Mbit/s), with margin. */
#define ACK_TIMEOUT_US 250

#define CC_ACK_TIMEOUT NRF_TIMER_CC_CHANNEL0
#define CC_RETRANSMIT NRF_TIMER_CC_CHANNEL1

enum ptx_state {
    PTX_STATE_IDLE,
    PTX_STATE_TX,
    PTX_STATE_WAIT_ACK,
    PTX_STATE_RETRANSMIT_WAIT,
};

static bool ready;
static volatile enum ptx_state state;
static uint8_t tx_buf[ESB_PHY_BUF_SIZE];
static uint8_t ack_buf[ESB_PHY_BUF_SIZE];
static uint8_t pid;
static uint8_t attempts;
static uint32_t first_tx_cycles;

static zmk_split_esb_ptx_done_cb_t done_cb;

void zmk_split_esb_ptx_set_done_cb(zmk_split_esb_ptx_done_cb_t cb) { done_cb = cb; }

static void timer_start(nrf_timer_cc_channel_t cc, uint32_t us) {
    nrf_timer_task_trigger(PTX_TIMER, NRF_TIMER_TASK_STOP);
    nrf_timer_task_trigger(PTX_TIMER, NRF_TIMER_TASK_CLEAR);
    nrf_timer_event_clear(PTX_TIMER, nrf_timer_compare_event_get(CC_ACK_TIMEOUT));
    nrf_timer_event_clear(PTX_TIMER, nrf_timer_compare_event_get(CC_RETRANSMIT));
    nrf_timer_int_disable(PTX_TIMER, NRF_TIMER_INT_COMPARE0_MASK | NRF_TIMER_INT_COMPARE1_MASK);
    nrf_timer_cc_set(PTX_TIMER, cc, us);
    nrf_timer_int_enable(PTX_TIMER, nrf_timer_compare_int_get(cc));
    nrf_timer_task_trigger(PTX_TIMER, NRF_TIMER_TASK_START);
}

static void timer_stop(void) {
    nrf_timer_task_trigger(PTX_TIMER, NRF_TIMER_TASK_STOP);
    nrf_timer_int_disable(PTX_TIMER, NRF_TIMER_INT_COMPARE0_MASK | NRF_TIMER_INT_COMPARE1_MASK);
}

static void start_tx(void) {
    NRF_RADIO->SHORTS = ESB_PHY_SHORTS_COMMON | NRF_RADIO_SHORT_DISABLED_RXEN_MASK;
    NRF_RADIO->PACKETPTR = (uint32_t)tx_buf;
    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
    state = PTX_STATE_TX;
    nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_TXEN);
}

static void finish(bool acked) {
    state = PTX_STATE_IDLE;
    pid = (pid + 1) & 0x3;

    uint8_t ack_len = acked ? MIN(ESB_PHY_LEN(ack_buf), ESB_PHY_MAX_PAYLOAD) : 0;

    if (acked) {
        atomic_inc(&zmk_split_esb_stats.tx_acked);
        atomic_set(&zmk_split_esb_stats.last_rtt_us,
                   (atomic_val_t)k_cyc_to_us_floor32(k_cycle_get_32() - first_tx_cycles));
    } else {
        atomic_inc(&zmk_split_esb_stats.tx_failed);
    }

    if (done_cb) {
        done_cb(acked, ESB_PHY_PAYLOAD(ack_buf), ack_len);
    }
}

static void radio_isr(const void *arg) {
    if (!nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_DISABLED)) {
        return;
    }
    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);

    switch (state) {
    case PTX_STATE_TX:
        /* Sent; SHORTS already started RX ramp-up for the ACK. */
        NRF_RADIO->SHORTS = ESB_PHY_SHORTS_COMMON;
        NRF_RADIO->PACKETPTR = (uint32_t)ack_buf;
        nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
        nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_END);
        state = PTX_STATE_WAIT_ACK;
        timer_start(CC_ACK_TIMEOUT, ACK_TIMEOUT_US);
        break;

    case PTX_STATE_WAIT_ACK: {
        timer_stop();
        bool acked = nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_END) &&
                     nrf_radio_crc_status_check(NRF_RADIO);
        if (acked) {
            finish(true);
        } else if (attempts < CONFIG_ZMK_SPLIT_ESB_PTX_RETRANSMIT_COUNT) {
            attempts++;
            atomic_inc(&zmk_split_esb_stats.tx_retransmits);
            state = PTX_STATE_RETRANSMIT_WAIT;
            timer_start(CC_RETRANSMIT, CONFIG_ZMK_SPLIT_ESB_PTX_RETRANSMIT_DELAY_US);
        } else {
            finish(false);
        }
        break;
    }

    default:
        break;
    }
}

static void timer_isr(const void *arg) {
    if (nrf_timer_event_check(PTX_TIMER, nrf_timer_compare_event_get(CC_ACK_TIMEOUT))) {
        nrf_timer_event_clear(PTX_TIMER, nrf_timer_compare_event_get(CC_ACK_TIMEOUT));
        /* Nothing heard: stop listening, the radio ISR then retries. */
        if (state == PTX_STATE_WAIT_ACK && !nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS)) {
            timer_stop();
            nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
        }
    }

    if (nrf_timer_event_check(PTX_TIMER, nrf_timer_compare_event_get(CC_RETRANSMIT))) {
        nrf_timer_event_clear(PTX_TIMER, nrf_timer_compare_event_get(CC_RETRANSMIT));
        if (state == PTX_STATE_RETRANSMIT_WAIT) {
            timer_stop();
            start_tx();
        }
    }
}

int zmk_split_esb_ptx_send(const uint8_t *data, uint8_t len) {
    if (len > ESB_PHY_MAX_PAYLOAD) {
        return -EINVAL;
    }

    unsigned int key = irq_lock();

    if (!ready || state != PTX_STATE_IDLE) {
        irq_unlock(key);
        return -EBUSY;
    }

    esb_phy_set_header(tx_buf, len, pid, false);
    memcpy(ESB_PHY_PAYLOAD(tx_buf), data, len);
    attempts = 0;
    first_tx_cycles = k_cycle_get_32();
    atomic_inc(&zmk_split_esb_stats.tx_sent);
    start_tx();

    irq_unlock(key);
    return 0;
}

static int ptx_init(void) {
    /* The radio needs the crystal oscillator. Keep it running for now;
     * releasing it between bursts is a later power optimisation. */
    struct onoff_manager *hf = z_nrf_clock_control_get_onoff(CLOCK_CONTROL_NRF_SUBSYS_HF);
    struct onoff_client cli;
    int res;

    sys_notify_init_spinwait(&cli.notify);
    int err = onoff_request(hf, &cli);
    if (err < 0) {
        LOG_ERR("HFXO request failed (%d)", err);
        return err;
    }
    while (sys_notify_fetch_result(&cli.notify, &res) == -EAGAIN) {
    }

    nrf_timer_mode_set(PTX_TIMER, NRF_TIMER_MODE_TIMER);
    nrf_timer_bit_width_set(PTX_TIMER, NRF_TIMER_BIT_WIDTH_32);
    nrf_timer_prescaler_set(PTX_TIMER, NRF_TIMER_FREQ_1MHz);

    nrf_radio_power_set(NRF_RADIO, true);
    esb_phy_configure();
    NRF_RADIO->TXADDRESS = PIPE;
    NRF_RADIO->RXADDRESSES = BIT(PIPE);
    nrf_radio_int_enable(NRF_RADIO, NRF_RADIO_INT_DISABLED_MASK);

    IRQ_CONNECT(RADIO_IRQn, 0, radio_isr, NULL, 0);
    IRQ_CONNECT(PTX_TIMER_IRQn, 0, timer_isr, NULL, 0);
    irq_enable(RADIO_IRQn);
    irq_enable(PTX_TIMER_IRQn);

    atomic_set(&zmk_split_esb_stats.channel, CONFIG_ZMK_SPLIT_ESB_RF_CHANNEL);
    ready = true;

    return 0;
}

/* Before ZMK's split peripheral enables transports, which starts sending. */
SYS_INIT(ptx_init, APPLICATION, ZMK_SPLIT_ESB_EARLY_INIT_PRIORITY);

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_TEST_PTX)

/* Phase 1 test traffic: a counter every few milliseconds. */
static void test_tx_cb(struct k_timer *timer) {
    static uint32_t counter;

    if (zmk_split_esb_ptx_send((const uint8_t *)&counter, sizeof(counter)) == 0) {
        counter++;
    }
}

static K_TIMER_DEFINE(test_tx_timer, test_tx_cb, NULL);

static int test_ptx_init(void) {
    k_timer_start(&test_tx_timer, K_MSEC(1000), K_MSEC(CONFIG_ZMK_SPLIT_ESB_TEST_PTX_INTERVAL_MS));
    return 0;
}

SYS_INIT(test_ptx_init, APPLICATION, 99);

#endif /* IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_TEST_PTX) */
