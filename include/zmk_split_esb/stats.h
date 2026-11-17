/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/sys/atomic.h>

/* Below CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, where ZMK's split code selects
 * and enables transports, and before Bluetooth starts the receiver. */
#define ZMK_SPLIT_ESB_EARLY_INIT_PRIORITY 30

/* Counters kept by the radio code and shown on the debug screen. Writers may
 * run in the radio ISR, so every field is atomic. */
struct zmk_split_esb_stats {
    atomic_t prx_active;
    atomic_t prx_low_power;
    atomic_t link_up; /* peripherals heard recently (central), or 0/1 (peripheral) */
    atomic_t prx_windows;
    atomic_t prx_preempts;
    atomic_t rx_ok;
    atomic_t rx_crc_err;
    atomic_t ack_payloads;
    atomic_t tx_sent;
    atomic_t tx_acked;
    atomic_t tx_retransmits;
    atomic_t tx_failed;
    atomic_t ch_hops;
    atomic_t channel;
    atomic_t last_rx_ms;
    atomic_t last_rtt_us;
    atomic_t key_lat_last_us; /* peripheral: key event queued until ACKed */
    atomic_t key_lat_max_us;
    atomic_t key_lat_sum_us;
    atomic_t key_lat_count;
    atomic_t rx_rssi[8]; /* central, per pipe: smoothed dBm of good packets, 0 until one arrives */
};

extern struct zmk_split_esb_stats zmk_split_esb_stats;

/* Signal strength of a peripheral's packets at the central, in dBm; 0 until
 * one arrives. source is the ZMK split source index (pipe - 1). */
static inline int zmk_split_esb_peripheral_rssi(uint8_t source) {
    return source < 7 ? (int)atomic_get(&zmk_split_esb_stats.rx_rssi[source + 1]) : 0;
}

#define ZMK_SPLIT_ESB_ASSERT_FILE_LEN 24

struct zmk_split_esb_assert_record {
    uint32_t count;
    uint32_t line;
    char file[ZMK_SPLIT_ESB_ASSERT_FILE_LEN];
};

/* The assert recorded before the most recent reboot, or NULL if none. */
const struct zmk_split_esb_assert_record *zmk_split_esb_last_assert(void);
