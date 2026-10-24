#!/bin/bash
# Restores the game data saved by patch-wasd.sh.
# Usage: wasd/unpatch-wasd.sh /path/to/steamapps/common/Undertale
set -euo pipefail
game=${1:?usage: unpatch-wasd.sh /path/to/steamapps/common/Undertale}
unx="$game/assets/game.unx"
[ -f "$unx.pre-wasd" ] || { echo "WASD: no backup ($unx.pre-wasd), nothing to restore."; exit 0; }
if pgrep -f "$game/runner" >/dev/null; then echo "Undertale is running. Close it first." >&2; exit 1; fi
mv -f "$unx.pre-wasd" "$unx"
echo "WASD: original game data restored."
