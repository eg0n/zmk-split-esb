/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

/* ZMK split central transport over the hosted ESB receiver. Peripheral id n
 * uses pipe n and is ZMK source n - 1. Commands are staged one at a time per
 * pipe as ACK payload, and released once the peripheral echoes their seq. */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/logging/log.h>

#include <zmk/split/central.h>
#include <zmk/split/transport/central.h>
#include <zmk/split/transport/types.h>

#include <zmk_split_esb/events/peripheral_link_changed.h>
#include <zmk_split_esb/prx.h>
#include <zmk_split_esb/stats.h>

#include "../phy/esb_phy.h"
#include "framing.h"

LOG_MODULE_DECLARE(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

#define PERIPHERAL_COUNT ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT
BUILD_ASSERT(PERIPHERAL_COUNT >= 1 && PERIPHERAL_COUNT <= 7,
             "ESB split supports 1 to 7 peripherals (set CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS)");

#define LINK_CHECK_MS 250
#define MAX_POSITIONS 256

struct rx_item {
    uint8_t pipe;
    uint8_t len;
    uint8_t data[ESB_PHY_MAX_PAYLOAD];
};

K_MSGQ_DEFINE(rx_q, sizeof(struct rx_item), CONFIG_ZMK_SPLIT_ESB_RX_QUEUE_SIZE, 4);

struct cmd_item {
    uint8_t pipe;
    uint8_t len;
    uint8_t data[ESB_PHY_MAX_PAYLOAD];
};

K_MSGQ_DEFINE(cmd_q, sizeof(struct cmd_item), CONFIG_ZMK_SPLIT_ESB_CMD_QUEUE_SIZE, 4);

static struct peripheral {
    int64_t last_heard_ms;
    bool connected;
    bool seq_valid;
    uint8_t last_seq;
    uint8_t cmd_seq;
    uint8_t staged_seq; /* 0: nothing staged */
    uint8_t session;    /* 0: never heard */
    ATOMIC_DEFINE(pressed, MAX_POSITIONS);
} peripherals[PERIPHERAL_COUNT];

static bool is_enabled;
static zmk_split_transport_central_status_changed_cb_t transport_status_cb;

static void rx_work_cb(struct k_work *work);
static void link_check_work_cb(struct k_work *work);

static K_WORK_DEFINE(rx_work, rx_work_cb);
static K_WORK_DELAYABLE_DEFINE(link_check_work, link_check_work_cb);

static struct zmk_split_transport_status get_status(void);

static const struct zmk_split_transport_central_api api;

ZMK_SPLIT_TRANSPORT_CENTRAL_REGISTER(esb_central, &api, CONFIG_ZMK_SPLIT_ESB_PRIORITY);

static void raise_link_changed(uint8_t source, bool connected) {
    raise_zmk_split_esb_peripheral_link_changed(
        (struct zmk_split_esb_peripheral_link_changed){.source = source, .connected = connected});
}

static void notify_status(void) {
    int up = 0;
    for (int i = 0; i < PERIPHERAL_COUNT; i++) {
        up += peripherals[i].connected;
    }
    atomic_set(&zmk_split_esb_stats.link_up, up);

    if (transport_status_cb) {
        transport_status_cb(&esb_central, get_status());
    }
}

/* Called from the radio ISR. */
static void on_rx(uint8_t pipe, const uint8_t *data, uint8_t len) {
    if (pipe < 1 || pipe > PERIPHERAL_COUNT) {
        return;
    }

    struct rx_item item = {.pipe = pipe, .len = len};
    memcpy(item.data, data, len);

    if (k_msgq_put(&rx_q, &item, K_NO_WAIT) == 0) {
        k_work_submit(&rx_work);
    }
}

static void stage_next_command(void) {
    struct cmd_item item;

    while (k_msgq_peek(&cmd_q, &item) == 0) {
        struct peripheral *p = &peripherals[item.pipe - 1];
        if (p->staged_seq) {
            return;
        }

        if (zmk_split_esb_prx_stage_ack(item.pipe, item.data, item.len) == 0) {
            p->staged_seq = item.data[0];
        }
        k_msgq_get(&cmd_q, &item, K_NO_WAIT);
    }
}

static void deliver(uint8_t source, struct zmk_split_transport_peripheral_event ev) {
    if (ev.type == ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT &&
        ev.data.key_position_event.position < MAX_POSITIONS) {
        if (ev.data.key_position_event.pressed) {
            atomic_set_bit(peripherals[source].pressed, ev.data.key_position_event.position);
        } else {
            atomic_clear_bit(peripherals[source].pressed, ev.data.key_position_event.position);
        }
    }

    if (is_enabled) {
        zmk_split_transport_central_peripheral_event_handler(&esb_central, source, ev);
    }
}

static void release_keys(uint8_t source);

static void process(const struct rx_item *item) {
    uint8_t source = item->pipe - 1;
    struct peripheral *p = &peripherals[source];

    p->last_heard_ms = k_uptime_get();
    if (!p->connected) {
        p->connected = true;
        LOG_INF("Peripheral %d connected", source);
        raise_link_changed(source, true);
        notify_status();
    }

    if (item->len < ESB_FRAME_PERIPHERAL_HEADER) {
        return;
    }

    /* Restarted, perhaps too quickly for the link to time out: nothing it
     * had pressed is still pressed, and its seq has started over. */
    uint8_t session = item->data[2];
    if (session != p->session) {
        if (p->session) {
            LOG_INF("Peripheral %d restarted", source);
            release_keys(source);
        }
        p->session = session;
        p->seq_valid = false;
    }

    if (p->staged_seq && item->data[1] == p->staged_seq) {
        zmk_split_esb_prx_clear_ack(item->pipe);
        p->staged_seq = 0;
        stage_next_command();
    }

    /* A packet resent after running out of retransmissions keeps its seq. */
    if (p->seq_valid && item->data[0] == p->last_seq) {
        return;
    }
    p->last_seq = item->data[0];
    p->seq_valid = true;

    uint8_t off = ESB_FRAME_PERIPHERAL_HEADER;
    while (off < item->len) {
        struct zmk_split_transport_peripheral_event ev = {.type = item->data[off++]};
        ssize_t size = esb_frame_event_data_size(ev.type);
        if (size < 0 || off + size > item->len) {
            LOG_WRN("Bad event (type %d) from peripheral %d", ev.type, source);
            return;
        }
        memcpy(&ev.data, &item->data[off], size);
        off += size;

        deliver(source, ev);
    }
}

static void rx_work_cb(struct k_work *work) {
    struct rx_item item;

    while (k_msgq_get(&rx_q, &item, K_NO_WAIT) == 0) {
        process(&item);
    }
}

/* Release whatever a lost peripheral was holding down. */
static void release_keys(uint8_t source) {
    struct peripheral *p = &peripherals[source];

    for (int pos = 0; pos < MAX_POSITIONS; pos++) {
        if (atomic_test_bit(p->pressed, pos)) {
            struct zmk_split_transport_peripheral_event ev = {
                .type = ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT,
                .data.key_position_event = {.position = pos, .pressed = 0},
            };
            deliver(source, ev);
        }
    }
}

static void link_check_work_cb(struct k_work *work) {
    int64_t now = k_uptime_get();

    for (int i = 0; i < PERIPHERAL_COUNT; i++) {
        struct peripheral *p = &peripherals[i];
        if (p->connected && now - p->last_heard_ms > CONFIG_ZMK_SPLIT_ESB_LINK_TIMEOUT_MS) {
            p->connected = false;
            LOG_INF("Peripheral %d lost", i);
            release_keys(i);
            raise_link_changed(i, false);
            notify_status();
        }
    }

    k_work_reschedule(&link_check_work, K_MSEC(LINK_CHECK_MS));
}

static int send_command(uint8_t source, struct zmk_split_transport_central_command cmd) {
    if (source >= PERIPHERAL_COUNT) {
        return -EINVAL;
    }
    if (cmd.type == ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_POLL_EVENTS) {
        return 0;
    }

    ssize_t size = esb_frame_command_data_size(cmd.type);
    if (size < 0) {
        return size;
    }

    struct peripheral *p = &peripherals[source];
    if (++p->cmd_seq == 0) {
        p->cmd_seq = 1;
    }

    struct cmd_item item = {.pipe = source + 1, .len = ESB_FRAME_COMMAND_HEADER + 1 + size};
    BUILD_ASSERT(ESB_FRAME_COMMAND_HEADER + 1 + ESB_FRAME_INVOKE_BEHAVIOR_SIZE <= ESB_PHY_MAX_PAYLOAD,
                 "Commands must fit one ACK payload");
    item.data[0] = p->cmd_seq;
    item.data[1] = cmd.type;
    memcpy(&item.data[2], &cmd.data, size);

    int err = k_msgq_put(&cmd_q, &item, K_NO_WAIT);
    if (err) {
        LOG_WRN("ESB command queue full, dropping command type %d", cmd.type);
        return -ENOSPC;
    }

    stage_next_command();
    return 0;
}

static int get_available_source_ids(uint8_t *sources) {
    int count = 0;
    for (int i = 0; i < PERIPHERAL_COUNT; i++) {
        if (peripherals[i].connected) {
            sources[count++] = i;
        }
    }
    return count;
}

static int set_enabled(bool enabled) {
    is_enabled = enabled;
    return 0;
}

static int set_status_callback(zmk_split_transport_central_status_changed_cb_t cb) {
    transport_status_cb = cb;
    return 0;
}

static struct zmk_split_transport_status get_status(void) {
    int up = 0;
    for (int i = 0; i < PERIPHERAL_COUNT; i++) {
        up += peripherals[i].connected;
    }

    return (struct zmk_split_transport_status){
        /* Always available, so ZMK keeps choosing it over BLE split. */
        .available = true,
        .enabled = is_enabled,
        .connections = up == 0                  ? ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_DISCONNECTED
                       : up == PERIPHERAL_COUNT ? ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_ALL_CONNECTED
                                                : ZMK_SPLIT_TRANSPORT_CONNECTIONS_STATUS_SOME_CONNECTED,
    };
}

static const struct zmk_split_transport_central_api api = {
    .send_command = send_command,
    .get_available_source_ids = get_available_source_ids,
    .set_enabled = set_enabled,
    .set_status_callback = set_status_callback,
    .get_status = get_status,
};

static int esb_central_init(void) {
    zmk_split_esb_prx_set_rx_cb(on_rx);
    k_work_schedule(&link_check_work, K_MSEC(LINK_CHECK_MS));
    return 0;
}

SYS_INIT(esb_central_init, APPLICATION, ZMK_SPLIT_ESB_EARLY_INIT_PRIORITY);
