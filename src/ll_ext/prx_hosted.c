/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

/* ESB receiver hosted in the Zephyr BLE controller via CONFIG_BT_CTLR_USER_EXT.
 * Like the continuous scanner: an unreserved periodic ticker feeds prepares,
 * the receiver listens as long as it can, yields to BLE events and resumes
 * after. ACKs go out through RADIO SHORTS, so no timer or PPI is taken. */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <soc.h>
#include <zephyr/bluetooth/hci_types.h>

#include "hal/cpu.h"
#include "hal/ccm.h"
#include "hal/radio.h"
#include "hal/ticker.h"
#include "util/util.h"
#include "util/memq.h"
#include "util/mayfly.h"
#include "ticker/ticker.h"
#include "pdu_df.h"
#include "pdu_vendor.h"
#include "pdu.h"
#include "lll.h"
#include "lll_vendor.h"
#include "lll_clock.h"
#include "lll_internal.h"
#include "ull_internal.h"
#include "hal/debug.h"

#include "ull_vendor.h"

struct node_rx_pdu;
struct pdu_data;
struct net_buf;
#include "hci/hci_user_ext.h"

#include <zmk_split_esb/stats.h>
#include <zmk_split_esb/prx.h>

#include "../phy/esb_phy.h"

#define PRX_TICKER_INTERVAL_US (CONFIG_ZMK_SPLIT_ESB_PRX_TICKER_INTERVAL_MS * 1000U)

struct lll_prx {
    struct lll_hdr hdr;
};

static struct {
    struct ull_hdr ull;
    struct lll_prx lll;
} prx;

enum prx_state {
    PRX_STATE_RX,
    PRX_STATE_TX_ACK,
};

static enum prx_state state;
static uint8_t rx_buf[ESB_PHY_BUF_SIZE];
static uint8_t ack_buf[ESB_PHY_BUF_SIZE];

/* Last packet seen per pipe, to drop retransmissions of packets we already
 * ACKed but whose ACK the peripheral missed. */
static struct {
    uint32_t crc;
    uint8_t pid;
    bool valid;
} pipe_last[8];

static zmk_split_esb_prx_rx_cb_t rx_cb;

void zmk_split_esb_prx_set_rx_cb(zmk_split_esb_prx_rx_cb_t cb) { rx_cb = cb; }

/* The thread writes data only while len is 0 and publishes it by setting
 * len; the ISR reads data only while len is non-zero. */
static struct {
    atomic_t len;
    uint8_t data[ESB_PHY_MAX_PAYLOAD];
} ack_slot[8];

int zmk_split_esb_prx_stage_ack(uint8_t pipe, const uint8_t *data, uint8_t len) {
    if (pipe >= ARRAY_SIZE(ack_slot) || len == 0 || len > ESB_PHY_MAX_PAYLOAD) {
        return -EINVAL;
    }
    if (atomic_get(&ack_slot[pipe].len)) {
        return -EBUSY;
    }

    memcpy(ack_slot[pipe].data, data, len);
    atomic_set(&ack_slot[pipe].len, len);
    return 0;
}

void zmk_split_esb_prx_clear_ack(uint8_t pipe) {
    if (pipe < ARRAY_SIZE(ack_slot)) {
        atomic_set(&ack_slot[pipe].len, 0);
    }
}

static int prepare_cb(struct lll_prepare_param *p);
static int resume_prepare_cb(struct lll_prepare_param *p);
static int is_abort_cb(void *next, void *curr, lll_prepare_cb_t *resume_cb);
static void abort_cb(struct lll_prepare_param *prepare_param, void *param);
static void isr_esb(void *param);
static void isr_done_cleanup(void *param);

/* LLL context. */

static void prx_prepare(void *param) {
    int err;

    err = lll_hfclock_on();
    LL_ASSERT(err >= 0);

    err = lll_prepare(is_abort_cb, abort_cb, prepare_cb, 0, param);
    LL_ASSERT(!err || err == -EINPROGRESS);
}

static void rx_start_listening(void) {
    NRF_RADIO->SHORTS = ESB_PHY_SHORTS_COMMON | NRF_RADIO_SHORT_DISABLED_TXEN_MASK;
    NRF_RADIO->PACKETPTR = (uint32_t)rx_buf;
    state = PRX_STATE_RX;
}

