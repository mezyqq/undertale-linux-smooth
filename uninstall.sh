#!/bin/bash
# Removes undertale-linux-smooth from the game folder.
# Usage: ./uninstall.sh [--wasd] [--dir /path/to/steamapps/common/Undertale]
#   --wasd  also restore the original game data (undo the WASD patch)
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
game=""
wasd=0
while [ $# -gt 0 ]; do
    case "$1" in
    --wasd) wasd=1 ;;
    --dir) game=${2:?--dir needs a path}; shift ;;
    -h | --help) sed -n '2,4p' "$0" | sed 's/^# //'; exit 0 ;;
    *) echo "Unknown option: $1" >&2; exit 1 ;;
    esac
    shift
done
if [ -z "$game" ]; then
    for root in "$HOME/.local/share/Steam" "$HOME/.steam/steam" "$HOME/.var/app/com.valvesoftware.Steam/.local/share/Steam"; do
        [ -e "$root/steamapps/common/Undertale/runner.real" ] && { game="$root/steamapps/common/Undertale"; break; }
    done
fi
if [ -z "$game" ] || [ ! -e "$game/runner.real" ]; then
    echo "No installation found. Pass the game folder with --dir." >&2
    exit 1
fi
if pgrep -f "$game/runner" >/dev/null; then
    echo "Undertale is running. Close it first." >&2
    exit 1
fi
mv -f "$game/runner.real" "$game/runner"
rm -rf "$game/linux-smooth"
echo "Smoothing and layout fix removed."
if [ "$wasd" = 1 ]; then
    "$here/wasd/unpatch-wasd.sh" "$game"
fi
