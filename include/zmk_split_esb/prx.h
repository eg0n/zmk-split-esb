/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

/* Called from the radio ISR for every new (non-duplicate) packet. It must
 * copy what it needs and return quickly. */
typedef void (*zmk_split_esb_prx_rx_cb_t)(uint8_t pipe, const uint8_t *data, uint8_t len);

void zmk_split_esb_prx_set_rx_cb(zmk_split_esb_prx_rx_cb_t cb);
