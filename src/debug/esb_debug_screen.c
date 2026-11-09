/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

/* nice!view status screen showing ESB link counters. The 160x68 panel is read
 * in portrait, so like ZMK's nice_view widget this draws into three 68x68
 * canvases and rotates each; only 24 rows of the bottom one are on screen. */

#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/drivers/hwinfo.h>
#include <lvgl.h>

#include <zmk/display.h>
#include <zmk_split_esb/stats.h>

#if IS_ENABLED(CONFIG_ZMK_BLE)
#include <zmk/ble.h>
#endif
#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
#include <zmk/battery.h>
#endif
#if IS_ENABLED(CONFIG_ZMK_SPLIT_BLE) && !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/split/bluetooth/peripheral.h>
#endif

#define CANVAS_SIZE 68
#define CANVAS_COLOR_FORMAT LV_COLOR_FORMAT_L8 /* smallest type lv_draw_sw_rotate supports */
#define CANVAS_BUF_SIZE                                                                            \
    LV_CANVAS_BUF_SIZE(CANVAS_SIZE, CANVAS_SIZE, LV_COLOR_FORMAT_GET_BPP(CANVAS_COLOR_FORMAT),     \
                       LV_DRAW_BUF_STRIDE_ALIGN)

#define LINE_HEIGHT 8 /* unscii_8 is an 8x8 monospace font: 8 columns fit in 68 px */
#define LINE_LEN 8
#define CANVAS_COUNT 3

static const uint8_t lines_per_canvas[CANVAS_COUNT] = {8, 8, 3};
#define LINE_COUNT (8 + 8 + 3)

static uint8_t cbuf[CANVAS_COUNT][CANVAS_BUF_SIZE];
static uint8_t rotate_buf[CANVAS_BUF_SIZE];
static lv_obj_t *canvases[CANVAS_COUNT];
static char lines[LINE_COUNT][LINE_LEN + 1];

static uint32_t reset_cause;

static void rotate_canvas(lv_obj_t *canvas) {
    uint8_t *buf = lv_canvas_get_draw_buf(canvas)->data;
    memcpy(rotate_buf, buf, sizeof(rotate_buf));

    const uint32_t stride = lv_draw_buf_width_to_stride(CANVAS_SIZE, CANVAS_COLOR_FORMAT);
    lv_draw_sw_rotate(rotate_buf, buf, CANVAS_SIZE, CANVAS_SIZE, stride, stride,
                      LV_DISPLAY_ROTATION_270, CANVAS_COLOR_FORMAT);
}

/* "LBL12345", switching to thousands once the value no longer fits. */
static void fmt_count(char *line, const char *label, uint32_t value) {
    if (value < 100000) {
        snprintf(line, LINE_LEN + 1, "%-3s%5u", label, value);
    } else {
        snprintf(line, LINE_LEN + 1, "%-3s%4uk", label, MIN(value / 1000, 9999));
    }
}

static const char *reset_cause_str(uint32_t cause) {
    if (cause & RESET_SOFTWARE) {
        return "SOFT";
    }
    if (cause & RESET_WATCHDOG) {
        return "WDT";
    }
    if (cause & RESET_CPU_LOCKUP) {
        return "LOCK";
    }
    if (cause & RESET_BROWNOUT) {
        return "BOR";
    }
    if (cause & RESET_PIN) {
        return "PIN";
    }
    if (cause & RESET_POR) {
        return "POR";
    }
    return cause ? "?" : "-";
}

static uint32_t get(atomic_t *v) { return (uint32_t)atomic_get(v); }

static void fill_lines(void) {
    struct zmk_split_esb_stats *s = &zmk_split_esb_stats;
    int n = 0;

    memset(lines, 0, sizeof(lines));

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    snprintf(lines[n++], LINE_LEN + 1, "PRX %s", get(&s->prx_active) ? "ON" : "OFF");
    fmt_count(lines[n++], "WIN", get(&s->prx_windows));
    fmt_count(lines[n++], "PRE", get(&s->prx_preempts));
    fmt_count(lines[n++], "RX", get(&s->rx_ok));
    fmt_count(lines[n++], "CRC", get(&s->rx_crc_err));
    fmt_count(lines[n++], "ACK", get(&s->ack_payloads));
    fmt_count(lines[n++], "CH", get(&s->channel));
    uint32_t last_rx = get(&s->last_rx_ms);
    if (last_rx) {
        fmt_count(lines[n++], "AGE", (uint32_t)k_uptime_get() - last_rx);
    } else {
        snprintf(lines[n++], LINE_LEN + 1, "AGE   --");
    }
#else
    snprintf(lines[n++], LINE_LEN + 1, "PTX");
    fmt_count(lines[n++], "TX", get(&s->tx_sent));
    fmt_count(lines[n++], "ACK", get(&s->tx_acked));
    fmt_count(lines[n++], "RTX", get(&s->tx_retransmits));
    fmt_count(lines[n++], "BAD", get(&s->tx_failed));
    fmt_count(lines[n++], "HOP", get(&s->ch_hops));
    fmt_count(lines[n++], "CH", get(&s->channel));
    fmt_count(lines[n++], "RTT", get(&s->last_rtt_us));
#endif

#if IS_ENABLED(CONFIG_ZMK_BLE) && IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    snprintf(lines[n++], LINE_LEN + 1, "BLE P%u %c", (uint8_t)(zmk_ble_active_profile_index() + 1) % 10,
             zmk_ble_active_profile_is_connected() ? 'C' : '-');
