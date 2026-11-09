/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Called from the radio ISR for every new (non-duplicate) packet. It must
 * copy what it needs and return quickly. */
typedef void (*zmk_split_esb_prx_rx_cb_t)(uint8_t pipe, const uint8_t *data, uint8_t len);

void zmk_split_esb_prx_set_rx_cb(zmk_split_esb_prx_rx_cb_t cb);

/* Payload to attach to every ACK sent on this pipe until it is cleared.
 * Returns -EBUSY if one is already staged. Call from thread context. */
int zmk_split_esb_prx_stage_ack(uint8_t pipe, const uint8_t *data, uint8_t len);

void zmk_split_esb_prx_clear_ack(uint8_t pipe);

/* Listen in short windows instead of continuously. Takes effect within one
 * receiver period. */
void zmk_split_esb_prx_set_low_power(bool enabled);
