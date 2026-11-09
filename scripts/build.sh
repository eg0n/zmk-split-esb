#!/usr/bin/env bash
# Build Corne images in the ZMK build container, reusing the devcontainer's
# Zephyr volumes read-only. Images go to build/corne_<side>.uf2, or
# corne_<side>-custom.uf2 with extra CMake args (they stick in the cache).
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

suffix=""
if [[ $# -gt 0 ]]; then
    suffix="-custom"
fi

mkdir -p "$here/build"
for side in "${sides[@]}"; do
    name="corne_$side$suffix"
    echo "== $name"

    # Zephyr doesn't notice edits to this module's Kconfig in an existing
    # build directory, so start it fresh whenever they change.
    stamp="$here/build/$name/.module-kconfig"
    kconfig_hash=$(cat "$here/Kconfig" "$here/zephyr/module.yml" | shasum | cut -d' ' -f1)
    if [[ -d "$here/build/$name" && "$(cat "$stamp" 2>/dev/null)" != "$kconfig_hash" ]]; then
        echo "   module Kconfig changed, rebuilding from scratch"
        rm -rf "$here/build/$name"
    fi
    docker run --rm \
        -v "$zmk_dir":/workspaces/zmk \
        -v zmk-zephyr:/workspaces/zmk/zephyr:ro \
        -v zmk-zephyr-modules:/workspaces/zmk/modules:ro \
        -v zmk-zephyr-tools:/workspaces/zmk/tools:ro \
        -v "$config_dir":/workspaces/zmk-config:ro \
        -v "$here":/workspaces/zmk-split-esb \
        -w /workspaces/zmk/app \
        "$image" \
        west build -p auto -b nice_nano//zmk -d "/workspaces/zmk-split-esb/build/$name" -- \
            -DSHIELD="corne_$side nice_view_adapter nice_view" \
            -DZMK_CONFIG=/workspaces/zmk-config/config \
            -DZMK_EXTRA_MODULES=/workspaces/zmk-split-esb \
            -DEXTRA_CONF_FILE="/workspaces/zmk-split-esb/conf/corne_$side.conf" \
            -DDTS_EXTRA_CPPFLAGS="$cppflags" \
            "$@"
    echo "$kconfig_hash" > "$stamp"
    cp "$here/build/$name/zephyr/zmk.uf2" "$here/build/$name.uf2"
done

if [[ -z "$suffix" ]]; then
    printf '%s\n' "${sides[@]}" > "$here/build/last"
fi
