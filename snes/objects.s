; Game objects (src/game/objects.c).

.p816
.smart
.include "inc/mem.inc"
.include "inc/dp.inc"
.include "inc/macros.inc"

.export objects_update

.segment "CODE"
.a16
.i16

; Run every object, then build the sprite table ($716E). Not ported yet.
objects_update:
    rts
