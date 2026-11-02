#!/bin/bash
# DELTARUNE: puts the 60 FPS Overhaul (BadArtAdventure, v2.0.2, "advanced" package from GameBanana) under an
# installed Russian translation (LazyDesman 5.0.8). The overhaul's UndertaleModTool scripts only accept clean
# vanilla data.win files, so every code entry that both mods change (the translation keeps the original as
# <name>_old) gets the 60 FPS edits 3-way merged into the Russian code (ru-60fps/), the rest is taken as is.
# Needs git, python3, curl+unzip, the vanilla data.win files of the same game version (checked by SHA-256).
# The Russian-only files are kept as data.win.pre-60fps; WASD (deltarune/patch-wasd.sh) is reapplied if present.
# Usage: deltarune/patch-60fps-ru.sh /path/to/DELTARUNE deltarune_60fps_advanced_v202.zip /path/to/vanilla/DELTARUNE
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
usage="usage: patch-60fps-ru.sh /path/to/DELTARUNE /path/to/deltarune_60fps_advanced_v202.zip /path/to/vanilla/DELTARUNE"
game=${1:?$usage}; zip=${2:?$usage}; vanilla=${3:?$usage}
tools=$here/ru-60fps
utmt_url=https://github.com/UnderminersTeam/UndertaleModTool/releases/download/0.9.2.0/UTMT_CLI_v0.9.2.0-Ubuntu.zip
die() { echo "60fps-ru: $*" >&2; exit 1; }

for tool in git python3 unzip sha256sum; do command -v $tool >/dev/null || die "'$tool' is needed."; done
[ -f "$zip" ] || die "no package $zip"
if pgrep -fi '[D]ELTARUNE[.]exe' >/dev/null; then die "DELTARUNE is running. Close it first."; fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cli=${UTMT_CLI:-}
if [ -z "$cli" ]; then
    command -v curl >/dev/null || die "'curl' is needed."
    echo "60fps-ru: downloading UndertaleModTool CLI..."
    curl -fsSL -o "$tmp/utmt.zip" "$utmt_url"
    (cd "$tmp" && unzip -q utmt.zip -d utmt && chmod +x utmt/UndertaleModCli)
    cli=$tmp/utmt/UndertaleModCli
fi
# the package uses backslashes as path separators: unzip warns (exit 1) but extracts fine
unzip -q "$zip" -d "$tmp/pkg" 2>/dev/null || [ $? -eq 1 ]
pkg="$tmp/pkg/60 FPS default"
[ -f "$pkg/apply_60fps_launcher.csx" ] || die "$zip is not the 60 FPS advanced package"

parts="launcher:data.win 1:chapter1_windows/data.win 2:chapter2_windows/data.win 3:chapter3_windows/data.win
       4:chapter4_windows/data.win 5:chapter5_windows/data.win"
m=$tmp/m; mkdir -p "$m"
wasd=0
for p in $parts; do
    key=${p%%:*} rel=${p#*:}
    script="$pkg/apply_60fps_$([ "$key" = launcher ] && echo launcher || echo "ch$key").csx"
    src="$game/$rel"
    if grep -q -a keyboard_set_map "$src"; then src="$src.pre-wasd"; wasd=1; fi
    [ -f "$src" ] || die "no $src"
    grep -q -a scr_fps_lerp_rate "$src" && die "$rel already has 60 FPS"
    grep -q -a get_lang_folder_path "$src" || die "$rel has no Russian translation, install it first"
    want=$(grep -oP 'Expected vanilla base: SHA-256 \K[0-9a-f]+' "$script")
    [ "$(sha256sum < "$vanilla/$rel" | cut -d' ' -f1)" = "$want" ] || die "$vanilla/$rel is not the vanilla file the package expects"
    cp "$src" "$tmp/ru_$key.win"

    echo "60fps-ru: merging $rel..."
    "$cli" load "$tmp/ru_$key.win" -s "$tools/listold.csx" </dev/null >"$tmp/old_$key.txt"
    grep -oP '^OLD \K.*' "$tmp/old_$key.txt" | sort -u > "$m/old_$key.names" || die "can't list translated entries of $rel"
    grep -oP '^Apply\("\K[^"]+' "$script" | sort -u | comm -12 "$m/old_$key.names" - > "$m/overlap_$key.names"
    args=(); while read -r n; do args+=(-c "$n"); done < "$m/overlap_$key.names"
    "$cli" dump "$vanilla/$rel" -o "$m/V_$key" "${args[@]}" </dev/null >/dev/null
    "$cli" dump "$tmp/ru_$key.win" -o "$m/R_$key" "${args[@]}" </dev/null >/dev/null
    python3 -I "$tools/merge.py" "$script" "$m" "$key"
done
python3 -I "$tools/resolve.py" "$m"

for p in $parts; do
    key=${p%%:*} rel=${p#*:}
    script="$pkg/apply_60fps_$([ "$key" = launcher ] && echo launcher || echo "ch$key").csx"
    echo "60fps-ru: building $rel..."
    python3 -I "$tools/build.py" "$script" "$m" "$key" "$tmp/ru60_$key.csx"
    "$cli" load "$tmp/ru_$key.win" -s "$tmp/ru60_$key.csx" -o "$tmp/out_$key.win" -f </dev/null >"$tmp/log_$key.txt" 2>&1 ||
        { cat "$tmp/log_$key.txt" >&2; die "building $rel failed, game files left unchanged"; }
done

# install only after every part built
for p in $parts; do
    key=${p%%:*} rel=${p#*:}
    [ -e "$game/$rel.pre-60fps" ] || cp "$tmp/ru_$key.win" "$game/$rel.pre-60fps"
    cp "$tmp/out_$key.win" "$game/$rel.new" && mv -f "$game/$rel.new" "$game/$rel"
done
echo "60fps-ru: done (Russian-only files kept as data.win.pre-60fps)."
if [ "$wasd" = 1 ]; then UTMT_CLI=$cli "$here/patch-wasd.sh" "$game"; fi
