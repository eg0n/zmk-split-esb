/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <hal/nrf_radio.h>

/* Enhanced ShockBurst on-air format, as Nordic's ESB library uses it with
 * dynamic payload length: 2 Mbit/s, 5-byte addresses, 6-bit length, 3-bit
 * packet control field (2-bit PID, no-ACK), 16-bit CRC. In RAM a packet is
 * [length][PID << 1 | no-ACK][payload...]. */

#define ESB_PHY_MAX_PAYLOAD 32
#define ESB_PHY_BUF_SIZE (2 + ESB_PHY_MAX_PAYLOAD)

#define ESB_PHY_LEN(buf) ((buf)[0])
#define ESB_PHY_PID(buf) ((buf)[1] >> 1)
#define ESB_PHY_NOACK(buf) ((buf)[1] & 1)
#define ESB_PHY_PAYLOAD(buf) (&(buf)[2])

/* Shorts every ESB state uses: start once ramped up, disable once done. */
#define ESB_PHY_SHORTS_COMMON (NRF_RADIO_SHORT_READY_START_MASK | NRF_RADIO_SHORT_END_DISABLE_MASK)

/* Pipe 0 is reserved for future pairing; peripherals use pipes 1 to 7. */
#define ESB_PHY_PERIPHERAL_PIPES 0xFE

/*
 * Program everything except SHORTS, PACKETPTR, TXADDRESS and RXADDRESSES.
 * The radio must be disabled.
 */
void esb_phy_configure(void);

static inline void esb_phy_set_header(uint8_t *buf, uint8_t len, uint8_t pid, bool noack) {
    buf[0] = len;
    buf[1] = (pid << 1) | (noack ? 1 : 0);
}
