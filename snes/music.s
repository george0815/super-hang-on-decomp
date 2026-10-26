; The music select screen (game mode $0C; src/game/music.c).

.p816
.smart
.include "inc/mem.inc"
.include "inc/dp.inc"
.include "inc/macros.inc"

.export music_graphics, music_select_mode
.import vdp_command, vdp_read, vdp_write, vdp_fill, vdp_copy, vdp_copy_rect
.import nemesis_to_vdp, palette_load, screen_reset, map_unpack
.import wait_vblank, mode_first_call, mode_setup_done
.import menu_countdown, push_start_blink, fade_out, sound_command, next_mode

cmd_hi      = $46       ; (vdp.s's arguments)
cmd_lo      = $6C
width       = $52
rows        = $54

MUSIC_CHOICE     = $0516    ; 0-3
MUSIC_SOUND      = $0518
ROM_MUSIC_SOUNDS = $007E2A  ; sound command per choice

; direct page: $B0-$BF
m_src       = $B0
m_dst       = $B2
m_count     = $B4
m_words     = $B6
m_d         = $B8

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

; Game mode $0C ($6CFE): the setup, or one frame.
music_select_mode:
    jsr mode_first_call
    bne @frame
    jsr music_select_setup
    lda #4
    jmp mode_setup_done
@frame:
    jsr wait_vblank
    jmp music_select_frame

; The music select screen's setup ($7E8E); the first tune starts playing.
music_select_setup:
    jsr screen_reset
    jsr music_names
    lda #$0003
    ldx #$D718
    jsr map_unpack
    command $40000000
    nemesis $03DA6E
    lda #$521C
    sta z:cmd_hi
    lda #$0003
    sta z:cmd_lo
    lda #$0000
    ldx #$7F16
    ldy #12
    jsr vdp_copy
    lda #$0000
    ldx #$8E56
    jsr palette_load
    lda #$0003
    ldx #$D70E
    jsr palette_load
    lda #$AE00              ; $00AE, big-endian
    sta a:$D3E0
    lda #$FFFF
    sta a:MUSIC_CHOICE
    stz a:$C718             ; countdown 11 ($00001101)
    lda #$0111
    sta a:$C71A
    lda #1
    jmp music_step

; One music select frame after the VBlank ($7DBA): when the countdown runs
; out, the tune is confirmed as with Start.
music_select_frame:
    jsr menu_countdown
    sep #$20
    .a8
    lda a:$C718
    rep #$20
    .a16
    beq :+
    jmp music_confirm
:   jsr music_select_input
    jsr music_draw
    jmp push_start_blink

; Confirm: fade out and on to the race.
music_confirm:
    lda #$B0
    jsr sound_command
    jsr fade_out
    lda #$10
    jmp next_mode

; Start or C: confirm; up/down change the tune, which starts playing at
; once ($7DCC).
music_select_input:
    sep #$20
    .a8
    lda a:PAD1_PRESSED
    rep #$20
    .a16
    bit #$A0
    bne music_confirm
    bit #1
    beq :+
    lda #$FFFF
    bra music_step
:   bit #2
    bne :+
    rts
:   lda #1
    ; fall through

; Step the tune choice by A and play it ($7E08).
music_step:
    sta z:m_d
    ldw MUSIC_CHOICE
    clc
    adc z:m_d
    and #3
    stw MUSIC_CHOICE
    tax
    lda f:MDROM + ROM_MUSIC_SOUNDS, x
    sep #$20
    .a8
    sta a:MUSIC_SOUND
    rep #$20
    .a16
    and #$00FF
    jmp sound_command

; The four tune names from their RAM copy ($7F00).
music_names:
    lda #$5388
    sta z:cmd_hi
    lda #$0003
    sta z:cmd_lo
    lda #$1F
    sta z:width
    lda #$0C
    sta z:rows
    lda #$00FF
    ldx #$EFC8
    jmp vdp_copy_rect

; Redraw the names, the selected one blanked every other 8 frames ($7E2E).
music_draw:
    jsr music_names
    sep #$20
    .a8
    lda a:FRAME_COUNTER_BYTE
    and #8
    rep #$20
    .a16
    beq :+
    rts
:   lda a:MUSIC_CHOICE      ; (the low byte)
    xba
    and #3
    asl
    tax
    lda f:blink_rows, x
    sta z:cmd_hi
    lda #$0003
    sta z:cmd_lo
    lda #3
    sta z:m_count
:   lda #0
    ldy #$1F
    jsr vdp_fill
    lda z:cmd_hi
    clc
    adc #$0080
    sta z:cmd_hi
    dec z:m_count
    bne :-
    rts

blink_rows: .word $5388, $5508, $5688, $5808

.segment "BWRAM"
copy_buffer: .res 32
