; The course select screen (game mode 8; src/game/title.c).

.p816
.smart
.include "inc/mem.inc"
.include "inc/dp.inc"
.include "inc/macros.inc"

.export course_select_mode
.import wait_vblank, mode_first_call, mode_setup_done
.import vdp_command, vdp_copy, vdp_copy_rect
.import screen_reset, palette_load, nemesis_to_vdp, map_unpack
.import menu_countdown, push_start_blink, sound_command, next_mode

cmd_hi      = $46       ; (vdp.s's arguments)
cmd_lo      = $6C
width       = $52
rows        = $54

COURSE      = $060C

; direct page: $A8-$AF
c_k         = $A8

.segment "CODE"
.a16
.i16

; Game mode 8 ($6CFA): the setup, or one frame.
course_select_mode:
    jsr mode_first_call
    bne @frame
    jsr course_select_setup
    lda #4
    jmp mode_setup_done
@frame:
    jsr wait_vblank
    jmp course_select_frame

; The course select labels ($7C5E): VRAM command, word count, text;
; drawn last to first.
labels:
    .dword $55A20003, $007D9A
    .word 9
    .dword $568C0003, $007D88
    .word 9
    .dword $54B20003, $007D76
    .word 9
    .dword $58A20003, $007D66
    .word 8
    .dword $55240003, $007D5A
    .word 6
    .dword $560E0003, $007D4E
    .word 6
    .dword $54340003, $007D42
    .word 6
    .dword $58220003, $007D32
    .word 8
    .dword $51980003, $007CEE
    .word 17

; The course select screen's setup ($7BD8): Africa first, a countdown of
; 16 and its music.
course_select_setup:
    jsr screen_reset
    lda #$4000
    ldx #$0000
    jsr vdp_command
    lda #$0003
    ldx #$DA6E
    jsr nemesis_to_vdp
    lda #$0003
    ldx #$D718
    jsr map_unpack
    lda #$6306
    sta z:cmd_hi
    lda #$0003
    sta z:cmd_lo
    lda #$22
    sta z:width
    lda #$12
    sta z:rows
    lda #$00FF
    ldx #$EB00
    jsr vdp_copy_rect
    lda #8 * 10
    sta z:c_k
@label:
    ldx z:c_k
    lda f:labels + 2, x     ; (.dword: low word first)
    sta z:cmd_hi
    lda f:labels, x
    sta z:cmd_lo
    lda f:labels + 8, x
    tay
    lda f:labels + 6, x
    pha
    lda f:labels + 4, x
    tax
    pla
    jsr vdp_copy
    lda z:c_k
    sec
    sbc #10
    sta z:c_k
    bpl @label
    lda #$0000
    ldx #$8E56
    jsr palette_load
    lda #$0003
    ldx #$D704
    jsr palette_load
    lda #$0ECA
    stw $D3E0
    stz a:COURSE
    stz a:$C718             ; countdown 16 ($00001601)
    lda #$0116
    sta a:$C71A
    lda #$81
    jmp sound_command

; One course select frame after the VBlank ($7B24): the countdown, then on
; to music select when it runs out.
course_select_frame:
    jsr menu_countdown
    sep #$20
    .a8
    lda a:$C718
    rep #$20
    .a16
    beq :+
    lda #$0C
    jmp next_mode
:   jsr course_select_input
    jsr course_select_colours
    jmp push_start_blink

; Start or C: on to the next mode; left/up and right/down pick one of the
; four courses ($7B38).
course_select_input:
    sep #$20
    .a8
    lda a:PAD1_PRESSED
    rep #$20
    .a16
    bit #$A0
    beq :+
    lda #$0C
    jmp next_mode
:   bit #$05
    beq :+
    lda #$FFFF
    bra @step
:   bit #$0A
    bne :+
    rts
:   lda #1
@step:
    sta z:c_k
    ldw COURSE
    clc
    adc z:c_k
    and #3
    stw COURSE
    rts

; The four course names in their colours, the selected one flashing white
; every 8 frames ($7B82).
course_select_colours:
    sep #$20
    .a8
    lda a:PALETTE_DIRTY
    ora #1
    sta a:PALETTE_DIRTY
    rep #$20
    .a16
    ldx #0
:   lda f:MDROM + $7BB4, x  ; (big-endian on both sides)
    sta a:$D398, x
    inx
    inx
    cpx #8
    bcc :-
    sep #$20
    .a8
    lda a:FRAME_COUNTER_BYTE
    and #8
    rep #$20
    .a16
    beq :+
    ldw COURSE
    asl
    tax
    lda #$CA0E              ; $0ECA, big-endian
    sta a:$D398, x
:   rts