static int common_prepare_cb(struct lll_prepare_param *p, bool is_resume) {
    struct lll_prx *lll = p->param;
    struct ull_hdr *ull = HDR_LLL2ULL(lll);
    uint32_t ticks_at_event, ticks_at_start, remainder;

    radio_reset();
    /* Make sure BLE's software tIFS switch can't fire off our END events. */
    radio_switch_complete_and_disable();
    radio_status_reset();

    esb_phy_configure();
    NRF_RADIO->RXADDRESSES = ESB_PHY_PERIPHERAL_PIPES;
    rx_start_listening();

    radio_isr_set(isr_esb, lll);

    ticks_at_event = p->ticks_at_expire + lll_event_offset_get(ull);
    ticks_at_start = ticks_at_event + HAL_TICKER_US_TO_TICKS(EVENT_OVERHEAD_START_US);
    remainder = p->remainder;

    /* A prepare that waited behind a BLE event is late, and its start time
     * may already have passed. The HF clock was requested when it was
     * queued, so start as soon as the radio can instead of dropping it. */
    uint32_t now = ticker_ticks_now_get();
    uint32_t lead = ticker_ticks_diff_get(ticks_at_start, now);
    if ((lead & BIT(HAL_TICKER_CNTR_MSBIT)) ||
        lead < HAL_TICKER_US_TO_TICKS(EVENT_OVERHEAD_START_US)) {
        ticks_at_start = now + HAL_TICKER_US_TO_TICKS(EVENT_OVERHEAD_START_US);
        remainder = 0;
    }

    radio_tmr_start(0, ticks_at_start, remainder);

    atomic_inc(&zmk_split_esb_stats.prx_windows);

    int err = lll_prepare_done(lll);
    LL_ASSERT(!err);

    return 0;
}

static int prepare_cb(struct lll_prepare_param *p) { return common_prepare_cb(p, false); }

static int resume_prepare_cb(struct lll_prepare_param *p) {
    struct ull_hdr *ull = HDR_LLL2ULL(p->param);

    p->ticks_at_expire = ticker_ticks_now_get() - lll_event_offset_get(ull);
    p->remainder = 0;
    p->lazy = 0;

    return common_prepare_cb(p, true);
}

static int is_abort_cb(void *next, void *curr, lll_prepare_cb_t *resume_cb) {
    /* Our own next periodic prepare: keep listening, drop the new one. */
    if (next == curr) {
        /* The PPI that started reception is normally disabled by the first
         * radio ISR. With no packets and no BLE event since, it is still
         * armed, and the 24-bit event timer would fire it again after
         * ~16 s, so disarm it here once the radio is listening. */
        if (nrf_radio_state_get(NRF_RADIO) == NRF_RADIO_STATE_RX ||
            nrf_radio_state_get(NRF_RADIO) == NRF_RADIO_STATE_RXIDLE) {
            radio_tmr_status_reset();
        }
        return 0;
    }

    /* Anything else is a BLE event, which always wins. Yield to it and get
     * resumed once it's done. */
    *resume_cb = resume_prepare_cb;

    int err = lll_hfclock_on();
    LL_ASSERT(err >= 0);

    atomic_inc(&zmk_split_esb_stats.prx_preempts);

    return -EAGAIN;
}

static void abort_cb(struct lll_prepare_param *prepare_param, void *param) {
    /* The running event is being stopped. */
    if (!prepare_param) {
        radio_isr_set(isr_done_cleanup, param);
        radio_disable();
        return;
    }

    /* A prepare of ours still in the pipeline is being cancelled. */
    int err = lll_hfclock_off();
    LL_ASSERT(err >= 0);

    lll_done(param);
}

static void isr_done_cleanup(void *param) {
    bool is_resume;

    lll_isr_status_reset();

    if (lll_is_done(param, &is_resume)) {
        return;
    }

    lll_isr_cleanup(param);
}

/* Abort the ACK the hardware has started ramping up for, and listen again. */
static void restart_rx(void) {
    NRF_RADIO->SHORTS = ESB_PHY_SHORTS_COMMON;
    NRF_RADIO->PACKETPTR = (uint32_t)rx_buf;

    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
    nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
    while (!nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_DISABLED)) {
    }
    nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);

    rx_start_listening();
    nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RXEN);
}

static void on_rx_disabled(void) {
    if (!radio_crc_is_valid()) {
        atomic_inc(&zmk_split_esb_stats.rx_crc_err);
        restart_rx();
        return;
    }

    uint8_t pipe = NRF_RADIO->RXMATCH;
    uint32_t crc = NRF_RADIO->RXCRC;
    uint8_t pid = ESB_PHY_PID(rx_buf);
    uint8_t len = MIN(ESB_PHY_LEN(rx_buf), ESB_PHY_MAX_PAYLOAD);
    bool dup = pipe_last[pipe].valid && pipe_last[pipe].pid == pid && pipe_last[pipe].crc == crc;

    pipe_last[pipe].crc = crc;
    pipe_last[pipe].pid = pid;
    pipe_last[pipe].valid = true;

    if (ESB_PHY_NOACK(rx_buf)) {
        restart_rx();
    } else {
        /* SHORTS already started TX ramp-up when the radio disabled; it has
         * ~130 us before it reads PACKETPTR. */
        NRF_RADIO->TXADDRESS = pipe;
        uint8_t ack_len = atomic_get(&ack_slot[pipe].len);
        if (ack_len) {
            memcpy(ESB_PHY_PAYLOAD(ack_buf), ack_slot[pipe].data, ack_len);
        }
        esb_phy_set_header(ack_buf, ack_len, pid, false);
        NRF_RADIO->PACKETPTR = (uint32_t)ack_buf;
        NRF_RADIO->SHORTS = ESB_PHY_SHORTS_COMMON | NRF_RADIO_SHORT_DISABLED_RXEN_MASK;
        state = PRX_STATE_TX_ACK;
        atomic_inc(&zmk_split_esb_stats.ack_payloads);
    }

    if (!dup) {
        atomic_inc(&zmk_split_esb_stats.rx_ok);
        atomic_set(&zmk_split_esb_stats.last_rx_ms, (atomic_val_t)k_uptime_get_32());
        if (rx_cb) {
            rx_cb(pipe, ESB_PHY_PAYLOAD(rx_buf), len);
        }
    }
}

