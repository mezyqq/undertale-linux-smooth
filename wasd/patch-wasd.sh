#!/bin/bash
# Optional: W/A/S/D work as the arrow keys. Patches assets/game.unx with UndertaleModTool's CLI
# (downloaded from its official GitHub release) and keeps the original as game.unx.pre-wasd.
# Usage: wasd/patch-wasd.sh /path/to/steamapps/common/Undertale
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
game=${1:?usage: patch-wasd.sh /path/to/steamapps/common/Undertale}
unx="$game/assets/game.unx"
utmt_url=https://github.com/UnderminersTeam/UndertaleModTool/releases/download/0.9.2.0/UTMT_CLI_v0.9.2.0-Ubuntu.zip

[ -f "$unx" ] || { echo "No $unx" >&2; exit 1; }
if pgrep -f "$game/runner" >/dev/null; then echo "Undertale is running. Close it first." >&2; exit 1; fi
if grep -q -a keyboard_set_map "$unx"; then echo "WASD: already patched."; exit 0; fi
for tool in curl unzip; do
    command -v $tool >/dev/null || { echo "WASD: '$tool' is needed." >&2; exit 1; }
done

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
echo "WASD: downloading UndertaleModTool CLI..."
curl -fsSL -o "$tmp/utmt.zip" "$utmt_url"
(cd "$tmp" && unzip -q utmt.zip && chmod +x UndertaleModCli)
echo "WASD: patching game data (about a minute)..."
# the CLI waits for a key press at the end unless stdin is closed
if ! "$tmp/UndertaleModCli" load "$unx" -s "$here/wasd.csx" -o "$tmp/game.unx" -f </dev/null >"$tmp/log.txt" 2>&1 ||
    ! grep -q -a keyboard_set_map "$tmp/game.unx"; then
    cat "$tmp/log.txt" >&2
    echo "WASD: patch failed, game data left unchanged." >&2
    exit 1
fi
[ -e "$unx.pre-wasd" ] || cp "$unx" "$unx.pre-wasd"
cp "$tmp/game.unx" "$unx.new.$$" && mv -f "$unx.new.$$" "$unx"
echo "WASD: done (original kept as $unx.pre-wasd)."
