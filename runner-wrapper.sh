#!/bin/sh
# Installed by undertale-linux-smooth as "runner": Steam launches this instead of the game binary,
# which the installer renamed to runner.real.
#  smooth.so    - in-between frames up to SMOOTH_FPS (game logic stays at 30 ticks/s); SMOOTH_FPS=0 turns it off
#  layoutfix.so - W/A/S/D/Z/X/C work under any keyboard layout (e.g. Russian)
# smooth.so goes before Steam's overlay so the overlay still draws on top of every frame.
dir=$(dirname "$(readlink -f "$0")")
mods="$dir/linux-smooth"
export SMOOTH_FPS="${SMOOTH_FPS:-144}" SMOOTH_MODE="${SMOOTH_MODE:-interp}"
export SMOOTH_LOG="${SMOOTH_LOG:-$mods/smooth.log}"
export LD_PRELOAD="$mods/smooth.so${LD_PRELOAD:+:$LD_PRELOAD}:$mods/layoutfix.so"
exec "$dir/runner.real" "$@"
