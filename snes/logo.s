; The SEGA logo screen (game mode 0; src/game/logo.c).
;
; The logo appears pixel by pixel: up to 32 walkers (8-byte records at
; LOGO_WALKERS: x and y in the tile, byte offset of the tile, direction;
; x negative = free) trace the logo's colour-1 pixels in LOGO_BITMAP
; (Genesis 4bpp tiles, 12 tiles = $180 bytes per row) and set them in
; LOGO_SHOWN. Both stay in the Genesis's format, as in the original, and
; are converted to SNES tiles for each upload.

.p816
.smart
.include "inc/hw.inc"
.include "inc/mem.inc"
.include "gen/assets.inc"

.export logo_mode
.import wait_vblank, upload_vram, upload_flush

LOGO_BITMAP       = $3000
LOGO_SHOWN        = $4000
LOGO_WALKERS      = $5000
LOGO_FOUND        = $5100   ; first step found this turn: x, y, offset
LOGO_WALKER_COUNT = $5108
LOGO_SWEEP        = $510E   ; two sweep positions
LOGO_FLAGS        = $5112   ; bit 7: a step was found
LOGO_STEPS        = $C710
LOGO_PHASE        = $C70E
LOGO_LEVEL        = $C714
LOGO_PAUSE        = $C71C
VBLANK_HANDLER    = $C722

; SNES VRAM (word addresses): the logo's 49 tiles from 0, its map
LOGO_MAP          = $3800
LOGO_TILES_BYTES  = $620