static void isr_esb(void *param) {
    /* Clears the events and the PPI that started reception on time. */
    radio_status_reset();
    radio_tmr_status_reset();

    switch (state) {
    case PRX_STATE_RX:
        on_rx_disabled();
        break;
    case PRX_STATE_TX_ACK:
        /* ACK sent; SHORTS already started RX ramp-up. */
        rx_start_listening();
        break;
    }
}

/* ULL context. */

static void ticker_cb(uint32_t ticks_at_expire, uint32_t ticks_drift, uint32_t remainder,
                      uint16_t lazy, uint8_t force, void *param) {
    static memq_link_t link;
    static struct mayfly mfy = {0, 0, &link, NULL, prx_prepare};
    static struct lll_prepare_param p;

    uint8_t ref = ull_ref_inc(&prx.ull);
    LL_ASSERT(ref);

    p.ticks_at_expire = ticks_at_expire;
    p.remainder = remainder;
    p.lazy = lazy;
    p.param = &prx.lll;
    p.force = force;
    mfy.param = &p;

    uint32_t ret = mayfly_enqueue(TICKER_USER_ID_ULL_HIGH, TICKER_USER_ID_LLL, 0, &mfy);
    LL_ASSERT(!ret);
}

static void ticker_op_cb(uint32_t status, void *param) {
    atomic_set(&zmk_split_esb_stats.prx_active, status == TICKER_STATUS_SUCCESS);
}

/* Controller hooks. */

/* Called once from ll_init(), in the thread that enables Bluetooth, before
 * the host can issue any HCI command; nothing else uses the thread ticker
 * user yet. */
int ull_user_init(void) {
    ull_hdr_init(&prx.ull);
    lll_hdr_init(&prx.lll, &prx);

    prx.ull.ticks_active_to_start = 0U;
    prx.ull.ticks_prepare_to_start = HAL_TICKER_US_TO_TICKS(EVENT_OVERHEAD_XTAL_US);
    prx.ull.ticks_preempt_to_start = HAL_TICKER_US_TO_TICKS(EVENT_OVERHEAD_PREEMPT_MIN_US);
    prx.ull.ticks_slot = 0U;

    atomic_set(&zmk_split_esb_stats.channel, CONFIG_ZMK_SPLIT_ESB_RF_CHANNEL);

    if (!IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_PRX_AUTOSTART)) {
        return 0;
    }

    uint32_t ticks_anchor =
        ticker_ticks_now_get() + HAL_TICKER_US_TO_TICKS(EVENT_OVERHEAD_START_US);
    uint32_t ret = ticker_start(
        TICKER_INSTANCE_ID_CTLR, TICKER_USER_ID_THREAD, TICKER_ID_USER_BASE, ticks_anchor, 0,
        HAL_TICKER_US_TO_TICKS(PRX_TICKER_INTERVAL_US), HAL_TICKER_REMAINDER(PRX_TICKER_INTERVAL_US),
        TICKER_NULL_LAZY, TICKER_NULL_SLOT, ticker_cb, &prx, ticker_op_cb, NULL);

    return (ret == TICKER_STATUS_SUCCESS || ret == TICKER_STATUS_BUSY) ? 0 : -EIO;
}

/* Only reached for rx node types the controller doesn't know; we create none. */
void rx_demux_rx_proprietary(memq_link_t *link, struct node_rx_hdr *rx, memq_link_t *tail,
                             memq_link_t **head) {
    LL_ASSERT(0);
}

/* Only reached for done events with a user extra type; ours use none. */
void ull_proprietary_done(struct node_rx_event_done *done) { LL_ASSERT(0); }

/* hci.c dispatches user rx node types here. We never send any to the host. */
int8_t hci_user_ext_get_class(struct node_rx_pdu *node_rx) { return 0; /* HCI_CLASS_NONE */ }

void hci_user_ext_encode_control(struct node_rx_pdu *node_rx, struct pdu_data *pdu_data,
                                 struct net_buf *buf) {}
