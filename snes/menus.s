; Pieces the menu screens share: the countdown, big digits, "PUSH START
; BUTTON", the fade to black, BCD, sound commands (src/game/vblank.c,
; system.c, title.c, audio.c).

.p816
.smart
.include "inc/mem.inc"
.include "inc/dp.inc"
.include "inc/macros.inc"

.export menu_countdown, hud_big_digits, push_start_blink, fade_out
.export bcd_sub, sound_command, next_mode
.import vdp_copy, vdp_fill, vdp_copy_rect, wait_vblank, palette_upload

cmd_hi      = $46       ; (vdp.s's arguments)
cmd_lo      = $6C
width       = $52
rows        = $54

SOUND_LOCKED     = $C754
ROM_DIGIT_TILES  = $00C438
DIGITS_BUFFER    = $0582

; direct page: $90-$97
m_t0        = $90
m_t1        = $92
m_k         = $94
m_step      = $96

.segment "CODE"
.a16
.i16

; On to mode A.
next_mode:
    stw GAME_MODE
    stz a:VBLANK_HANDLER
    rts

; Send sound command A to the sound driver, unless sounds are locked. The
; SNES has no sound driver yet: it is kept for it.
sound_command:
    pha
    sep #$20
    .a8
    lda a:SOUND_LOCKED
    rep #$20
    .a16
    bne :+
    pla
    sta f:sound_request
    rts
:   pla
    rts

; a - b in BCD (bytes in A and X): sbcd.
bcd_sub:
    sep #$29                ; 8-bit, decimal, borrow clear (carry set)
    .a8
    stx z:m_t0
    sbc z:m_t0
    rep #$29
    .a16
    and #$00FF
    rts

; The menus' countdown ($C1D8 on): a second's tick, then the BCD count
; at $C71A one down and drawn; $C718 set when it runs out.
menu_countdown:
    sep #$20
    .a8
    dec a:$C71B
    rep #$20
    .a16
    bne @done
    sep #$20
    .a8
    lda a:$C71A
    beq :+
    lda #0
    bra :++
:   lda #$FF
:   sta a:$C718
    lda a:$C71A
    rep #$20
    .a16
    beq @done
    sep #$20
    .a8
    lda #$3C
    sta a:$C71B
    rep #$20
    .a16
    lda a:$C71A
    and #$00FF
    ldx #1
    jsr bcd_sub
    sep #$20
    .a8
    sta a:$C71A
    rep #$20
    .a16
    lda #$5A26
    sta z:cmd_hi
    lda #$0003
    sta z:cmd_lo
    ldx #$C71A
    jmp hud_big_digits
@done:
    rts

; Two tall digits (2x2 entries each) for the BCD byte at X ($C1B0), drawn
; at the command in cmd_hi/cmd_lo. A leading zero is blank.
hud_big_digits:
    lda a:0, x
    and #$00FF
    sta z:m_t0
    ; the two digits as name table entries (bcd_digits): tile $614 + digit,
    ; 0 for a leading zero
    lsr
    lsr
    lsr
    lsr
    beq :+
    clc
    adc #$614
:   stw DIGITS_BUFFER
    lda z:m_t0
    and #$000F
    clc
    adc #$614
    stw DIGITS_BUFFER + 2
    ; each into its 2x2 tiles from the ROM table
    ldx #0
@digit:
    ldw DIGITS_BUFFER, x
    beq :+
    sec
    sbc #$613
    asl
    asl
:   phx
    tax
    lda f:MDROM + ROM_DIGIT_TILES, x
    sta z:m_t0
    lda f:MDROM + ROM_DIGIT_TILES + 2, x
    plx
    sta a:DIGITS_BUFFER + 4, x
    lda z:m_t0
    sta a:DIGITS_BUFFER, x
    inx
    inx
    cpx #4
    bcc @digit
    lda #2
    sta z:width
    sta z:rows
    lda #$00FF
    ldx #DIGITS_BUFFER
    jmp vdp_copy_rect

; "PUSH START BUTTON", shown every other 32 frames ($7BBC).
push_start_blink:
    lda #$5B96
    sta z:cmd_hi
    lda #$0003
    sta z:cmd_lo
    sep #$20
    .a8
    lda a:FRAME_COUNTER_BYTE
    and #$20
    rep #$20
    .a16
    beq :+
    lda #$0000
    ldx #$7D10
    ldy #$11
    jmp vdp_copy
:   lda #0
    ldy #$11
    jmp vdp_fill

; Fade the whole palette to black ($6DD4): seven steps, each darkening
; every channel of the 64 colours by one level, shown every 3 VBlanks.
fade_out:
    jsr wait_vblank
    lda #7
    sta z:m_step
@step:
    ldx #0
@colour:
    ldw PALETTE_BUFFER, x
    sta z:m_t0
    and #$000E
    jsr channel_down
    sta z:m_t1
    lda z:m_t0
    and #$00E0
    lsr
    lsr
    lsr
    lsr
    jsr channel_down
    asl
    asl
    asl
    asl
    tsb z:m_t1
    lda z:m_t0
    and #$0E00
    xba
    jsr channel_down
    xba
    ora z:m_t1
    stw PALETTE_BUFFER, x
    inx
    inx
    cpx #128
    bcc @colour
    jsr wait_vblank
    jsr wait_vblank
    jsr palette_upload      ; (straight to CRAM, the flag left alone)
    dec z:m_step
    bne @step
    rts

; One colour level down: even values, floor 0 ($6E1C).
channel_down:
    and #$000E
    cmp #2
    bcc :+
    sbc #2
    rts
:   lda #0
    rts

.segment "BWRAM"
sound_request: .res 2
