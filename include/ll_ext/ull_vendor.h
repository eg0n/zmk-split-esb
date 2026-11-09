/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

/* Included by the Zephyr controller (ull.c, ull_conn.c, ull_llcp_conn_upd.c)
 * when CONFIG_BT_CTLR_USER_EXT is set. Declares the hooks the controller calls
 * into; they are implemented in src/ll_ext/prx_hosted.c. */

#pragma once

/* memq.h has no include guard, so name the struct behind memq_link_t. */
struct _memq_link;
struct node_rx_hdr;
struct node_rx_event_done;

int ull_user_init(void);
void rx_demux_rx_proprietary(struct _memq_link *link, struct node_rx_hdr *rx,
                             struct _memq_link *tail, struct _memq_link **head);
void ull_proprietary_done(struct node_rx_event_done *done);
