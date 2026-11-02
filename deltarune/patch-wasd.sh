#!/bin/bash
# DELTARUNE (Steam or standalone, Windows version under Proton): W/A/S/D work as the arrow keys in the
# chapter select and in every chapter. Patches each data.win with UndertaleModTool's CLI (downloaded from
# its official GitHub release, or taken from $UTMT_CLI) and keeps the originals as data.win.pre-wasd.
# Install a translation (e.g. the Russian one) first: it replaces data.win, then run this again.
# Usage: deltarune/patch-wasd.sh /path/to/DELTARUNE
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
game=${1:?usage: patch-wasd.sh /path/to/DELTARUNE}
utmt_url=https://github.com/UnderminersTeam/UndertaleModTool/releases/download/0.9.2.0/UTMT_CLI_v0.9.2.0-Ubuntu.zip

[ -f "$game/data.win" ] || { echo "No $game/data.win" >&2; exit 1; }
if pgrep -fi 'DELTARUNE.exe' >/dev/null; then echo "DELTARUNE is running. Close it first." >&2; exit 1; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cli=${UTMT_CLI:-}
if [ -z "$cli" ]; then
    for tool in curl unzip; do
        command -v $tool >/dev/null || { echo "WASD: '$tool' is needed." >&2; exit 1; }
    done
    echo "WASD: downloading UndertaleModTool CLI..."
    curl -fsSL -o "$tmp/utmt.zip" "$utmt_url"
    (cd "$tmp" && unzip -q utmt.zip && chmod +x UndertaleModCli)
    cli=$tmp/UndertaleModCli
fi

for win in "$game/data.win" "$game"/chapter*_windows/data.win; do
    [ -f "$win" ] || continue
    if grep -q -a keyboard_set_map "$win"; then echo "WASD: ${win#$game/} already patched."; continue; fi
    echo "WASD: patching ${win#$game/}..."
    # the CLI waits for a key press at the end unless stdin is closed
    if ! "$cli" load "$win" -s "$here/wasd.csx" -o "$tmp/out.win" -f </dev/null >"$tmp/log.txt" 2>&1 ||
        ! grep -q -a keyboard_set_map "$tmp/out.win"; then
        cat "$tmp/log.txt" >&2
        echo "WASD: patch failed, ${win#$game/} left unchanged." >&2
        exit 1
    fi
    cp "$win" "$win.pre-wasd"
    mv -f "$tmp/out.win" "$win"
done
echo "WASD: done (originals kept as data.win.pre-wasd)."