; direct page
w           = $20       ; logo_walk: the walker
dir         = $22
tries       = $24
px          = $26
py          = $28
poff        = $2A
mask        = $2C
bit_        = $2E
src         = $30       ; logo_tiles: source (24 bits)
row01       = $34
row23       = $36
tile_out    = $38
rows_left   = $3A
up_src      = $02       ; (sa1.s's upload arguments)
up_dst      = $06
up_len      = $08

.segment "CODE"
.a16
.i16

; Game mode 0 ($6CF2): the setup, or one frame.
logo_mode:
    lda a:GAME_MODE
    lsr
    bcs @frame
    lda a:GAME_MODE
    ora #1
    sta a:GAME_MODE
    jsr logo_setup
    jmp mode_setup_done
@frame:
    jsr wait_vblank
    jmp logo_frame

; The end of a mode's setup ($6D70 and on): VBlanks on, then after the
; next one the display on.
mode_setup_done:
    lda #4
    sta a:VBLANK_HANDLER
    jsr wait_vblank
    sep #$20
    .a8
    lda #$0F
    sta f:SH_INIDISP
    rep #$20
    .a16
    rts

; ---------------------------------------------------------------- walk

; One pixel in direction dir (0 = right, then clockwise in steps of 2,
; 8 = left) from px, py in the tile at poff ($80D8).
logo_step:
    lda z:dir
    and #$000E
    tax
    jmp (@steps, x)
@steps:
    .word @r, @dr, @d, @dl, @l, @ul, @u, @ur
@r:  jmp step_right
@dr: jsr step_right
     jmp step_down
@d:  jmp step_down
@dl: jsr step_left
     jmp step_down
@l:  jmp step_left
@ul: jsr step_left
     jmp step_up
@u:  jmp step_up
@ur: jsr step_right
     jmp step_up

step_right:
    lda z:px
    inc
    cmp #8
    bcc :+
    lda z:poff
    clc
    adc #$20
    sta z:poff
    lda #0
:   sta z:px
    rts

step_left:
    lda z:px
    bne :+
    lda z:poff
    sec
    sbc #$20
    sta z:poff
    lda #8
:   dec
    sta z:px
    rts

step_down:
    lda z:py
    inc
    cmp #8
    bcc :+
    lda z:poff
    clc
    adc #$180
    sta z:poff
    lda #0
:   sta z:py
    rts

step_up:
    lda z:py
    bne :+
    lda z:poff
    sec
    sbc #$180
    sta z:poff
    lda #8
:   dec
    sta z:py
    rts

; Move the walker at X ($8002): look at the five neighbours from 90
; degrees right of its direction to 90 degrees left. Every neighbour that
; is a logo pixel not shown yet is shown; the walker moves to the first
; one, and each further one starts a new walker (waiting a turn: its y has
; the high byte set). A walker with nowhere to go is freed.
logo_walk:
    stx z:w
    lda a:6, x
    clc
    adc #4
    and #$000E
    sta z:dir
    sep #$20
    .a8
    stz a:LOGO_FLAGS
    rep #$20
    .a16
    stz z:tries
@try:
    ldx z:w
    lda a:0, x
    sta z:px
    lda a:2, x
    sta z:py
    lda a:4, x
    sta z:poff
    lda z:tries
    beq @step
    lda z:dir
    cmp #2
    bcs :+
    lda #$10                ; 0 turns to $E
:   sec
    sbc #2
    sta z:dir
@step:
    jsr logo_step
    lda z:px                ; even x: the high nibble
    lsr
    lda #$10
    ldx #$F0
    bcc :+
    lda #$01
    ldx #$0F
:   sta z:bit_
    stx z:mask
    ; the byte, at a signed offset: off the top or left edge it is below
    ; the bitmap
    lda z:py
    asl
    asl
    adc z:poff              ; (carry clear: py < 8)
    sta z:row01
    lda z:px
    lsr
    clc
    adc z:row01
    clc
    adc #LOGO_BITMAP
    tax
    sep #$20
    .a8
    lda a:0, x
    and z:mask
    cmp z:bit_
    bne @next
    lda a:LOGO_SHOWN - LOGO_BITMAP, x
    and z:mask
    cmp z:bit_
    beq @next
    lda a:LOGO_SHOWN - LOGO_BITMAP, x
    ora z:bit_
    sta a:LOGO_SHOWN - LOGO_BITMAP, x
    lda a:LOGO_FLAGS
    bmi @spawn
    ora #$80
    sta a:LOGO_FLAGS
    rep #$20
    .a16
    ldx z:w
    lda z:dir
    sta a:6, x
    lda z:px
    sta a:LOGO_FOUND
    lda z:py
    sta a:LOGO_FOUND + 2
    lda z:poff
    sta a:LOGO_FOUND + 4
    bra @next
@spawn:
    rep #$20
    .a16
    inc a:LOGO_WALKER_COUNT
    ldx #LOGO_WALKERS
:   lda a:0, x
    bmi :+
    txa
    clc
    adc #8
    tax
    bra :-
:   lda z:px
    sta a:0, x
    lda z:py
    ora #$FF00
    sta a:2, x
    lda z:poff
    sta a:4, x
    lda z:dir
    sta a:6, x
@next:
    rep #$20
    .a16
    inc z:tries
    lda z:tries
    cmp #5
    bcs :+
    jmp @try
:   ldx z:w
    sep #$20
    .a8
    lda a:LOGO_FLAGS
    bpl @free
    and #$7F
    sta a:LOGO_FLAGS
    rep #$20
    .a16
    lda a:LOGO_FOUND
    sta a:0, x
    lda a:LOGO_FOUND + 2
    sta a:2, x
    lda a:LOGO_FOUND + 4
    sta a:4, x
    rts
@free:
    rep #$20
    .a16
    dec a:LOGO_WALKER_COUNT
    lda #$FFFF
    sta a:0, x
    rts

; ---------------------------------------------------------------- frame

logo_next_mode:
    stz a:VBLANK_HANDLER
    lda #4                  ; the title screen
    sta a:GAME_MODE
    rts

; One logo frame after the VBlank ($724E). C/Start skips to the title.
; While walkers remain, they trace the logo (a new walker waits one turn);
; then the logo brightens in 14 steps of $111, and two bands of colour
; sweep across it every other frame; after a pause, the title.
logo_frame:
    sep #$20
    .a8
    lda a:PAD1_PRESSED
    and #$A0
    rep #$20
    .a16
    bne logo_next_mode
    sep #$20
    .a8
    lda a:PALETTE_DIRTY
    ora #1
    sta a:PALETTE_DIRTY
    rep #$20
    .a16
    lda a:LOGO_WALKER_COUNT
    beq @traced
    ldx #LOGO_WALKERS
@walker:
    lda a:0, x
    bmi @skip
    lda a:2, x
    bpl @walk
    and #$00FF              ; waited its turn: y back to its low byte
    cmp #$80
    bcc :+
    ora #$FF00
:   sta a:2, x
    bra @skip
@walk:
    phx
    jsr logo_walk
    plx
@skip:
    txa
    clc
    adc #8
    tax
    cpx #LOGO_WALKERS + 32 * 8
    bcc @walker
    ldx #LOGO_SHOWN
    jmp logo_tiles

@traced:
    ldx #LOGO_BITMAP
    jsr logo_tiles
    lda a:LOGO_PAUSE
    beq @brighten
    dec
    sta a:LOGO_PAUSE
    bne :+
    jmp logo_next_mode
:   rts

@brighten:
    lda a:LOGO_PHASE
    bne @sweep
    lda a:LOGO_LEVEL
    clc
    adc #$111
    sta a:LOGO_LEVEL
    ldx #0
:   sta a:PALETTE_BUFFER + $24, x
    sta a:PALETTE_BUFFER + $04, x
    inx
    inx
    cpx #14 * 2
    bcc :-
    lda a:LOGO_STEPS
    dec
    sta a:LOGO_STEPS
    bpl :+
    inc a:LOGO_PHASE
    lda #7
    sta a:LOGO_SWEEP
    lda #$0A
    sta a:LOGO_SWEEP + 2
    lda #$0D
    sta a:LOGO_STEPS
:   rts

@sweep:
    sep #$20
    .a8
    lda a:FRAME_COUNTER_BYTE
    lsr
    rep #$20
    .a16
    bcs :+
    rts
:   lda a:LOGO_STEPS
    cmp #6
    bcc :+
    lda a:PALETTE_BUFFER + $04
    sec
    sbc #$111
    sta a:PALETTE_BUFFER + $04
    lda a:PALETTE_BUFFER + $24
    sec
    sbc #$111
    sta a:PALETTE_BUFFER + $24
:   ldy #0                  ; band * 2
@band:
    lda a:LOGO_SWEEP, y
    dec
    sta a:LOGO_SWEEP, y
    sta z:px                ; (i)
    tya
    asl
    asl
    asl
    asl
    clc
    adc #PALETTE_BUFFER + 8 ; dst
    sta z:poff
    lda #4
    sta z:tries
@colour:
    inc z:px
    lda z:px
    cmp #8                  ; unsigned: also rejects i < 0
    bcs @none
    asl                     ; 6 * i
    adc z:px
    asl
    tax
    lda f:logo_sweep_colours, x
    sta z:mask
    lda f:logo_sweep_colours + 2, x
    sta z:bit_
    lda f:logo_sweep_colours + 4, x
    ldx z:poff
    sta a:4, x
    lda z:bit_
    sta a:2, x
    lda z:mask
    sta a:0, x
@none:
    lda z:poff
    clc
    adc #6
    sta z:poff
    dec z:tries
    bne @colour
    iny
    iny
    cpy #4
    bcc @band
    lda a:LOGO_STEPS
    dec
    sta a:LOGO_STEPS
    bpl :+
    lda #$3C
    sta a:LOGO_PAUSE
:   rts

; Upload the 49 logo tiles at X (LOGO_SHOWN or LOGO_BITMAP) to VRAM 0
; ($80CA), converted to SNES planar tiles: each Genesis byte (two pixels)
; looks up its bits for the four planes, by its place in the row.
logo_tiles:
    stx z:src
    lda #^MIRROR
    sta z:src + 2
    ldy #0
    stz z:tile_out
@tile:
    lda #8
    sta z:rows_left
@row:
    lda [src], y            ; pixels 0, 1
    and #$00FF
    asl
    asl
    tax
    lda f:planar_lut, x
    sta z:row01
    lda f:planar_lut + 2, x
    sta z:row23
    iny
    lda [src], y            ; pixels 2, 3
    and #$00FF
    asl
    asl
    tax
    lda f:planar_lut + $400, x
    tsb z:row01
    lda f:planar_lut + $402, x
    tsb z:row23
    iny
    lda [src], y            ; pixels 4, 5
    and #$00FF
    asl
    asl
    tax
    lda f:planar_lut + $800, x
    tsb z:row01
    lda f:planar_lut + $802, x
    tsb z:row23
    iny
    lda [src], y            ; pixels 6, 7
    and #$00FF
    asl
    asl
    tax
    lda f:planar_lut + $C00, x
    tsb z:row01
    lda f:planar_lut + $C02, x
    tsb z:row23
    iny
    ldx z:tile_out
    lda z:row01
    sta f:logo_tiles_buffer, x
    lda z:row23
    sta f:logo_tiles_buffer + 16, x
    inx
    inx
    stx z:tile_out
    dec z:rows_left
    bne @row
    txa
    clc
    adc #16                 ; past planes 2 and 3
    sta z:tile_out
    cpy #LOGO_TILES_BYTES
    bcc @tile

    lda #.loword(logo_tiles_buffer)
    sta up_src
    lda #^logo_tiles_buffer
    sta up_src + 2
    stz up_dst
    lda #LOGO_TILES_BYTES
    sta up_len
    jmp upload_vram

; ---------------------------------------------------------------- setup

; The logo screen's setup ($739A): the bitmap unpacked to RAM and an empty
; copy shown, a 12 x 4 tile block on the window plane (palette 1 for the
; lower half), palettes 0-1 black with colour 1 white, one walker at the
; logo's first pixel, the rest free.
logo_setup:
    ; the PPU: mode 1, BG1 alone from the logo map; blanked until
    ; mode_setup_done
    sep #$20
    .a8
    lda #$80
    sta f:SH_INIDISP
    lda #$01
    sta f:SH_BGMODE
    lda #(LOGO_MAP >> 10) << 2
    sta f:SH_BG1SC
    lda #0
    sta f:SH_BG12NBA
    lda #$01
    sta f:SH_TM
    rep #$20
    .a16
    lda #0
    sta f:SH_BG1HOFS
    lda #$FFFF              ; line 0 of the map on the first visible line
    sta f:SH_BG1VOFS

    ldx #0                  ; the bitmap (Nemesis at $020A56 in the
:   lda f:logo_bitmap, x    ; original, decoded when the ROM is built)
    sta a:LOGO_BITMAP, x
    stz a:LOGO_SHOWN, x
    inx
    inx
    cpx #LOGO_TILES_BYTES
    bcc :-
    ldx #LOGO_SHOWN
    jsr logo_tiles

    ; the map: tiles 1-24 in palette 0, 25-48 in palette 1, at row 12,
    ; column 14 of the Genesis's 40 (10 of the SNES's 32)
    ldx #0
    lda #0
