/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

/* Battery sensor for a central without a battery (a dongle): state of charge
 * is the lowest level any peripheral has reported, so with zmk,battery
 * pointing here, hosts see the lowest half. */

#define DT_DRV_COMPAT zmk_split_peripheral_battery

#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>

#include <zmk/split/central.h>

struct peripheral_battery_data {
    uint8_t level; /* 0: no peripheral has reported yet */
};

static int peripheral_battery_sample_fetch(const struct device *dev, enum sensor_channel chan) {
    struct peripheral_battery_data *data = dev->data;
    uint8_t lowest = 0;

    for (uint8_t i = 0; i < ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT; i++) {
        uint8_t level;
        if (zmk_split_central_get_peripheral_battery_level(i, &level) == 0 && level > 0 &&
            (lowest == 0 || level < lowest)) {
            lowest = level;
        }
    }

    data->level = lowest;
    return 0;
}

static int peripheral_battery_channel_get(const struct device *dev, enum sensor_channel chan,
                                          struct sensor_value *val) {
    struct peripheral_battery_data *data = dev->data;

    if (chan != SENSOR_CHAN_GAUGE_STATE_OF_CHARGE) {
        return -ENOTSUP;
    }
    if (data->level == 0) {
        return -ENODATA;
    }

    val->val1 = data->level;
    val->val2 = 0;
    return 0;
}

static const struct sensor_driver_api peripheral_battery_api = {
    .sample_fetch = peripheral_battery_sample_fetch,
    .channel_get = peripheral_battery_channel_get,
};

#define PERIPHERAL_BATTERY_INST(n)                                                                 \
    static struct peripheral_battery_data peripheral_battery_data_##n;                             \
    SENSOR_DEVICE_DT_INST_DEFINE(n, NULL, NULL, &peripheral_battery_data_##n, NULL, POST_KERNEL,   \
                                 CONFIG_SENSOR_INIT_PRIORITY, &peripheral_battery_api);

DT_INST_FOREACH_STATUS_OKAY(PERIPHERAL_BATTERY_INST)
