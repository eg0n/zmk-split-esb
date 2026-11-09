/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

/* ZMK split peripheral transport over the standalone ESB transmitter. Events
 * are packed several to a packet; a packet that runs out of retransmissions
 * is resent unchanged (same seq) until it gets through, so key events aren't
 * dropped while the queue has room. Commands arrive in ACK payloads. */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>
#include <hal/nrf_rng.h>

#include <zmk/split/transport/peripheral.h>
#include <zmk/split/transport/types.h>

#include <zmk_split_esb/ptx.h>
#include <zmk_split_esb/stats.h>

#include "../phy/esb_phy.h"
#include "framing.h"

LOG_MODULE_DECLARE(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

#define RESEND_DELAY_MIN_MS 2
#define RESEND_DELAY_MAX_MS 100

K_MSGQ_DEFINE(event_q, sizeof(struct zmk_split_transport_peripheral_event),
              CONFIG_ZMK_SPLIT_ESB_EVENT_QUEUE_SIZE, 4);

struct cmd_item {
    uint8_t len;
    uint8_t data[ESB_PHY_MAX_PAYLOAD];
};

K_MSGQ_DEFINE(cmd_q, sizeof(struct cmd_item), 4, 4);

/* The packet being sent, owned by the work items below. */
static uint8_t pkt[ESB_PHY_MAX_PAYLOAD];
static uint8_t pkt_len;
static bool in_flight;
static uint8_t seq;
static uint8_t resend_delay_ms;

/* Set from the radio ISR, consumed by done_work. */
static atomic_t result_acked;

static uint8_t session;
static uint8_t last_cmd_seq;
static atomic_t keepalive_pending;
static int64_t last_ack_ms;
static bool connected;
static bool is_enabled;

static zmk_split_transport_peripheral_status_changed_cb_t transport_status_cb;

static void send_work_cb(struct k_work *work);
static void done_work_cb(struct k_work *work);
static void resend_work_cb(struct k_work *work);
static void keepalive_work_cb(struct k_work *work);

static K_WORK_DEFINE(send_work, send_work_cb);
static K_WORK_DEFINE(done_work, done_work_cb);
static K_WORK_DELAYABLE_DEFINE(resend_work, resend_work_cb);
static K_WORK_DELAYABLE_DEFINE(keepalive_work, keepalive_work_cb);

static struct zmk_split_transport_status get_status(void);

static const struct zmk_split_transport_peripheral_api api;

ZMK_SPLIT_TRANSPORT_PERIPHERAL_REGISTER(esb_peripheral, &api, CONFIG_ZMK_SPLIT_ESB_PRIORITY);

static void set_connected(bool now_connected) {
    if (connected == now_connected) {
        return;
    }

    connected = now_connected;
    atomic_set(&zmk_split_esb_stats.link_up, connected);
    LOG_INF("ESB link %s", connected ? "up" : "down");

    if (transport_status_cb) {
        transport_status_cb(&esb_peripheral, get_status());
    }
}

static void transmit(void) {
    int err = zmk_split_esb_ptx_send(pkt, pkt_len);
    if (err == -EBUSY) {
        k_work_reschedule(&resend_work, K_MSEC(1));
    } else if (err) {
        LOG_ERR("ESB send failed (%d)", err);
        in_flight = false;
    }
}

static void send_work_cb(struct k_work *work) {
    if (!is_enabled || in_flight) {
        return;
    }

    struct zmk_split_transport_peripheral_event ev;
    uint8_t len = ESB_FRAME_PERIPHERAL_HEADER;

    while (k_msgq_peek(&event_q, &ev) == 0) {
        ssize_t size = esb_frame_event_data_size(ev.type);
        if (size < 0) {
            k_msgq_get(&event_q, &ev, K_NO_WAIT);
            continue;
        }
        if (len + 1 + size > sizeof(pkt)) {
            break;
        }

        pkt[len++] = ev.type;
        memcpy(&pkt[len], &ev.data, size);
        len += size;
        k_msgq_get(&event_q, &ev, K_NO_WAIT);
    }

    if (len == ESB_FRAME_PERIPHERAL_HEADER && !atomic_clear(&keepalive_pending)) {
        return;
    }

    pkt[0] = ++seq;
    pkt[1] = last_cmd_seq;
    pkt[2] = session;
    pkt_len = len;
    in_flight = true;
    resend_delay_ms = RESEND_DELAY_MIN_MS;
    transmit();
}

static void resend_work_cb(struct k_work *work) {
    if (in_flight) {
        pkt[1] = last_cmd_seq;
        transmit();
    }
}

static void handle_commands(void) {
    struct cmd_item item;

    while (k_msgq_get(&cmd_q, &item, K_NO_WAIT) == 0) {
        if (item.len < ESB_FRAME_COMMAND_HEADER + 1 || item.data[0] == last_cmd_seq) {
            continue;
        }
        last_cmd_seq = item.data[0];

        struct zmk_split_transport_central_command cmd = {.type = item.data[1]};
        ssize_t size = esb_frame_command_data_size(cmd.type);
        if (size < 0 || ESB_FRAME_COMMAND_HEADER + 1 + size > item.len) {
            LOG_WRN("Bad command (type %d, len %d)", item.data[1], item.len);
            continue;
        }
        memcpy(&cmd.data, &item.data[2], size);

        zmk_split_transport_peripheral_command_handler(&esb_peripheral, cmd);

        /* Echo the seq soon so the central can stage its next command. */
        atomic_set(&keepalive_pending, 1);
    }
}

static void done_work_cb(struct k_work *work) {
    handle_commands();

    if (atomic_get(&result_acked)) {
        in_flight = false;
        last_ack_ms = k_uptime_get();
        set_connected(true);
        k_work_submit(&send_work);
        return;
    }

    /* Ran out of retransmissions: the central is busy or gone. Back off and
     * resend the same packet. */
    if (k_uptime_get() - last_ack_ms > CONFIG_ZMK_SPLIT_ESB_LINK_TIMEOUT_MS) {
        set_connected(false);
    }
    k_work_reschedule(&resend_work, K_MSEC(resend_delay_ms));
    resend_delay_ms = MIN(resend_delay_ms * 2, RESEND_DELAY_MAX_MS);
}

static void ptx_done(bool acked, const uint8_t *ack, uint8_t ack_len) {
    if (acked && ack_len > ESB_FRAME_COMMAND_HEADER && ack[0] != last_cmd_seq) {
        struct cmd_item item = {.len = ack_len};
        memcpy(item.data, ack, ack_len);
        k_msgq_put(&cmd_q, &item, K_NO_WAIT);
    }

    atomic_set(&result_acked, acked);
    k_work_submit(&done_work);
}

static void keepalive_work_cb(struct k_work *work) {
    atomic_set(&keepalive_pending, 1);
    k_work_submit(&send_work);
    k_work_reschedule(&keepalive_work, K_MSEC(CONFIG_ZMK_SPLIT_ESB_KEEPALIVE_MS));
}

static int report_event(const struct zmk_split_transport_peripheral_event *event) {
    int err = k_msgq_put(&event_q, event, K_NO_WAIT);
    if (err) {
        LOG_WRN("ESB event queue full, dropping event type %d", event->type);
        return -ENOMEM;
    }

    k_work_submit(&send_work);
    return 0;
}

static int set_enabled(bool enabled) {
    is_enabled = enabled;

    if (enabled) {
        k_work_reschedule(&keepalive_work, K_NO_WAIT);
    } else {
        k_work_cancel_delayable(&keepalive_work);
    }

    return 0;
}

static int set_status_callback(zmk_split_transport_peripheral_status_changed_cb_t cb) {
    transport_status_cb = cb;
    return 0;
}

static struct zmk_split_transport_status get_status(void) {
    return (struct zmk_split_transport_status){
        .available = true,
        .enabled = is_enabled,
        .connections = connected ? ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_ALL_CONNECTED
                                 : ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_DISCONNECTED,
    };
}

static const struct zmk_split_transport_peripheral_api api = {
    .report_event = report_event,
    .set_enabled = set_enabled,
    .get_status = get_status,
    .set_status_callback = set_status_callback,
};

/* One byte from the hardware RNG; BLE is off, so nothing else is using it. */
static uint8_t random_session(void) {
    uint8_t value;

    nrf_rng_event_clear(NRF_RNG, NRF_RNG_EVENT_VALRDY);
    nrf_rng_task_trigger(NRF_RNG, NRF_RNG_TASK_START);
    do {
        while (!nrf_rng_event_check(NRF_RNG, NRF_RNG_EVENT_VALRDY)) {
        }
        nrf_rng_event_clear(NRF_RNG, NRF_RNG_EVENT_VALRDY);
        value = nrf_rng_random_value_get(NRF_RNG);
    } while (value == 0);
    nrf_rng_task_trigger(NRF_RNG, NRF_RNG_TASK_STOP);

    return value;
}

static int esb_peripheral_init(void) {
    session = random_session();
    zmk_split_esb_ptx_set_done_cb(ptx_done);
    return 0;
}

SYS_INIT(esb_peripheral_init, APPLICATION, ZMK_SPLIT_ESB_EARLY_INIT_PRIORITY);
