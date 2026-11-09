/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

/* Listen continuously while the keyboard is in use, in windows when idle. */

#include <zephyr/kernel.h>

#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>

#include <zmk_split_esb/prx.h>

static int activity_listener(const zmk_event_t *eh) {
    const struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);
    if (ev) {
        zmk_split_esb_prx_set_low_power(ev->state != ZMK_ACTIVITY_ACTIVE);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(esb_prx_power, activity_listener);
ZMK_SUBSCRIPTION(esb_prx_power, zmk_activity_state_changed);
