#!/usr/bin/env bash
# Copy images onto UF2 bootloader volumes (macOS), one part at a time; a DFU .zip
# goes to a Nordic nRF5 bootloader through nrfutil instead.
#   scripts/flash.sh [[corne|totem] <part|all> | <file.uf2|file.zip>]
# With no argument, flashes what scripts/build.sh built last. Parts of one
# keyboard share a Board-ID, so the check only catches the wrong board type.
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)

# A built part's image: its .uf2, or the DFU .zip of a board with the nRF5 bootloader.
image_for() {
    if [[ -f "$here/build/$1.zip" ]]; then echo "$here/build/$1.zip"; else echo "$here/build/$1.uf2"; fi
}

images=()
case "${1:-}" in
    "")
        [[ -f "$here/build/last" ]] || { echo "nothing built yet; run scripts/build.sh" >&2; exit 2; }
        while read -r name; do images+=("$(image_for "$name")"); done < "$here/build/last"
        ;;
    *.uf2 | *.zip)
        images=("$1")
        ;;
    *)
        keyboard=corne
        case "$1" in
            corne | totem) keyboard=$1; shift ;;
        esac
        case "$keyboard" in
            corne) all=(left right) ;;
            totem) all=(left right dongle) more=(nrfdongle reset nrfdongle_reset) ;;
        esac
        part=${1:-all}
        if [[ "$part" == all ]]; then
            for p in "${all[@]}"; do images+=("$(image_for "${keyboard}_$p")"); done
        elif [[ " ${all[*]} ${more[*]-} " =~ " $part " ]]; then
            images=("$(image_for "${keyboard}_$part")")
        else
            echo "usage: $0 [[corne|totem] <part|all> | <file.uf2|file.zip>]" >&2
            exit 2
        fi
        ;;
esac

find_volume() {
    for v in /Volumes/*; do
        [[ -f "$v/INFO_UF2.TXT" ]] && { echo "$v"; return 0; }
    done
    return 1
}

nrf5_bootloader_up() {
    ioreg -p IOUSB -l -w0 | grep -q '"USB Product Name" = "Open DFU Bootloader"'
}

for image in "${images[@]}"; do
    [[ -f "$image" ]] || { echo "missing $image" >&2; exit 1; }

    if [[ "$image" == *.zip ]]; then
        name=$(basename "$image" .zip)
        echo "Start the bootloader on $name: press RESET, or hold it while plugging in..."
        until nrf5_bootloader_up; do sleep 0.5; done
        sleep 1
        port=$(ls /dev/tty.usbmodem* | head -1)
        echo "Flashing $name through $port"
        nrfutil nrf5sdk-tools dfu usb-serial -pkg "$image" -p "$port"
        echo "Done: $name"
        continue
    fi

    name=$(basename "$image" .uf2)

    case "$name" in
        corne_*) board_pattern=${FLASH_BOARD_PATTERN:-nice} ;;
        totem_*) board_pattern=${FLASH_BOARD_PATTERN:-xiao} ;;
        *) board_pattern=${FLASH_BOARD_PATTERN:-.} ;;
    esac

    if vol=$(find_volume); then
        echo "A bootloader drive is already mounted at $vol; eject it or flash it first." >&2
        exit 1
    fi

    echo "Double-tap reset on $name..."
    until vol=$(find_volume); do sleep 0.5; done

    if ! grep -qi "$board_pattern" "$vol/INFO_UF2.TXT"; then
        echo "$vol doesn't look like the board for $name:" >&2
        cat "$vol/INFO_UF2.TXT" >&2
        exit 1
    fi

    echo "Copying $name to $vol"
    # The board reboots as soon as the image is written, which can make cp
    # report an I/O error after a successful flash.
    cp -X "$image" "$vol/" 2>/dev/null || true

    while [[ -d "$vol" ]]; do sleep 0.5; done
    echo "Done: $name"
done
