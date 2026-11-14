#!/usr/bin/env bash
# Build ESB images in the ZMK build container, reusing the devcontainer's
# Zephyr volumes read-only; the Totem also needs its modules in ZMK_MODULES_DIR.
#   scripts/build.sh [corne|totem] [<part>|all] [extra cmake args...]
# Images go to build/<kb>_<part>.uf2, or -custom.uf2 with extra CMake args. Boards with
# Nordic's nRF5 bootloader get a .hex instead, and a DFU .zip if nrfutil is installed.
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
zmk_dir=${ZMK_DIR:-$HOME/src/zmk}
config_dir=${ZMK_CONFIG_DIR:-$HOME/src/miryoku_zmk}
modules_dir=${ZMK_MODULES_DIR:-$HOME/src/zmk-modules}
image=${ZMK_BUILD_IMAGE:-zmkfirmware/zmk-build-arm:stable}
layout=${PROSPECTOR_LAYOUT:-operator}

keyboard=corne
case "${1:-}" in
    corne | totem) keyboard=$1; shift ;;
esac

target=${1:-left}
shift || true

# Miryoku layout choices, as in miryoku_zmk's build-acb-* workflows
miryoku="-DMIRYOKU_ALPHAS_COLEMAK;-DMIRYOKU_CLIPBOARD_MAC;-DMIRYOKU_EXTRA_QWERTY;-DMIRYOKU_TAP_COLEMAK"

case "$keyboard" in
    corne)
        board=nice_nano//zmk
        all=(left right)
        ;;
    totem)
        board=xiao_ble//zmk
        all=(left right dongle)
        # A Nordic nRF52840 Dongle (or a clone) as the dongle, and settings resets
        more=(nrfdongle reset nrfdongle_reset)
        ;;
esac

case "$target" in
    all) parts=("${all[@]}") ;;
    *)
        if [[ ! " ${all[*]} ${more[*]-} " =~ " $target " ]]; then
            echo "usage: $0 [corne|totem] [${all[*]// /|}${more:+|${more[*]// /|}}|all] [cmake args...]" >&2
            exit 2
        fi
        parts=("$target")
        ;;
esac

# Sets board, shield, modules (space-separated container paths), cppflags, conf
# (the conf/ files' part name, empty for none), more_conf (a further conf/ file's
# part name) and extra (CMake arguments) for one part.
part_settings() {
    local part=$1
    board=$keyboard_board
    conf=$part
    more_conf=""
    modules=""
    extra=()
    case "$part" in
        reset | nrfdongle_reset)
            [[ "$part" == nrfdongle_reset ]] && board=nrf52840dongle//zmk
            shield=settings_reset
            cppflags=""
            conf=""
            return
            ;;
    esac
    case "$keyboard" in
        corne)
            shield="corne_$part nice_view_adapter nice_view"
            cppflags="$miryoku"
            ;;
        totem)
            modules="/workspaces/zmk-modules/zmk-rgbled-widget"
            cppflags="$miryoku;-DMIRYOKU_KLUDGE_GLOBALSHIFTFUNCTIONS;-DRGBLED_WIDGET_BEHAVIORS"
            if [[ "$part" == dongle ]]; then
                shield="totem_dongle prospector_adapter"
                modules="$modules /workspaces/zmk-modules/prospector-zmk-module"
                cppflags="$cppflags;-DBACKLIGHT_BEHAVIORS"
                extra=("-DCONFIG_PROSPECTOR_STATUS_SCREEN_$(echo "$layout" | tr "[:lower:]" "[:upper:]")=y")
            elif [[ "$part" == nrfdongle ]]; then
                # No screen: the Prospector adapter only fits a XIAO.
                board=nrf52840dongle//zmk
                shield=totem_dongle
                conf=dongle
                more_conf=nrfdongle
                modules="$modules /workspaces/zmk-modules/zmk-keybeacon"
            else
                shield="totem_$part rgbled_adapter"
            fi
            ;;
    esac
}

suffix=""
if [[ $# -gt 0 ]]; then
    suffix="-custom"
fi

keyboard_board=$board
mkdir -p "$here/build"
built=()
for part in "${parts[@]}"; do
    name="${keyboard}_$part$suffix"
    echo "== $name"
    part_settings "$part"
    if [[ -n "$conf" ]]; then
        conf_files="/workspaces/zmk-split-esb/conf/${keyboard}_$conf.conf"
        if [[ -n "$more_conf" ]]; then
            conf_files="$conf_files;/workspaces/zmk-split-esb/conf/${keyboard}_$more_conf.conf"
        fi
        extra+=("-DEXTRA_CONF_FILE=$conf_files")
        if [[ -f "$here/conf/${keyboard}_$conf.overlay" ]]; then
            extra+=("-DEXTRA_DTC_OVERLAY_FILE=/workspaces/zmk-split-esb/conf/${keyboard}_$conf.overlay")
        fi
    fi

    # Zephyr misses edits to this module's Kconfig in an existing build dir,
    # and -D options stick in its cache, so start fresh when either changes.
    stamp="$here/build/$name/.module-kconfig"
    kconfig_hash=$( (cat "$here/Kconfig" "$here/zephyr/module.yml"; echo "${extra[@]+${extra[*]}}") |
        shasum | cut -d' ' -f1)
    if [[ -d "$here/build/$name" && "$(cat "$stamp" 2>/dev/null)" != "$kconfig_hash" ]]; then
        echo "   module Kconfig or build options changed, rebuilding from scratch"
        rm -rf "$here/build/$name"
    fi

    module_list="/workspaces/zmk-split-esb"
    for m in $modules; do
        module_list="$module_list;$m"
    done

    docker run --rm \
        -v "$zmk_dir":/workspaces/zmk \
        -v zmk-zephyr:/workspaces/zmk/zephyr:ro \
        -v zmk-zephyr-modules:/workspaces/zmk/modules:ro \
        -v zmk-zephyr-tools:/workspaces/zmk/tools:ro \
        -v "$config_dir":/workspaces/zmk-config:ro \
        -v "$modules_dir":/workspaces/zmk-modules:ro \
        -v "$here":/workspaces/zmk-split-esb \
        -w /workspaces/zmk/app \
        "$image" \
        west build -p auto -b "$board" -d "/workspaces/zmk-split-esb/build/$name" -- \
            -DSHIELD="$shield" \
            -DZMK_CONFIG=/workspaces/zmk-config/config \
            -DZMK_EXTRA_MODULES="$module_list" \
            -DDTS_EXTRA_CPPFLAGS="$cppflags" \
            ${extra[@]+"${extra[@]}"} \
            "$@"
    echo "$kconfig_hash" > "$stamp"
    out="$here/build/$name/zephyr"
    if [[ -f "$out/zmk.uf2" ]]; then
        cp "$out/zmk.uf2" "$here/build/$name.uf2"
    else
        cp "$out/zmk.hex" "$here/build/$name.hex"
        if command -v nrfutil >/dev/null && nrfutil nrf5sdk-tools --help >/dev/null 2>&1; then
            nrfutil nrf5sdk-tools pkg generate --hw-version 52 --sd-req 0x00 --application-version 1 \
                --application "$here/build/$name.hex" "$here/build/$name.zip" >/dev/null
        else
            echo "   no DFU package: install nrfutil and run 'nrfutil install nrf5sdk-tools'" >&2
        fi
    fi
    built+=("$name")
done

if [[ -z "$suffix" ]]; then
    printf '%s\n' "${built[@]}" > "$here/build/last"
fi
