/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Called from the radio ISR once a packet is ACKed or given up on. */
typedef void (*zmk_split_esb_ptx_done_cb_t)(bool acked);

void zmk_split_esb_ptx_set_done_cb(zmk_split_esb_ptx_done_cb_t cb);

/* Send one packet, retransmitting until ACKed or out of attempts. Returns
 * -EBUSY while the previous packet is still in flight. */
int zmk_split_esb_ptx_send(const uint8_t *data, uint8_t len);
