#!/usr/bin/env bash
# Copy images onto UF2 bootloader volumes (macOS), one part at a time.
#   scripts/flash.sh [[corne|totem] <part|all> | <file.uf2>]
# With no argument, flashes what scripts/build.sh built last. Parts of one
# keyboard share a Board-ID, so the check only catches the wrong board type.
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)

images=()
case "${1:-}" in
    "")
        [[ -f "$here/build/last" ]] || { echo "nothing built yet; run scripts/build.sh" >&2; exit 2; }
        while read -r name; do images+=("$here/build/$name.uf2"); done < "$here/build/last"
        ;;
    *.uf2)
        images=("$1")
        ;;
    *)
        keyboard=corne
        case "$1" in
            corne | totem) keyboard=$1; shift ;;
        esac
        case "$keyboard" in
            corne) all=(left right) ;;
            totem) all=(left right dongle) ;;
        esac
        part=${1:-all}
        if [[ "$part" == all ]]; then
            for p in "${all[@]}"; do images+=("$here/build/${keyboard}_$p.uf2"); done
        elif [[ " ${all[*]} " =~ " $part " ]]; then
            images=("$here/build/${keyboard}_$part.uf2")
        else
            echo "usage: $0 [[corne|totem] <part|all> | <file.uf2>]" >&2
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

for image in "${images[@]}"; do
    [[ -f "$image" ]] || { echo "missing $image" >&2; exit 1; }
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
