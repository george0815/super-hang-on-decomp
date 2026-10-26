; The title screen (game mode 4; src/game/title.c).

.p816
.smart
.include "inc/hw.inc"
.include "inc/mem.inc"
.include "inc/dp.inc"
.include "inc/macros.inc"

.export title_mode
.import wait_vblank, mode_first_call, mode_setup_done
.import vdp_command, vdp_write, vdp_copy, vdp_fill, vdp_copy_rect
.import screen_reset, palette_load, palette_words, nemesis_to_vdp, map_unpack
.import objects_clear, objects_update, music_graphics

cmd_hi      = $46       ; (vdp.s's arguments)
cmd_lo      = $6C
width       = $52
rows        = $54
v_dst       = $84       ; (video.s's)

TITLE_STEP       = $C72C   ; byte: intro step x4
TITLE_COUNT      = $C712
TITLE_TIMER      = $C714
TITLE_PHASE      = $C716
TITLE_CYCLE      = $C70E   ; colour cycling: frames to the next step,
TITLE_CYCLE_POS  = $C710   ; and the position in the ramp
TITLE_DELAY      = $C71C   ; frames before the intro steps start
DEMO_DELAY       = $C82A
MENU_STATE       = $B600   ; bit 1: second menu; bits 0/2: cursor
SOUND_LOCKED     = $C754

; direct page: $A0-$AF
t_obj       = $A0
t_menu      = $A2
t_top       = $A4
t_bottom    = $A6

.segment "CODE"
.a16
.i16

; Game mode 4 ($6CF6): the setup, or one frame.
title_mode:
    jsr mode_first_call
    bne @frame
    jsr title_setup
    lda #4
    jmp mode_setup_done
@frame:
    jsr wait_vblank
    jmp title_frame

; ---------------------------------------------------------------- setup

; Copy A words from the Genesis address in the table after the command.
.macro copy_words command, source, words
    lda #.hiword(command)
    sta z:cmd_hi
    lda #.loword(command)
    sta z:cmd_lo
    lda #.hiword(source)
    ldx #.loword(source)
    ldy #words
    jsr vdp_copy
.endmacro

.macro copy_rect command, source, w, h
    lda #.hiword(command)
    sta z:cmd_hi
    lda #.loword(command)
    sta z:cmd_lo
    lda #w
    sta z:width
    lda #h
    sta z:rows
    lda #.hiword(source)
    ldx #.loword(source)
    jsr vdp_copy_rect
.endmacro

.macro command c
    lda #.hiword(c)
    ldx #.loword(c)
    jsr vdp_command
.endmacro

; The title screen's setup ($793E): the background, menu graphics, logo
; tiles and texts, the palette, and the demo after 3 seconds.
title_setup:
    lda #1                  ; the VDP layer on from here
    sta z:vdp_on
    jsr screen_reset
    stz a:SOUND_LOCKED
    stz a:$C82C
    stz a:$C8B6
    command $40000000
    lda #$0001
    ldx #$E07C
    jsr nemesis_to_vdp
    lda #$0001
    ldx #$E03C
    jsr map_unpack
    jsr music_graphics
    lda #$00EE
    stw $D3C4
    command $62400001
    lda #$0004
    ldx #$BC00
    jsr nemesis_to_vdp
    jsr objects_clear
    sep #$20
    .a8
    stz a:TITLE_STEP
    rep #$20
    .a16
    copy_rect $62160003, $FFEBB0, 16, 3
    copy_rect $5A1E0003, $FFEB60, 10, 4
    lda #$4000
    sta z:cmd_hi
    stz z:cmd_lo
    lda #$1111
    ldy #$20
    jsr vdp_fill
    copy_words $57A00003, $007ABC, 8
    copy_words $58A00003, $007ACC, 8
    copy_words $5CB60003, $007AA6, 11
    command $66460003
    lda #$E631
    jsr vdp_write
    lda #$E62A
    jsr vdp_write
    lda #$0000
    ldx #$78D6
    jsr palette_load
    ldx #0
:   lda f:MDROM + $790C, x  ; (big-endian on both sides)
    sta a:$D3B0, x
    lda f:MDROM + $7926, x
    sta a:$D3A2, x
    inx
    inx
    cpx #7 * 2
    bcc :-
    lda #$B4                ; the demo after 3 seconds
    stw DEMO_DELAY
    stz a:TITLE_CYCLE
    stz a:TITLE_CYCLE_POS
    lda #$23
    stw TITLE_DELAY
    stz a:MENU_STATE
    rts

; ---------------------------------------------------------------- frame

; Any menu activity keeps the demo away for at least 2 seconds ($77F4).
demo_delay:
    ldw DEMO_DELAY
    cmp #$78
    bcs :+
    lda #$78
    stw DEMO_DELAY
:   rts

; On to mode A.
next_mode:
    stw GAME_MODE
    stz a:VBLANK_HANDLER
    rts

; One title screen frame after the VBlank ($74A0). First menu: new game
; or password (A held with Start: mode $30); second menu: arcade or
; original mode. Then the colour cycling, the cursor, and the intro steps
; with the objects once the start delay is over.
title_frame:
    sep #$20
    .a8
    lda a:MENU_STATE
    sta z:t_menu
    lda a:PAD1_PRESSED
    bit #$02 | $01
    beq :+
    jmp @move
:   bit #$A0
    bne :+
    jmp @idle8
:   ; Start or C
    lda z:t_menu
    bit #2
    bne @second_start
    lda a:PAD1_HELD
    bit #$40
    rep #$20
    .a16
    beq :+
    lda #$30
    jmp next_mode
:   jsr demo_delay
    lda z:t_menu
    and #1
    beq :+
    lda #$2C                ; password
    jmp next_mode
:   sep #$20
    .a8
    lda z:t_menu
    ora #2
    sta a:MENU_STATE
    rep #$20
    .a16
    copy_words $579A0003, $007A8C, 13
    copy_words $589A0003, $007A72, 13
    rts
@second_start:
    rep #$20
    .a16
    lda z:t_menu
    and #4
    bne :+
    lda #$08                ; arcade
    jmp next_mode
:   ldx #0                  ; original mode: a new save
:   stz a:MENU_STATE, x
    inx
    inx
    cpx #$200
    bcc :-
    ldx #0
    lda #$0F00              ; ($000F, big-endian)
:   sta a:$B648, x
    inx
    inx
    cpx #6 * 2
    bcc :-
    lda #$24
    jmp next_mode
@move:
    .a8
    rep #$20
    .a16
    jsr demo_delay
    sep #$20
    .a8
    lda z:t_menu
    bit #2
    beq :+
    eor #4
    bra :++
:   eor #1
:   sta a:MENU_STATE
@idle8:
    rep #$20
    .a16
    jsr title_cycle
    jsr title_cursor
    ldw TITLE_DELAY
    dec
    stw TITLE_DELAY
    cmp #$8000
    bcs :+
    rts
:   sep #$20
    .a8
    lda a:TITLE_STEP
    rep #$20
    .a16
    and #$00FF
    lsr                     ; step / 4, x2
    tax
    jsr (title_steps, x)
    jmp objects_update

title_steps:
    .word title_launch, title_launch, title_launch, title_launch, title_launch
    .word title_launch, title_launch, title_wipe, title_flash, title_wait

; Cycle 12 colours of the title every 8 frames ($77C6).
title_cycle:
    ldw TITLE_CYCLE
    dec
    stw TITLE_CYCLE
    cmp #$8000
    bcs :+
    rts
:   lda #7
    stw TITLE_CYCLE
    ldw TITLE_CYCLE_POS
    sec
    sbc #2
    cmp #$8000
    bcc :+
    lda #$16
:   stw TITLE_CYCLE_POS
    clc
    adc #$78DE
    tax
    lda #$D3E8
    sta z:v_dst
    lda #0
    ldy #12
    jmp palette_words

; The blinking menu cursor: two positions per menu, the selected one shown
; every other 16 frames ($7806).
title_cursor:
    sep #$20
    .a8
    lda a:MENU_STATE
    sta z:t_menu
    rep #$20
    .a16
    lda z:t_menu
    and #2
    beq :+
    lda #$5796              ; second menu: top, bottom
    ldx #$5896
    bra :++
:   lda #$579C
    ldx #$589C
:   sta z:t_top
    stx z:t_bottom
    ldx #3
    jsr vdp_command
    lda #0
    jsr vdp_write
    lda z:t_bottom
    ldx #3
    jsr vdp_command
    lda #0
    jsr vdp_write
    sep #$20
    .a8
    lda a:FRAME_COUNTER_BYTE
    and #$10
    rep #$20
    .a16
    beq @done
    lda z:t_menu
    and #2
    beq :+
    lda #4
    bra :++
:   lda #1
:   and z:t_menu
    beq :+
    lda z:t_bottom
    bra :++
:   lda z:t_top
:   ldx #3
    jsr vdp_command
    lda #$C349
    jsr vdp_write
@done:
    rts

; Step 0: launch the title bike's pieces as thrown objects ($75EA):
; handler 4 with a fixed fall, its x from a ROM table.
title_launch:
    sep #$20
    .a8
    lda a:TITLE_STEP
    rep #$20
    .a16
    and #$00FF
    asl
    asl
    asl
    asl                     ; 16 * step
    clc
    adc #$CE40
    tax
    lda a:0, x
    beq :+
    rts
:   lda #4
    stw 0, x
    sep #$20
    .a8
    lda a:TITLE_STEP
    lsr                     ; (signed) step >> 2
    lsr
    sta a:$2D, x
    rep #$20
    .a16
    lda #$0010              ; $00100000
    stw $18, x
    stz a:$1A, x
    lda #$0001              ; $00017000
    stw $1C, x
    lda #$7000
    stw $1E, x
    lda #4
    stw $20, x
    sep #$20
    .a8
    lda a:TITLE_STEP
    lsr
    rep #$20
    .a16
    and #$00FF
    phx
    tax
    lda f:MDROM + $763C, x  ; (big-endian on both sides)
    plx
    sta a:4, x
    lda #$FFB4
    stw 8, x
    rts

; Step 4: wipe the first VRAM tile in a growing checker of colour 1, then
; clear it again ($76B4), one stage every 4 counts.
title_wipe:
    ldw TITLE_TIMER
    dec
    stw TITLE_TIMER
    cmp #$8000
    bcs :+
    rts
:   ldw TITLE_COUNT
    inc
    stw TITLE_COUNT
    cmp #$21
    bcc @checker
    lsr
    lsr
    sec
    sbc #8
    inc
    sta z:t_obj             ; (n >> 2) - 8 + 1 longs
    command $40000000
:   lda #0
    jsr vdp_write
    lda #0
    jsr vdp_write
    dec z:t_obj
    bne :-
    bra @end
@checker:
    and #3
    beq :+
    rts
:   ldw TITLE_COUNT
    lsr
    lsr
    inc
    sta z:t_obj             ; (n >> 2) + 1 longs, alternating
    stz z:t_menu
    command $40000000
:   lda z:t_menu
    and #1
    beq :+
    lda #$0011
    jsr vdp_write
    lda #$0011
    bra :++
:   lda #$1100
    jsr vdp_write
    lda #$1100
:   jsr vdp_write
    inc z:t_menu
    dec z:t_obj
    bne :---
@end:
    ldw TITLE_COUNT
    cmp #$40
    bne :+
    sep #$20
    .a8
    lda a:TITLE_STEP
    clc
    adc #4
    sta a:TITLE_STEP
    rep #$20
    .a16
    stz a:TITLE_COUNT
    stz a:TITLE_COUNT + 2
    stz a:TITLE_PHASE
    stz a:TITLE_PHASE + 2
:   rts

; Step 8: flash two colour ramps, alternating every count, ten counts
; ($7740); then wait for the demo.
title_flash:
    sep #$20
    .a8
    lda a:TITLE_COUNT + 1
    cmp #$0A
    rep #$20
    .a16
    bcc :+
    sep #$20
    .a8
    lda a:TITLE_STEP
    clc
    adc #4
    sta a:TITLE_STEP
    rep #$20
    .a16
    lda #$B4
    stw TITLE_COUNT
    stz a:$C71C
    rts
:   lda a:TITLE_COUNT       ; odd count?
    xba
    and #1
    sta z:t_menu
    ldw TITLE_TIMER
    dec
    stw TITLE_TIMER
    cmp #$8000
    bcs :+
    rts
:   lda #4
    stw TITLE_TIMER
    ldw TITLE_PHASE
    inc
    inc
    ldx z:t_menu
    beq :+
    cmp #$0C
    bra :++
:   cmp #$0E
:   bcc :+
    ldw TITLE_COUNT
    inc
    stw TITLE_COUNT
    lda #0
:   stw TITLE_PHASE
    ldx z:t_menu
    beq @even
    clc
    adc #$7928
    tax
    lda #$D3A4
    sta z:v_dst
    lda #0
    ldy #6
    jmp palette_words
@even:
    clc
    adc #$790C
    tax
    lda #$D3B0
    sta z:v_dst
    lda #0
    ldy #7
    jmp palette_words

; Step 12: after a while, start the demo race ($75B0).
title_wait:
    ldw DEMO_DELAY
    dec
    stw DEMO_DELAY
    cmp #$8000
    bcs :+
    rts
:   lda #$14
    jmp next_mode
