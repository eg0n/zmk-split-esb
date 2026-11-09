#!/usr/bin/env bash
# Build Corne images in the ZMK build container, reusing the devcontainer's
# Zephyr volumes read-only. Images go to build/corne_<side>.uf2, and
# build/last records them for scripts/flash.sh.
#   scripts/build.sh [left|right|all] [extra cmake args...]
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
zmk_dir=${ZMK_DIR:-$HOME/src/zmk}
config_dir=${ZMK_CONFIG_DIR:-$HOME/src/miryoku_zmk}
image=${ZMK_BUILD_IMAGE:-zmkfirmware/zmk-build-arm:stable}

target=${1:-left}
shift || true
case "$target" in
    left) sides=(left) ;;
    right) sides=(right) ;;
    all) sides=(left right) ;;
    *) echo "usage: $0 [left|right|all] [cmake args...]" >&2; exit 2 ;;
esac

# Miryoku layout choices, as in miryoku_zmk's build-acb-corne workflow
cppflags="-DMIRYOKU_ALPHAS_COLEMAK;-DMIRYOKU_CLIPBOARD_MAC;-DMIRYOKU_EXTRA_QWERTY;-DMIRYOKU_TAP_COLEMAK"

mkdir -p "$here/build"
for side in "${sides[@]}"; do
    echo "== corne_$side"
    docker run --rm \
        -v "$zmk_dir":/workspaces/zmk \
        -v zmk-zephyr:/workspaces/zmk/zephyr:ro \
        -v zmk-zephyr-modules:/workspaces/zmk/modules:ro \
        -v zmk-zephyr-tools:/workspaces/zmk/tools:ro \
        -v "$config_dir":/workspaces/zmk-config:ro \
        -v "$here":/workspaces/zmk-split-esb \
        -w /workspaces/zmk/app \
        "$image" \
        west build -p auto -b nice_nano//zmk -d "/workspaces/zmk-split-esb/build/corne_$side" -- \
            -DSHIELD="corne_$side nice_view_adapter nice_view" \
            -DZMK_CONFIG=/workspaces/zmk-config/config \
            -DZMK_EXTRA_MODULES=/workspaces/zmk-split-esb \
            -DEXTRA_CONF_FILE="/workspaces/zmk-split-esb/conf/corne_$side.conf" \
            -DDTS_EXTRA_CPPFLAGS="$cppflags" \
            "$@"
    cp "$here/build/corne_$side/zephyr/zmk.uf2" "$here/build/corne_$side.uf2"
done

printf '%s\n' "${sides[@]}" > "$here/build/last"