#elif IS_ENABLED(CONFIG_ZMK_SPLIT_BLE)
    snprintf(lines[n++], LINE_LEN + 1, "SPL %c", zmk_split_bt_peripheral_is_connected() ? 'C' : '-');
#else
    snprintf(lines[n++], LINE_LEN + 1, "BLE OFF");
#endif

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
    snprintf(lines[n++], LINE_LEN + 1, "BAT %3u%%", zmk_battery_state_of_charge());
#else
    n++;
#endif

    fmt_count(lines[n++], "UP", (uint32_t)(k_uptime_get() / 1000));
    snprintf(lines[n++], LINE_LEN + 1, "RST %s", reset_cause_str(reset_cause));

    const struct zmk_split_esb_assert_record *a = zmk_split_esb_last_assert();
    if (a) {
        fmt_count(lines[n++], "AS#", a->count);
        snprintf(lines[n++], LINE_LEN + 1, "%.8s", a->file);
        snprintf(lines[n++], LINE_LEN + 1, "L%u", a->line);
    } else {
        snprintf(lines[n++], LINE_LEN + 1, "AS# none");
    }
}

static void draw(void) {
    lv_draw_label_dsc_t label_dsc;
    lv_draw_label_dsc_init(&label_dsc);
    label_dsc.color = lv_color_black();
    label_dsc.font = &lv_font_unscii_8;
    label_dsc.align = LV_TEXT_ALIGN_LEFT;

    int line = 0;
    for (int c = 0; c < CANVAS_COUNT; c++) {
        lv_obj_t *canvas = canvases[c];
        lv_canvas_fill_bg(canvas, lv_color_white(), LV_OPA_COVER);

        lv_layer_t layer;
        lv_canvas_init_layer(canvas, &layer);
        for (int i = 0; i < lines_per_canvas[c]; i++, line++) {
            label_dsc.text = lines[line];
            lv_area_t coords = {0, i * LINE_HEIGHT, CANVAS_SIZE - 1,
                                i * LINE_HEIGHT + LINE_HEIGHT - 1};
            lv_draw_label(&layer, &label_dsc, &coords);
        }
        lv_canvas_finish_layer(canvas, &layer);

        rotate_canvas(canvas);
    }
}

static void refresh_cb(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(refresh_work, refresh_cb);

static void refresh_cb(struct k_work *work) {
    fill_lines();
    draw();
    k_work_schedule_for_queue(zmk_display_work_q(), &refresh_work,
                              K_MSEC(CONFIG_ZMK_SPLIT_ESB_DEBUG_SCREEN_REFRESH_MS));
}

lv_obj_t *zmk_display_status_screen(void) {
    hwinfo_get_reset_cause(&reset_cause);

    lv_obj_t *screen = lv_obj_create(NULL);

    /* Same placement as ZMK's nice_view widget: top, middle, bottom. */
    static const lv_align_t align[CANVAS_COUNT] = {LV_ALIGN_TOP_RIGHT, LV_ALIGN_TOP_LEFT,
                                                   LV_ALIGN_TOP_LEFT};
    static const int8_t x_ofs[CANVAS_COUNT] = {0, 24, -44};

    for (int c = 0; c < CANVAS_COUNT; c++) {
        canvases[c] = lv_canvas_create(screen);
        lv_obj_align(canvases[c], align[c], x_ofs[c], 0);
        lv_canvas_set_buffer(canvases[c], cbuf[c], CANVAS_SIZE, CANVAS_SIZE, CANVAS_COLOR_FORMAT);
    }

    /* This runs on the display work queue, so the first draw can happen now. */
    refresh_cb(NULL);

    return screen;
}
