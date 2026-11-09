# Copyright (c) 2026 The ZMK Contributors
# SPDX-License-Identifier: MIT

if(CONFIG_ZMK_SPLIT_ESB)
  zephyr_library()
  zephyr_include_directories(include)
  zephyr_library_include_directories(${APPLICATION_SOURCE_DIR}/include)

  zephyr_library_sources(src/stats.c)

  if(CONFIG_ZMK_SPLIT_ESB_HOSTED_PRX OR CONFIG_ZMK_SPLIT_ESB_PTX)
    zephyr_library_sources(src/phy/esb_phy.c)
  endif()
  zephyr_library_sources_ifdef(CONFIG_ZMK_SPLIT_ESB_PTX src/standalone/ptx.c)

  if(CONFIG_ZMK_SPLIT_ESB_TRANSPORT)
    zephyr_library_sources_ifdef(CONFIG_ZMK_SPLIT_ESB_HOSTED_PRX
      src/transport/central.c
      src/events/peripheral_link_changed.c
    )
    zephyr_library_sources_ifdef(CONFIG_ZMK_SPLIT_ESB_PTX src/transport/peripheral.c)
  endif()

  if(CONFIG_ZMK_SPLIT_ESB_HOSTED_PRX)
    # With BT_CTLR_USER_EXT the controller's own sources include
    # "ull_vendor.h", so that header has to be on the global include path.
    zephyr_include_directories(include/ll_ext)

    # The hosted receiver is built against the controller's internal headers.
    set(BT_CTLR_DIR ${ZEPHYR_BASE}/subsys/bluetooth/controller)
    zephyr_library_include_directories(
      ${BT_CTLR_DIR}
      ${BT_CTLR_DIR}/include
      ${BT_CTLR_DIR}/ll_sw
      ${BT_CTLR_DIR}/ll_sw/nordic
      ${BT_CTLR_DIR}/ll_sw/nordic/lll
      ${BT_CTLR_DIR}/ll_sw/nordic/hci
    )
    zephyr_library_sources(src/ll_ext/prx_hosted.c)
    zephyr_library_sources_ifdef(CONFIG_ZMK_SPLIT_ESB_PRX_LOW_POWER_IDLE src/ll_ext/prx_power.c)
  endif()

  zephyr_library_sources_ifdef(CONFIG_ZMK_SPLIT_ESB_DEBUG_SCREEN src/debug/esb_debug_screen.c)
endif()
