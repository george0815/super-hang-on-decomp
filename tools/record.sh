#!/bin/sh
# Record a reference session with Genesis Plus GX for an input script.
#   tools/record.sh NAME FRAMES      uses tests/inputs/NAME.txt
# writes build/ref/NAME.{events,vtrace,samples,snd,zram}
set -e
name=$1; frames=$2
mkdir -p build/ref
tools/trace/gpgx_fncap sho.md "$frames" build/recomp/entries.txt "build/ref/$name.samples" \
    -i "tests/inputs/$name.txt" -v "build/ref/$name.vtrace" -e "build/ref/$name.events" \
    -s "build/ref/$name.snd" -z "build/ref/$name.zram"
