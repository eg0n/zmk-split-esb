# zmk-split-esb

A ZMK split transport over Nordic's Enhanced ShockBurst (ESB) for nRF52 halves.
The central keeps full BLE to its hosts, including multiple profiles.

Instead of Nordic's SoftDevice Controller and MPSL timeslots (which ZMK
doesn't use), the ESB receiver runs as another radio role inside Zephyr's
open-source BLE controller, through its `CONFIG_BT_CTLR_USER_EXT` hooks. BLE
events always win, and the receiver listens in the gaps between them, much as
the controller's own continuous scanner does. Peripherals own their radio and
transmit ESB only.

## Status

**Phase 0:** the controller hooks are compiled in but schedule nothing, so BLE
behaves exactly as without the module. The nice!view debug screen and the
controller assert record work.

| Phase | Goal |
|---|---|
| 0 | Hooks compiled in, debug screen, assert record |
| 1 | Hosted receiver listening alongside full BLE (split + host) |
| 2 | Full ESB split on a Corne |
| 3 | Idle duty-cycling, channel hopping, ACK-payload commands |
| 4 | Dongle (Totem + Prospector) |

## Debug screen

With `CONFIG_ZMK_SPLIT_ESB_DEBUG_SCREEN=y` (and `CONFIG_NICE_VIEW_WIDGET_STATUS=n`),
the nice!view shows, top to bottom:

| Central | Peripheral | Meaning |
|---|---|---|
| `PRX ON/OFF` | `PTX` | role, and whether the hosted receiver is scheduled |
| `WIN` | `TX` | receive windows started / packets sent |
| `PRE` | `ACK` | windows preempted by BLE / packets acknowledged |
| `RX` | `RTX` | packets received / retransmissions |
| `CRC` | `BAD` | CRC failures / packets given up on |
| `ACK` | `HOP` | ACK payloads sent / channel hops |
| `CH` | `CH` | current RF channel |
| `AGE` | `RTT` | ms since the peripheral was last heard / last round trip (µs) |
| `BLE Pn C` | `SPL C` | active BLE profile and whether it's connected / BLE split link |
| `BAT` | `BAT` | battery %, or `USB` while USB-powered (the voltage estimate reads 100% while charging) |
| `UP` | `UP` | uptime in seconds |
| `RST` | `RST` | last reset cause (`SOFT` after an assert reboot) |
| `AS#` | `AS#` | controller asserts recorded, then the file and line of the last one |

Counters above 99999 switch to thousands (`12k`).

When the controller hits an `LL_ASSERT`, the file and line are kept in RAM that
survives a warm reset, and the board reboots. `CONFIG_ZMK_SPLIT_ESB_SELFTEST_ASSERT=y`
raises one fake assert 30 s after boot so the path can be checked.

## Building and flashing (Corne, local)

The scripts build in the `zmkfirmware/zmk-build-arm` container and reuse the
Zephyr checkout from the ZMK devcontainer's Docker volumes. Override the paths
with `ZMK_DIR` (default `~/src/zmk`) and `ZMK_CONFIG_DIR` (default
`~/src/miryoku_zmk`).

```sh
scripts/build.sh left          # or right, or all; extra args go to CMake
scripts/flash.sh               # flashes what was just built, one half at a time
```

Per-half Kconfig lives in `conf/corne_<side>.conf`.

## License

MIT
