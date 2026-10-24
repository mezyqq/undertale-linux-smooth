#!/bin/bash
# Installs undertale-linux-smooth into the Steam (native Linux) copy of Undertale.
# Usage: ./install.sh [--wasd] [--dir /path/to/steamapps/common/Undertale]
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
game=""
wasd=0

while [ $# -gt 0 ]; do
    case "$1" in
    --wasd) wasd=1 ;;
    --dir) game=${2:?--dir needs a path}; shift ;;
    -h | --help) sed -n '2,3p' "$0" | sed 's/^# //'; exit 0 ;;
    *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
    shift
done

find_game() {
    local root p
    for root in "$HOME/.local/share/Steam" "$HOME/.steam/steam" "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam"; do
        [ -d "$root/steamapps/common/Undertale" ] && echo "$root/steamapps/common/Undertale"
        [ -f "$root/steamapps/libraryfolders.vdf" ] || continue
        # other Steam libraries (extra drives)
        grep -oP '"path"\s*"\K[^"]+' "$root/steamapps/libraryfolders.vdf" | while read -r p; do
            [ -d "$p/steamapps/common/Undertale" ] && echo "$p/steamapps/common/Undertale"
        done
    done | head -n 1
}

[ -n "$game" ] || game=$(find_game)
if [ -z "$game" ] || [ ! -d "$game" ]; then
    echo "Undertale not found. Pass its folder: ./install.sh --dir /path/to/steamapps/common/Undertale" >&2
    exit 1
fi
if [ ! -f "$game/assets/game.unx" ]; then
    echo "$game is not the native Linux version of Undertale (no assets/game.unx)." >&2
    echo "In Steam: Undertale > Properties > Compatibility, turn off Proton and reinstall." >&2
    exit 1
fi
if pgrep -f "$game/runner" >/dev/null; then
    echo "Undertale is running. Save, close it and run the installer again." >&2
    exit 1
fi
is_elf() { [ "$(head -c 4 "$1" 2>/dev/null | od -An -c | tr -d ' \n')" = "177ELF" ]; }

# Libraries: build from source when a 32-bit compiler works, otherwise use the bundled ones.
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
if echo 'int main(void){return 0;}' > "$build/t.c" && gcc -m32 "$build/t.c" -o "$build/t" 2>/dev/null &&
    make -s -C "$here" CC=gcc >/dev/null 2>&1; then
    echo "Built libraries from source."
elif [ -f "$here/lib/smooth.so" ] && [ -f "$here/lib/layoutfix.so" ]; then
    echo "Using prebuilt libraries (no 32-bit compiler found)."
else
    echo "No prebuilt libraries in $here/lib and no 32-bit compiler (gcc -m32). Use the release .zip." >&2
    exit 1
fi

mods="$game/linux-smooth"
mkdir -p "$mods"
# copy then rename: a file that a running program has loaded must never be overwritten in place
put() { cp "$1" "$2.new.$$" && chmod "$3" "$2.new.$$" && mv -f "$2.new.$$" "$2"; }
put "$here/lib/smooth.so" "$mods/smooth.so" 755
put "$here/lib/layoutfix.so" "$mods/layoutfix.so" 755

if [ ! -e "$game/runner.real" ]; then
    if ! is_elf "$game/runner"; then
        echo "$game/runner is not the game binary and runner.real is missing; reinstall the game in Steam." >&2
        exit 1
    fi
    mv "$game/runner" "$game/runner.real"
fi
put "$here/runner-wrapper.sh" "$game/runner" 755

echo "Installed into $game"
echo "  smoothing: up to 144 FPS (game logic unchanged), log: $mods/smooth.log"
echo "  layoutfix: controls work in any keyboard layout"

if [ "$wasd" = 1 ]; then
    "$here/wasd/patch-wasd.sh" "$game"
fi
echo "Start Undertale from Steam as usual."
