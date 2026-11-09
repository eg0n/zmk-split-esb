/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

/* ESB receiver hosted inside the Zephyr BLE controller, through the
 * controller's CONFIG_BT_CTLR_USER_EXT hooks. Phase 0: the hooks are compiled
 * in but nothing is scheduled, so the controller behaves as without them. */

#include <zephyr/kernel.h>

#include "util/memq.h"
#include "ull_vendor.h"

struct node_rx_pdu;
struct pdu_data;
struct net_buf;
#include "hci/hci_user_ext.h"

void bt_ctlr_assert_handle(char *file, uint32_t line);

int ull_user_init(void) { return 0; }

/* Only reached for rx node types the controller doesn't know; we create none. */
void rx_demux_rx_proprietary(memq_link_t *link, struct node_rx_hdr *rx, memq_link_t *tail,
                             memq_link_t **head) {
    bt_ctlr_assert_handle(__FILE__, __LINE__);
}

/* Only reached for done events of user event types; we create none yet. */
void ull_proprietary_done(struct node_rx_event_done *done) {
    bt_ctlr_assert_handle(__FILE__, __LINE__);
}

/* hci.c dispatches user rx node types here. We never send any to the host. */
int8_t hci_user_ext_get_class(struct node_rx_pdu *node_rx) { return 0; /* HCI_CLASS_NONE */ }

void hci_user_ext_encode_control(struct node_rx_pdu *node_rx, struct pdu_data *pdu_data,
                                 struct net_buf *buf) {}
