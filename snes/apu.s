; The S-CPU's side of the sound: loads the SPC700 program and samples
; (snes/spc/driver.spc) through the boot ROM's upload protocol, and passes
; the DSP writes the SA-1 queues (DSP_RING) on to it.
;
; Called with A 8-bit, X/Y 16-bit, the data bank and direct page 0.

.p816
.smart
.include "inc/hw.inc"
.include "inc/mem.inc"
.include "inc/dp.inc"
.include "gen/spc.inc"

.export apu_upload, apu_pump, apu_write, apu_driver
.import sound_vblank_snes

SOUND_DP = $0100            ; the compiled sound code's direct page (WRAM)

APUIO0 = $2140
APUIO1 = $2141
APUIO2 = $2142
APUIO3 = $2143

.segment "ZEROPAGE"
apu_count:  .res 1          ; the last count sent on port 0
apu_src:    .res 3

.segment "CODE"
.a8
.i16

; The program and samples into ARAM at SPC_ORG, then started at SPC_START,
; and the DSP set up: no echo, the main volume up, the directory at $0300.
apu_upload:
    ldx #$BBAA              ; the boot ROM is ready
:   cpx APUIO0
    bne :-
    ldx #SPC_ORG
    stx APUIO2
    lda #1                  ; a block follows
    sta APUIO1
    lda #$CC
    sta APUIO0
:   cmp APUIO0
    bne :-
    ldx #0
    ldy #0                  ; (the count: the byte index's low byte)
@byte:
    lda f:spc_image, x
    sta APUIO1
    tya
    sta APUIO0
:   cmp APUIO0
    bne :-
    iny
    inx
    cpx #SPC_SIZE
    bne @byte
    ldx #SPC_START          ; the end: start the program
    stx APUIO2
    stz APUIO1
    tya
    inc
    inc                     ; (a count above the last, not zero)
    bne :+
    inc
:   sta APUIO0
    sta apu_count
:   cmp APUIO0
    bne :-
    ; the DSP's setup
    ldx #0
:   lda f:dsp_setup, x
    cmp #$FF
    beq :+
    xba
    lda f:dsp_setup + 1, x
    jsr apu_write_ba
    inx
    inx
    bra :-
:   rts

; DSP register B = A, through the program (A and B kept).
apu_write_ba:
    xba
    sta APUIO1
    xba
apu_write:                  ; (port 1 already the register)
    sta APUIO2
    lda apu_count
    inc
    sta apu_count
    sta APUIO0
:   cmp APUIO0
    bne :-
    rts

; The writes the SA-1 has queued since the last call.
apu_pump:
    lda f:DSP_TAIL
    cmp f:DSP_HEAD
    beq @done
    rep #$20
    .a16
    and #$00FF
    asl
    tax
    sep #$20
    .a8
    lda f:DSP_RING, x       ; the register
    sta APUIO1
    lda f:DSP_RING + 1, x   ; the value
    jsr apu_write
    lda f:DSP_TAIL
    inc
    sta f:DSP_TAIL
    bra apu_pump
@done:
    rts

; The sound driver's work for this VBlank (snes/sound.c), run here so
; that the SA-1 keeps its time for the game: the compiled code with its
; own direct page (arithmetic by shifts: SOFT_MATH) and the data bank $40.
apu_driver:
    phb
    phd
    rep #$30
    .a16
    lda #SOUND_DP
    tcd
    lda #1
    sta z:SOFT_MATH
    sep #$20
    .a8
    lda #$40
    pha
    plb
    rep #$20
    .a16
    jsl sound_vblank_snes
    sep #$20
    .a8
    pld
    plb
    rts

dsp_setup:
    .byte $6C, $20          ; FLG: no echo writes
    .byte $2C, $00, $3C, $00    ; echo volume
    .byte $4D, $00, $3D, $00, $2D, $00  ; no echo, noise, pitch modulation
    .byte $5D, $03          ; DIR: $0300
    .byte $6D, $FF, $7D, $00    ; echo buffer (unused)
    .byte $5C, $00          ; KOF
    .byte $0C, $7F, $1C, $7F    ; main volume
.ifdef SOUND_TEST
    .byte $00, $40, $01, $40, $02, $00, $03, $08, $04, $00, $05, $8F, $06, $E0
    .byte $4C, $01
.endif
    .byte $FF

.segment "ASSETS"
spc_image:
    .incbin "gen/spc.bin"
