; The music select screen (game mode $0C; src/game/music.c).

.p816
.smart
.include "inc/mem.inc"
.include "inc/dp.inc"
.include "inc/macros.inc"

.export music_graphics
.import vdp_command, vdp_read, vdp_write, nemesis_to_vdp, palette_load

; direct page: $B0-$BF
m_src       = $B0
m_dst       = $B2
m_count     = $B4
m_words     = $B6

.segment "CODE"
.a16
.i16

.macro command c
    lda #.hiword(c)
    ldx #.loword(c)
    jsr vdp_command
.endmacro

.macro nemesis address
    lda #.hiword(address)
    ldx #.loword(address)
    jsr nemesis_to_vdp
.endmacro

; The screen's graphics ($7F2E): four Nemesis sets, 25 tiles' worth of
; words copied within VRAM, and the palette.
music_graphics:
    command $7D800002
    nemesis $01D948
    command $42800003
    nemesis $01DA32
    command $5E000003
    nemesis $01DDA2
    command $40200000
    nemesis $01DE10
    lda #$0020              ; read from $00200000 on, write to $50600003 on
    sta z:m_src
    lda #$5060
    sta z:m_dst
    lda #25
    sta z:m_count
@copy:
    lda z:m_src
    ldx #0
    jsr vdp_command
    ldx #0
:   phx
    jsr vdp_read
    plx
    sta f:copy_buffer, x
    inx
    inx
    cpx #32
    bcc :-
    lda z:m_dst
    ldx #3
    jsr vdp_command
    ldx #0
:   lda f:copy_buffer, x
    phx
    jsr vdp_write
    plx
    inx
    inx
    cpx #32
    bcc :-
    lda z:m_src
    clc
    adc #$0020
    sta z:m_src
    lda z:m_dst
    clc
    adc #$0080
    sta z:m_dst
    dec z:m_count
    bne @copy
    lda #$0000
    ldx #$8E56
    jmp palette_load

.segment "BWRAM"
copy_buffer: .res 32
