#!/usr/bin/env bash
# Copy Corne images onto nice!nano UF2 bootloader volumes (macOS).
#   scripts/flash.sh [left|right|all|<file.uf2>]
# With no argument, flashes what scripts/build.sh built last, one half at a
# time. Both halves share a Board-ID, so the check only catches the wrong board.
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
board_pattern=${FLASH_BOARD_PATTERN:-nice}

images=()
target=${1:-}
if [[ -z "$target" ]]; then
    [[ -f "$here/build/last" ]] || { echo "nothing built yet; run scripts/build.sh" >&2; exit 2; }
    while read -r side; do images+=("$here/build/corne_$side.uf2"); done < "$here/build/last"
elif [[ "$target" == *.uf2 ]]; then
    images=("$target")
else
    case "$target" in
        left|right) images=("$here/build/corne_$target.uf2") ;;
        all) images=("$here/build/corne_left.uf2" "$here/build/corne_right.uf2") ;;
        *) echo "usage: $0 [left|right|all|<file.uf2>]" >&2; exit 2 ;;
    esac
fi

find_volume() {
    for v in /Volumes/*; do
        [[ -f "$v/INFO_UF2.TXT" ]] && { echo "$v"; return 0; }
    done
    return 1
}

for image in "${images[@]}"; do
    [[ -f "$image" ]] || { echo "missing $image" >&2; exit 1; }
    name=$(basename "$image" .uf2)

    if vol=$(find_volume); then
        echo "A bootloader drive is already mounted at $vol; eject it or flash it first." >&2
        exit 1
    fi

    echo "Double-tap reset on ${name#corne_} ($name)..."
    until vol=$(find_volume); do sleep 0.5; done

    if ! grep -qi "$board_pattern" "$vol/INFO_UF2.TXT"; then
        echo "$vol doesn't look like a nice!nano:" >&2
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
