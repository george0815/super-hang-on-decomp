#!/bin/sh
# One recording on the SNES port against the native port (make snes-check):
# livetest plays tests/inputs/NAME.txt with its race frames reading the pads
# as they start, logging the pad and the RAM at each game frame's end; a
# test build of the SNES port plays that pad log by game frame, and snesrun
# -F dumps its RAM at the same points; tools/snes/framecmp.py compares them.
# The dumps are left in build/framecheck/NAME (KEEP=1) or removed.
# usage: tools/snes/framecheck.sh NAME
set -e
name=$1
in=tests/inputs/$name.txt
w=build/framecheck/$name
rm -rf "$w"
mkdir -p "$w/md" "$w/mdf" "$w/sn"
# video frames: the length the script states, or past its last press
n=$(sed -n 's/.*# \([0-9]*\) frames.*/\1/p' "$in" | head -n 1)
if [ -z "$n" ]; then
    last=$(awk '/^[0-9]/ { if ($2 > m) m = $2 } END { print m + 0 }' "$in")
    n=$((last + 1500))
fi
[ "$n" -lt 3000 ] && n=3000
SHO_FRAMEPADS=1 SHO_FRAMEDUMP="$w/mdf" SHO_PADLOG="$w/pad" \
    build/livetest "${ROM:-sho.md}" "$n" "$in" "$w/md" > /dev/null
${MAKE:-make} -s snes SNES_PADLOG="$w/pad"
rm -f build/snes/shot_f*.ram
${MAKE:-make} -s snes-shots SNES_PADLOG="$w/pad" PADLOG="$w/pad" FRAMEDUMP=1 \
    INPUT="$in" FRAMES=$((n * 3)) SHOTS= > /dev/null
find build/snes -maxdepth 1 -name 'shot_f*.ram' -exec mv -t "$w/sn" {} +
echo "== $name ($n video frames)"
${PYTHON:-python3} tools/snes/framecmp.py "$w/mdf" "$w/sn"
[ -n "$KEEP" ] || rm -rf "$w"
