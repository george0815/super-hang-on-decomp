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
.include "inc/dp.inc"
.include "inc/macros.inc"

.export sa1_reset, sa1_irq, wait_vblank, upload, upload_vram, upload_flush, palette_upload
.export upload_fits
.export mode_first_call, mode_setup_done
.import logo_mode, title_mode, course_select_mode, music_select_mode, vdp_flush, vdp_pending, vdp_reset, display_on
.import sprites_reset


; direct page (SA1_DP + offset)
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
    lda #$80
    sta CIE                 ; the VBlank interrupt from the S-CPU
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
    stz z:vdp_on
    jsr sprites_reset
    cli

main_loop:
    sep #$20
    .a8
    lda a:FRAME_COUNTER_BYTE
    inc
    sta a:FRAME_COUNTER_BYTE
    rep #$20
    .a16
    ldw GAME_MODE
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
    .word logo_mode, title_mode, course_select_mode, music_select_mode
modes_end:

; The first call of a game mode? Z set if so; marks it called (bit 0 of
; the mode, src/game/game.h).
mode_first_call:
    sep #$20
    .a8
    lda a:GAME_MODE + 1
    bit #1
    php
    ora #1
    sta a:GAME_MODE + 1
    plp
    rep #$20
    .a16
    rts

; The end of most modes' setup ($6D70 and on): VBlanks on with handler A,
; then after the next one the display on. Modes that use the VDP layer
; also wait for its uploads, so the screen appears complete.
mode_setup_done:
    cli
    stw VBLANK_HANDLER
    jsr wait_vblank
    lda z:vdp_on
    bne :+
    sep #$20
    .a8
    lda #$0F
    sta f:SH_INIDISP
    rep #$20
    .a16
    rts
:   jsr vdp_pending
    bne :+
    lda f:PALETTE_PENDING
    and #$00FF
    beq :++
:   jsr wait_vblank
    bra :--
:   jmp display_on

; Wait for the next VBlank ($6D4E). The frame's uploads go out in it: the
; palette if the last VBlank took it, the VDP layer's changes and the
; list, which the S-CPU's NMI runs before the VBlank interrupt counts it.
wait_vblank:
    sep #$20
    .a8
    lda f:PALETTE_PENDING
    beq :+
    lda #0
    sta f:PALETTE_PENDING
    rep #$20
    .a16
    jsr palette_upload
:   rep #$20
    .a16
    lda z:vdp_on
    beq :+
    jsr vdp_flush
:   jsr upload_flush
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
    lda #UPLOAD_BUDGET
    sta z:up_budget
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

; The NMI's DMA only reaches VRAM in VBlank while the display is on:
; about 5.5 KB a frame. The palette and OAM always go; the rest checks.
UPLOAD_BUDGET = 5400 - 544 - 256

; Is there room for A more bytes this frame? Carry set if so (always while
; the display is off before or after this VBlank).
upload_fits:
    pha
    sep #$20
    .a8
    lda f:SHOWN_INIDISP
    ora f:SH_INIDISP
    rep #$20
    .a16
    and #$0080
    bne @yes
    pla
    cmp z:up_budget
    beq :+
    bcs @no
:   sec
    rts
@yes:
    pla
    sec
    rts
@no:
    clc
    rts

; Queue a VRAM upload: up_len bytes from up_src to VRAM word up_dst.
upload_vram:
    lda #UP_VRAM
upload:
    pha
    lda z:up_budget
    sec
    sbc up_len
    bcs :+
    lda #0
:   sta z:up_budget
    pla
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
    ldw PALETTE_BUFFER, x       ; ----BBB-GGG-RRR-
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
    lda z:vdp_on            ; the backdrop: the colour VDP register 7 picks
    beq :+
    lda z:VDP_REGS + 7
    and #$003F
    asl
    tax
    lda f:cgram_buffer, x
    sta f:cgram_buffer
:
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

; --------------------------------------------------------------- VBlank

; The Genesis's VBlank interrupt ($6D2A), raised by the S-CPU after its
; NMI work: the handler the mode picked, then the VBlank counted. It has
; a direct page of its own and touches only the Genesis RAM and I-RAM.
; The game masks it (sei) where the original masks interrupts.
VBLANK_DP_JOY = $00
VBLANK_DP_PAD = $02

sa1_irq:
    rep #$30
    pha
    phx
    phy
    phb
    phd
    lda #SA1_IRQ_DP
    tcd
    sep #$20
    .a8
    lda #$80
    sta f:CIC               ; (long: the data bank is still the game's)
    lda #^MIRROR
    pha
    plb
    rep #$20
    .a16
    ldw VBLANK_HANDLER
    cmp #4
    bne :+
    jsr vblank_menu
:   ldw VBLANK_COUNT
    inc
    stw VBLANK_COUNT
    rep #$30
    pld
    plb
    ply
    plx
    pla
    rti

; The menus' VBlank ($7486): the sprite table, the palette if marked, the
; pads. (The uploads themselves are the main loop's; this keeps the RAM
; as the original leaves it.)
vblank_menu:
    lda #$0048              ; DMA command $48000083: the sprite table
    sta a:$C70A
    lda #$8300
    sta a:$C70C
    sep #$20
    .a8
    lda a:PALETTE_DIRTY
    lsr
    bcc :+
    asl
    sta a:PALETTE_DIRTY
    lda #1
    sta f:PALETTE_PENDING
    rep #$20
    .a16
    lda #$00C0              ; $C0000080: CRAM
    sta a:$C70A
    lda #$8000
    sta a:$C70C
:   rep #$20
    .a16
    ; fall through

; The pad as the Genesis's: held buttons S A C B R L D U, and the newly
; pressed ones ($8196). SNES Y, B and A stand for the Genesis's A, B and C.
read_pad:
    lda f:PAD_RAW
    sta z:VBLANK_DP_JOY
    stz z:VBLANK_DP_PAD
    ldx #0
@button:
    lda f:pad_map, x
    and z:VBLANK_DP_JOY
    beq :+
    lda f:pad_map + 2, x
    and #$00FF
    tsb z:VBLANK_DP_PAD
:   inx
    inx
    inx
    cpx #8 * 3
    bne @button
    sep #$20
    .a8
    lda a:PAD1_HELD
    eor z:VBLANK_DP_PAD     ; changed
    and z:VBLANK_DP_PAD     ; and down now
    sta a:PAD1_PRESSED
    lda z:VBLANK_DP_PAD
    sta a:PAD1_HELD
    rep #$20
    .a16
    rts

; SNES button, Genesis button
pad_map:
    .word $0800
    .byte $01               ; up
    .word $0400
    .byte $02               ; down
    .word $0200
    .byte $04               ; left
    .word $0100
    .byte $08               ; right
    .word $8000
    .byte $10               ; B: B
    .word $0080
    .byte $20               ; A: C
    .word $4000
    .byte $40               ; Y: A
    .word $1000
    .byte $80               ; Start

.segment "BWRAM"
cgram_buffer: .res 128
