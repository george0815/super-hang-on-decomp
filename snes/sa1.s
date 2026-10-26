; The SA-1: the game's main loop (src/game/system.c reset()), and the
; services the modes share: waiting for VBlank, the upload list, the
; palette.
;
; Conventions: native mode, A and X/Y 16-bit unless marked, the data bank
; at $40 (the Genesis RAM mirror, see inc/mem.inc), the direct page in
; I-RAM at SA1_DP.

.p816
.smart
.include "inc/hw.inc"
.include "inc/mem.inc"

.export sa1_reset, wait_vblank, upload_vram, upload_flush
.import logo_mode

; direct page (SA1_DP + offset)
up_pos      = $00       ; next free byte in the upload list
up_src      = $02       ; arguments of upload_vram: source (24 bits),
up_dst      = $06       ; VRAM word address,
up_len      = $08       ; length in bytes
colour      = $0A
channel     = $0C
channel2    = $0E

.segment "CODE"

.a8
.i8
sa1_reset:
    clc
    xce
    rep #$38
    .a16
    .i16
    ldx #SA1_STACK
    txs
    lda #SA1_DP
    tcd
    sep #$20
    .a8
    lda #$80
    sta CBWE
    lda #$FF
    sta CIWP
    lda #^MIRROR
    pha
    plb
    rep #$20
    .a16

    ; the Genesis's RAM starts cleared
    ldx #0
:   stz a:0, x
    inx
    inx
    bne :-
    stz up_pos

main_loop:
    sep #$20
    .a8
    lda a:FRAME_COUNTER_BYTE
    inc
    sta a:FRAME_COUNTER_BYTE
    rep #$20
    .a16
    lda a:GAME_MODE
    and #$7FFC
    lsr
    tax
    cpx #modes_end - modes
    bcs unported
    jsr (modes, x)
    bra main_loop

; Game modes not ported yet: keep the screen as it is.
unported:
    jsr wait_vblank
    bra main_loop

modes:
    .word logo_mode
modes_end:

; Wait for the next VBlank ($6D4E). The frame's uploads go out in it: the
; palette if marked, and the list, which the S-CPU's NMI runs before it
; counts the VBlank.
wait_vblank:
    sep #$20
    .a8
    lda a:PALETTE_DIRTY
    lsr
    bcc :+
    asl
    sta a:PALETTE_DIRTY
    rep #$20
    .a16
    jsr palette_upload
:   rep #$20
    .a16
    jsr upload_flush
    stz a:VBLANK_COUNT
:   lda a:VBLANK_COUNT
    beq :-
    sep #$20
    .a8
:   lda f:UPLOAD_READY      ; (if a VBlank came before the count was cleared)
    bne :-
    rep #$20
    .a16
    stz up_pos
    rts

; End the upload list and hand it to the next NMI, which clears
; UPLOAD_READY when it is done.
upload_flush:
    ldx up_pos
    sep #$20
    .a8
    lda #UP_END
    sta f:UPLOADS, x
    lda #1
    sta f:UPLOAD_READY
    rep #$20
    .a16
    rts

; Queue a VRAM upload: up_len bytes from up_src to VRAM word up_dst.
upload_vram:
    lda #UP_VRAM
upload:
    ldx up_pos
    sta f:UPLOADS, x
    lda up_src
    sta f:UPLOADS + 1, x
    lda up_src + 1
    sta f:UPLOADS + 2, x
    lda up_dst
    sta f:UPLOADS + 4, x
    lda up_len
    sta f:UPLOADS + 6, x
    txa
    clc
    adc #8
    sta up_pos
    rts

; The Genesis palette buffer as SNES colours, to CGRAM 0-63 (the BG
; palettes) and 128-191 (the OBJ palettes).
palette_upload:
    ldx #0
@colour:
    lda a:PALETTE_BUFFER, x     ; ----BBB-GGG-RRR-
    sta z:colour
    and #$000E
    jsr level
    sta f:cgram_buffer, x
    lda z:colour
    lsr
    lsr
    lsr
    lsr
    and #$000E
    jsr level
    asl
    asl
    asl
    asl
    asl
    ora f:cgram_buffer, x
    sta f:cgram_buffer, x
    lda z:colour
    xba
    and #$000E
    jsr level
    xba
    asl
    asl
    ora f:cgram_buffer, x
    sta f:cgram_buffer, x
    inx
    inx
    cpx #128
    bne @colour

    lda #.loword(cgram_buffer)
    sta up_src
    lda #^cgram_buffer
    sta up_src + 2
    stz up_dst
    lda #128
    sta up_len
    lda #UP_CGRAM
    jsr upload
    lda #128
    sta up_dst
    lda #UP_CGRAM
    jmp upload

; A Genesis colour channel, 3 bits at bit 1 (A = 2v), as 5 bits:
; v << 2 | v >> 1.
level:
    sta z:channel
    asl
    sta z:channel2
    lda z:channel
    lsr
    lsr
    ora z:channel2
    rts

.segment "BWRAM"
cgram_buffer: .res 128
