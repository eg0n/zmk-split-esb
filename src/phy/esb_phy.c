/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <cmsis_core.h>

#include "esb_phy.h"

/* ESB sends address bytes most significant bit first, while the RADIO sends
 * least significant first, so every byte is bit-reversed. */
static uint32_t bytewise_bit_swap(uint32_t word) { return __REV(__RBIT(word)); }

/* Pipes 1-7 share BASE1, so it decides which keyboard a packet belongs to.
 * With a network name set it is an FNV-1a hash of the name. Bytes like
 * 0x00, 0xFF, 0x55 and 0xAA make an address look like noise or preamble,
 * so those get nudged. */
static uint32_t base_addr1(void) {
    static uint32_t cached;
    const char *name = CONFIG_ZMK_SPLIT_ESB_NETWORK;

    if (name[0] == '\0') {
        return CONFIG_ZMK_SPLIT_ESB_BASE_ADDR1;
    }
    if (cached) {
        return cached;
    }

    uint32_t hash = 2166136261u;
    for (const char *c = name; *c; c++) {
        hash ^= (uint8_t)*c;
        hash *= 16777619u;
    }

    for (int shift = 0; shift < 32; shift += 8) {
        uint8_t byte = hash >> shift;
        if (byte == 0x00 || byte == 0xFF || byte == 0x55 || byte == 0xAA) {
            hash ^= 0x5Au << shift;
        }
    }

    cached = hash;
    return hash;
}

static nrf_radio_txpower_t tx_power(void) {
    switch (CONFIG_ZMK_SPLIT_ESB_TX_POWER_DBM) {
#if defined(RADIO_TXPOWER_TXPOWER_Pos8dBm)
    case 8:
        return NRF_RADIO_TXPOWER_POS8DBM;
#endif
#if defined(RADIO_TXPOWER_TXPOWER_Pos4dBm)
    case 4:
        return NRF_RADIO_TXPOWER_POS4DBM;
#endif
    case -4:
        return NRF_RADIO_TXPOWER_NEG4DBM;
    case -8:
        return NRF_RADIO_TXPOWER_NEG8DBM;
    default:
        return NRF_RADIO_TXPOWER_0DBM;
    }
}

void esb_phy_configure(void) {
    NRF_RADIO->MODE = RADIO_MODE_MODE_Nrf_2Mbit << RADIO_MODE_MODE_Pos;
    /* Default (slow, ~130 us) ramp-up on both ends. It gives the receiver's
     * ISR time to point the radio at the ACK before transmission starts. */
    NRF_RADIO->MODECNF0 = (RADIO_MODECNF0_RU_Default << RADIO_MODECNF0_RU_Pos) |
                          (RADIO_MODECNF0_DTX_Center << RADIO_MODECNF0_DTX_Pos);

    NRF_RADIO->PCNF0 = (6 << RADIO_PCNF0_LFLEN_Pos) | (0 << RADIO_PCNF0_S0LEN_Pos) |
                       (3 << RADIO_PCNF0_S1LEN_Pos);
    NRF_RADIO->PCNF1 = (RADIO_PCNF1_WHITEEN_Disabled << RADIO_PCNF1_WHITEEN_Pos) |
                       (RADIO_PCNF1_ENDIAN_Big << RADIO_PCNF1_ENDIAN_Pos) |
                       (4 << RADIO_PCNF1_BALEN_Pos) | (0 << RADIO_PCNF1_STATLEN_Pos) |
                       (ESB_PHY_MAX_PAYLOAD << RADIO_PCNF1_MAXLEN_Pos);

    NRF_RADIO->BASE0 = bytewise_bit_swap(CONFIG_ZMK_SPLIT_ESB_BASE_ADDR0);
    NRF_RADIO->BASE1 = bytewise_bit_swap(base_addr1());
    NRF_RADIO->PREFIX0 = bytewise_bit_swap(CONFIG_ZMK_SPLIT_ESB_ADDR_PREFIX0);
    NRF_RADIO->PREFIX1 = bytewise_bit_swap(CONFIG_ZMK_SPLIT_ESB_ADDR_PREFIX1);

    NRF_RADIO->CRCCNF = (RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos) |
                        (RADIO_CRCCNF_SKIPADDR_Include << RADIO_CRCCNF_SKIPADDR_Pos);
    NRF_RADIO->CRCINIT = 0xFFFF;
    NRF_RADIO->CRCPOLY = 0x11021;

    nrf_radio_txpower_set(NRF_RADIO, tx_power());
    NRF_RADIO->FREQUENCY = CONFIG_ZMK_SPLIT_ESB_RF_CHANNEL;
}
