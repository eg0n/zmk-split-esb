/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/kernel.h>
#include <zmk/event_manager.h>

/* Raised on the central when a peripheral's ESB link comes up or is lost.
 * source is the ZMK split source index (peripheral id - 1). Status widgets
 * written for BLE split, which track bt_conn callbacks, can map this onto
 * their own per-peripheral state. */
struct zmk_split_esb_peripheral_link_changed {
    uint8_t source;
    bool connected;
};

ZMK_EVENT_DECLARE(zmk_split_esb_peripheral_link_changed);
