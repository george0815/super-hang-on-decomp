; Screen helpers the modes share (src/game/vblank.c, video.c, unpack.c,
; nemesis.c), on top of the VDP layer in vdp.s.
;
; Genesis addresses (24 bits) are passed as A = the high word (bank) and
; X = the low word.

.p816
.smart
.include "inc/hw.inc"
.include "inc/mem.inc"
.include "inc/dp.inc"
.include "inc/macros.inc"
.include "gen/assets.inc"

.export screen_reset, display_on, plane_a_fill, palette_load, palette_words
.export nemesis_to_vdp, map_unpack, rle_unpack, objects_clear
.import vdp_command, vdp_register, vdp_write, vdp_fill, vdp_clear, vdp_tiles
.import sprites_hide

cmd_hi      = $46       ; (vdp.s's arguments)
cmd_lo      = $6C

; direct page: $80-$9F
v_src       = $80       ; 24 bits
v_dst       = $84
v_count     = $86
v_out       = $88

VDP_MODE_SHADOW   = $C700   ; copies of the mode register commands
SPRITE_BUFFER     = $D100
OBJECTS           = $C900
UNPACK_BUFFER     = $FB00
UNPACK_MAP        = $EB00

.segment "CODE"
.a16
.i16

; Display off, scroll and sprite tables cleared, plane B and the top of
; plane A empty, the window over the whole screen filled with $E000
; ($8380 and on; with interrupts masked in the original).
screen_reset:
    sep #$20
    .a8
    lda a:VDP_MODE_SHADOW + 3
    and #$BF
    sta a:VDP_MODE_SHADOW + 3
    rep #$20
    .a16
    ldw VDP_MODE_SHADOW + 2
    jsr vdp_register
    lda #$8AFF
    jsr vdp_register
    lda #$380               ; the horizontal scroll table
    ldx #$CC00
    jsr vdp_clear
    lda #$4000              ; VSRAM
    sta z:cmd_hi
    lda #$0010
    sta z:cmd_lo
    lda #0
    ldy #$29
    jsr vdp_fill
    ldx #0                  ; the sprite buffer
:   stz a:SPRITE_BUFFER, x
    inx
    inx
    cpx #$140 * 2
    bcc :-
    jsr sprites_hide
    lda #$140               ; the sprite table
    ldx #$C800
    jsr vdp_clear
    lda #$280
    ldx #$C000
    jsr vdp_clear
    lda #$2000              ; plane B
    ldx #$E000
    jsr vdp_clear
    lda #$E000
    ; fall through

; The window's 28 rows of 40 entries filled with A, the window over the
; whole screen ($8344).
plane_a_fill:
    pha
    lda #$921C
    jsr vdp_register
    lda #$5000
    sta z:cmd_hi
    lda #$0003
    sta z:cmd_lo
    lda #28
    sta z:v_count
:   lda 1, s
    ldy #$28
    jsr vdp_fill
    lda z:cmd_hi
    clc
    adc #$0080
    sta z:cmd_hi
    dec z:v_count
    bne :-
    pla
    rts

; Display on ($8430).
display_on:
    sep #$20
    .a8
    lda a:VDP_MODE_SHADOW + 3
    ora #$40
    sta a:VDP_MODE_SHADOW + 3
    rep #$20
    .a16
    ldw VDP_MODE_SHADOW + 2
    jmp vdp_register

; Clear every object ($70FC).
objects_clear:
    ldx #0
:   stz a:OBJECTS, x
    inx
    inx
    cpx #29 * $40
    bcc :-
    rts

; ------------------------------------------------------------- palette

; A palette record at A:X ($8448): byte offset into the palette buffer,
; count - 1, then the colours. Marks the palette for upload; returns the
; next record in A:X.
palette_load:
    jsr far_source
    sep #$20
    .a8
    lda a:PALETTE_DIRTY
    ora #1
    sta a:PALETTE_DIRTY
    rep #$20
    .a16
    lda [v_src]
    and #$00FF
    clc
    adc #PALETTE_BUFFER
    tax
    lda [v_src]
    xba
    and #$00FF
    inc
    sta z:v_count
    ldy #2
:   lda [v_src], y          ; (big-endian on both sides)
    sta a:0, x
    inx
    inx
    iny
    iny
    dec z:v_count
    bne :-
    tya
    clc
    adc z:v_src
    tax
    lda z:v_src + 2
    sec
    sbc #^MDROM
    rts

; Copy Y colours from the Genesis address A:X to the palette buffer at
; v_dst, and mark it ($738A).
palette_words:
    sty z:v_count
    jsr far_source
    ldx z:v_dst
    ldy #0
:   lda [v_src], y
    sta a:0, x
    inx
    inx
    iny
    iny
    dec z:v_count
    bne :-
    sep #$20
    .a8
    lda a:PALETTE_DIRTY
    ora #1
    sta a:PALETTE_DIRTY
    rep #$20
    .a16
    rts

; v_src = where Genesis address A:X is: the RAM mirror or the ROM copy.
far_source:
    stx z:v_src
    cmp #$00FF
    bne :+
    lda #^MIRROR
    sta z:v_src + 2
    rts
:   clc
    adc #^MDROM
    sta z:v_src + 2
    rts

; ------------------------------------------------------------- Nemesis

; Decompress the Nemesis block at Genesis address A:X to the VDP's current
; address ($855E): the build decoded it, so find it and copy it.
nemesis_to_vdp:
    sta z:v_count
    stx z:v_dst
    ldx #0
@find:
    lda f:nemesis_index, x
    cmp #$FFFF
    beq @missing
    cmp z:v_dst
    bne @next
    lda f:nemesis_index + 2, x
    cmp z:v_count
    beq @found
@next:
    txa
    clc
    adc #10
    tax
    bra @find
@found:
    lda f:nemesis_index + 8, x
    tay
    lda f:nemesis_index + 6, x
    pha
    lda f:nemesis_index + 4, x
    tax
    pla
    jmp vdp_tiles
@missing:
    brk                     ; a block the build didn't decode

; ------------------------------------------------------------ unpacking

; Unpack the run-length data at Genesis address A:X to the RAM mirror at
; v_out ($8538): 0 ends; $80+n copies the next n bytes; n (1-$7F) writes n
; bytes counting up from the next byte; a count of 0 means 256. Returns
; the end of the output in v_out.
rle_unpack:
    jsr far_source
    ldy #0
    ldx z:v_out
@code:
    lda [v_src], y
    iny
    and #$00FF
    beq @done
    pha
    and #$007F
    dec
    and #$00FF
    inc
    sta z:v_count           ; n, 0 meaning 256
    pla
    and #$0080
    bne @copy
    lda [v_src], y          ; the first value
    iny
    sep #$20
    .a8
:   sta a:0, x
    inx
    inc
    dec z:v_count
    bne :-
    rep #$20
    .a16
    bra @code
@copy:
    sep #$20
    .a8
:   lda [v_src], y
    iny
    sta a:0, x
    inx
    dec z:v_count
    bne :-
    rep #$20
    .a16
    bra @code
@done:
    stx z:v_out
    rts

; A word map in two byte planes at Genesis address A:X ($84F0): unpacked
; to $FFFB00, then those runs (0 ends a plane; $80+n literal bytes, n
; repeats of the next byte) expanded into the high bytes, then the low
; bytes, of the words at $FFEB00.
map_unpack:
    pha
    lda #UNPACK_BUFFER
    sta z:v_out
    pla
    jsr rle_unpack
    ldy #UNPACK_BUFFER
    ldx #UNPACK_MAP
    jsr @plane
    ldx #UNPACK_MAP + 1
@plane:
    lda a:0, y
    iny
    and #$00FF
    beq @end
    pha
    and #$007F
    dec
    and #$00FF
    inc
    sta z:v_count
    pla
    and #$0080
    bne @literal
    lda a:0, y              ; repeat the next byte
    iny
    sep #$20
    .a8
:   sta a:0, x
    inx
    inx
    dec z:v_count
    bne :-
    rep #$20
    .a16
    bra @plane
@literal:
    sep #$20
    .a8
:   lda a:0, y
    iny
    sta a:0, x
    inx
    inx
    dec z:v_count
    bne :-
    rep #$20
    .a16
    bra @plane
@end:
    rts
