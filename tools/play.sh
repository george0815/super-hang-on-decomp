#!/bin/sh
# Play the game and turn the session into a reference recording.
#   tools/play.sh NAME [PREFIX.txt]
# Saves the inputs to tests/inputs/NAME.txt (optionally starting from an
# existing script), then records build/ref/NAME.* with tools/record.sh.
# Keys: arrows, Z = A, X = B, C = C, Enter = Start, Esc = finish.
set -e
name=$1
[ -n "$name" ] || { echo "usage: $0 NAME [PREFIX.txt]"; exit 1; }
make -s -C tools/trace gpgx_play
script=tests/inputs/$name.txt
if [ -n "$2" ]; then
    tools/trace/gpgx_play sho.md "$script" -i "$2"
else
    tools/trace/gpgx_play sho.md "$script"
fi
frames=$(sed -n 's/^# \([0-9]*\) frames$/\1/p' "$script")
tools/record.sh "$name" "$frames"
