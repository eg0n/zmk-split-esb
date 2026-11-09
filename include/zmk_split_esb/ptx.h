/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Called from the radio ISR once a packet is ACKed or given up on. When
 * acked, ack/ack_len is the ACK's payload (ack_len may be 0); copy it out
 * before returning. */
typedef void (*zmk_split_esb_ptx_done_cb_t)(bool acked, const uint8_t *ack, uint8_t ack_len);

void zmk_split_esb_ptx_set_done_cb(zmk_split_esb_ptx_done_cb_t cb);

/* Send one packet, retransmitting until ACKed or out of attempts. Returns
 * -EBUSY while the previous packet is still in flight. */
int zmk_split_esb_ptx_send(const uint8_t *data, uint8_t len);