:   sta f:logo_map_buffer, x
    inx
    inx
    cpx #32 * 32 * 2
    bcc :-
    lda #1
    ldx #(12 * 32 + 10) * 2
@map_row:
    ldy #12
:   sta f:logo_map_buffer, x
    inc
    inx
    inx
    dey
    bne :-
    pha
    txa
    clc
    adc #(32 - 12) * 2
    tax
    pla
    cmp #25
    bne :+
    ora #$0400              ; palette 1
:   cpx #(16 * 32) * 2
    bcc @map_row
    lda #.loword(logo_map_buffer)
    sta up_src
    lda #^logo_map_buffer
    sta up_src + 2
    lda #LOGO_MAP
    sta up_dst
    lda #32 * 32 * 2
    sta up_len
    jsr upload_vram

    ; palettes 0 and 1: colour 1 white, the rest black
    ldx #0
:   stz a:PALETTE_BUFFER, x
    inx
    inx
    cpx #$40
    bcc :-
    lda #$0EEE
    sta a:PALETTE_BUFFER + 2
    sta a:PALETTE_BUFFER + $22
    sep #$20
    .a8
    lda a:PALETTE_DIRTY
    ora #1
    sta a:PALETTE_DIRTY
    rep #$20
    .a16

    lda #1
    sta a:LOGO_WALKER_COUNT
    ldx #0
:   lda f:logo_first_walker, x
    sta a:LOGO_WALKERS, x
    inx
    inx
    cpx #8
    bcc :-
    lda #$FFFF
:   sta a:LOGO_WALKERS, x
    txa
    clc
    adc #8
    tax
    lda #$FFFF
    cpx #32 * 8
    bcc :-
    stz a:LOGO_PHASE
    lda #$0D
    sta a:LOGO_STEPS
    stz a:LOGO_LEVEL
    stz a:LOGO_PAUSE
    rts

.segment "BWRAM"
logo_tiles_buffer:  .res LOGO_TILES_BYTES
logo_map_buffer:    .res 32 * 32 * 2
