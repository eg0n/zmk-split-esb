/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#pragma once

/* Peripheral to central: [seq][cmd_ack][session][type][data]...; no events
 * means keepalive. seq is kept when a packet is resent, cmd_ack echoes the
 * last command's seq, and session is random per boot (a change = restart).
 * Central to peripheral, as an ACK payload: [seq][type][data]. */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include <zmk/split/transport/types.h>

#define ESB_FRAME_PERIPHERAL_HEADER 3
#define ESB_FRAME_COMMAND_HEADER 1

static inline ssize_t esb_frame_event_data_size(enum zmk_split_transport_peripheral_event_type type) {
    const struct zmk_split_transport_peripheral_event *ev = NULL;

    switch (type) {
    case ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_KEY_POSITION_EVENT:
        return sizeof(ev->data.key_position_event);
    case ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_SENSOR_EVENT:
        return sizeof(ev->data.sensor_event);
    case ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_INPUT_EVENT:
        return sizeof(ev->data.input_event);
    case ZMK_SPLIT_TRANSPORT_PERIPHERAL_EVENT_TYPE_BATTERY_EVENT:
        return sizeof(ev->data.battery_event);
    default:
        return -ENOTSUP;
    }
}

/* Up to and including the last member: the inner structs aren't packed, and
 * invoke_behavior's trailing padding would push it past one payload. */
#define ESB_FRAME_SIZE_THROUGH(member, last)                                                       \
    (offsetof(__typeof__(member), last) + sizeof((member).last))

#define ESB_FRAME_INVOKE_BEHAVIOR_SIZE                                                             \
    ESB_FRAME_SIZE_THROUGH(((struct zmk_split_transport_central_command *)0)->data.invoke_behavior, \
                           state)

static inline ssize_t esb_frame_command_data_size(enum zmk_split_transport_central_command_type type) {
    const struct zmk_split_transport_central_command *cmd = NULL;

    switch (type) {
    case ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_POLL_EVENTS:
        return 0;
    case ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_INVOKE_BEHAVIOR:
        return ESB_FRAME_INVOKE_BEHAVIOR_SIZE;
    case ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_SET_PHYSICAL_LAYOUT:
        return sizeof(cmd->data.set_physical_layout);
    case ZMK_SPLIT_TRANSPORT_CENTRAL_CMD_TYPE_SET_HID_INDICATORS:
        return sizeof(cmd->data.set_hid_indicators);
    default:
        return -ENOTSUP;
    }
}
