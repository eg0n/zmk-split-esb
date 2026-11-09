/*
 * Copyright (c) 2026 The ZMK Contributors
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/logging/log.h>

#include <zmk_split_esb/stats.h>

LOG_MODULE_REGISTER(zmk_split_esb, CONFIG_ZMK_SPLIT_ESB_LOG_LEVEL);

struct zmk_split_esb_stats zmk_split_esb_stats;

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_ASSERT_RECORD)

#define ASSERT_RECORD_MAGIC 0x45534241 /* "ESBA" */

/* Survives a warm reset. After power-on its contents are random, which the
 * magic and its complement catch. */
static __noinit struct {
    uint32_t magic;
    uint32_t magic_inv;
    struct zmk_split_esb_assert_record rec;
} retained;

static struct zmk_split_esb_assert_record last_assert;
static bool have_last_assert;

static bool retained_valid(void) {
    return retained.magic == ASSERT_RECORD_MAGIC && retained.magic_inv == ~ASSERT_RECORD_MAGIC;
}

const struct zmk_split_esb_assert_record *zmk_split_esb_last_assert(void) {
    return have_last_assert ? &last_assert : NULL;
}

/* Called by LL_ASSERT in the controller, possibly from the radio ISR. */
void bt_ctlr_assert_handle(char *file, uint32_t line) {
    const char *base = strrchr(file, '/');
    base = base ? base + 1 : file;

    uint32_t count = retained_valid() ? retained.rec.count : 0;

    retained.rec.count = count + 1;
    retained.rec.line = line;
    strncpy(retained.rec.file, base, sizeof(retained.rec.file) - 1);
    retained.rec.file[sizeof(retained.rec.file) - 1] = '\0';
    retained.magic = ASSERT_RECORD_MAGIC;
    retained.magic_inv = ~ASSERT_RECORD_MAGIC;

    sys_reboot(SYS_REBOOT_WARM);
}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_SELFTEST_ASSERT)
static void selftest_assert_cb(struct k_work *work) { bt_ctlr_assert_handle(__FILE__, __LINE__); }

static K_WORK_DELAYABLE_DEFINE(selftest_assert_work, selftest_assert_cb);
#endif

static int assert_record_init(void) {
    if (retained_valid()) {
        last_assert = retained.rec;
        have_last_assert = true;
        LOG_WRN("Controller assert before reboot: %s:%u (%u total)", last_assert.file,
                last_assert.line, last_assert.count);
    } else {
        memset(&retained, 0, sizeof(retained));
    }

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_SELFTEST_ASSERT)
    if (!have_last_assert) {
        k_work_schedule(&selftest_assert_work, K_SECONDS(30));
    }
#endif

    return 0;
}

SYS_INIT(assert_record_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#else

const struct zmk_split_esb_assert_record *zmk_split_esb_last_assert(void) { return NULL; }

#endif /* IS_ENABLED(CONFIG_ZMK_SPLIT_ESB_ASSERT_RECORD) */
