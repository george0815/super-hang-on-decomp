; The SA-1: its start, the Genesis's VBlank interrupt, and the upload
; lists the S-CPU's NMI carries out. The game itself is the compiled C
; (build/snes/c, from src/game): reset() is its main loop.
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

.export sa1_reset, sa1_irq, upload, upload_vram, upload_fits, upload_handover
.import vdp_reset, sprites_reset, c_init
.import reset, vblank_menu, vblank_race

UPLOAD_A = $E000        ; the two upload lists (bank $41)

.ifdef PROFILE
PROFILE_RING = $43C000  ; (the top of the tile cache: free in profiling runs)
PROFILE_SIZE = $4000
.endif

.segment "BWRAM"
profile_pos: .res 2
iram_copy: .res $200    ; I-RAM $3000-$31FF, each VBlank (dumps can't see I-RAM)
UPLOAD_B = $F000

.segment "CODE"

.a8
.i8
sa1_reset:
    sei
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
    stz up_pos
    lda #UPLOAD_A
    sta z:up_list
    lda #UPLOAD_BUDGET
    sta z:up_budget
    jsr vdp_reset
    jsr sprites_reset
    jsl c_init              ; the C's initialised statics
    jml reset               ; the game (interrupts masked, as on a reset)

; --------------------------------------------------------------- VBlank

; The Genesis's VBlank interrupt ($6D2A), raised by the S-CPU after its
; NMI work: the handler the mode picked (C722: 4 menus, 8 races), then
; the VBlank counted. The compiled code's temporaries are kept; the game
; masks the interrupt (set_sr) where the original masks interrupts.
sa1_irq:
    rep #$30
.ifdef PROFILE
    pha                     ; (profiling: note the interrupted address)
    phx
    lda f:profile_pos
    tax
    lda 6, s                ; PC (under X, A and P)
    sta f:PROFILE_RING, x
    lda 8, s                ; PB
    sta f:PROFILE_RING + 2, x
    txa
    clc
    adc #4
    cmp #PROFILE_SIZE
    bcc :+
    lda #0
:   sta f:profile_pos
    plx
    pla
.endif
    pha
    phx
    phy
    phb
    phd
    lda #SA1_DP
    tcd
    ldx #$1E                ; the temporaries ($E0-$FF)
:   lda z:CHI, x
    pha
    dex
    dex
    bpl :-
    pei (VDP_ADDR)          ; and the VDP's port state: a slow frame can
    pei (VDP_CODE)          ; still be writing VRAM when the VBlank comes
    pei (VDP_CTRL_PENDING)
    pei (VDP_CMD_FIRST)
    pei (VDP_FILL_PENDING)
    sep #$20
    .a8
    lda #$80
    sta f:CIC               ; (long: the data bank is still the game's)
    lda #^MIRROR
    pha
    plb
    rep #$20
    .a16
    jsr pad_convert
    ldx #$1FE               ; (for debugging: I-RAM where dumps can see it)
:   lda f:$003000, x
    sta f:iram_copy, x
    dex
    dex
    bpl :-
    ldw VBLANK_HANDLER
    cmp #4
    bne :+
    jsl vblank_menu
    bra @count
:   cmp #8
    bne @count
    jsl vblank_race
@count:
    ldw VBLANK_COUNT
    inc
    stw VBLANK_COUNT
    pla
    sta z:VDP_FILL_PENDING
    pla
    sta z:VDP_CMD_FIRST
    pla
    sta z:VDP_CTRL_PENDING
    pla
    sta z:VDP_CODE
    pla
    sta z:VDP_ADDR
    ldx #0
:   pla
    sta z:CHI, x
    inx
    inx
    cpx #$20
    bcc :-
    rep #$30
    pld
    plb
    ply
    plx
    pla
    rti

; The SNES pad as the Genesis's S A C B R L D U (1 = down), for the pad
; ports (rt.s). SNES Y, B and A stand for the Genesis's A, B and C.
pad_convert:
    lda f:PAD_RAW
    sta z:CT0
    stz z:CT1
    ldx #0
@button:
    lda f:pad_map, x
    and z:CT0
    beq :+
    lda f:pad_map + 2, x
    and #$00FF
    tsb z:CT1
:   inx
    inx
    inx
    cpx #8 * 3
    bne @button
    lda z:CT1
    sep #$20
    .a8
    sta f:PAD_HELD_MD
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

; -------------------------------------------------------------- uploads

; The NMI's DMA only reaches VRAM in VBlank while the display is on:
; about 5.5 KB a frame. OAM always goes; the rest checks.
UPLOAD_BUDGET = 5400 - 544

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
; Queue an upload of kind A.
upload:
    php
    sei
    pha
    lda z:up_budget
    sec
    sbc up_len
    bcs :+
    lda #0
:   sta z:up_budget
    lda z:up_list
    clc
    adc up_pos
    tax
    pla
    sta f:$410000, x
    lda up_src
    sta f:$410000 + 1, x
    lda up_src + 1
    sta f:$410000 + 2, x
    lda up_dst
    sta f:$410000 + 4, x
    lda up_len
    sta f:$410000 + 6, x
    lda up_pos
    clc
    adc #8
    sta up_pos
    plp
    rts

; Hand the list built so far to the next NMI and start the other one. The
; caller makes sure the last one was carried out (UPLOAD_READY clear).
upload_handover:
    php
    sei
    lda z:up_list
    clc
    adc up_pos
    tax
    sep #$20
    .a8
    lda #UP_END
    sta f:$410000, x
    rep #$20
    .a16
    lda z:up_list
    sta f:UPLOAD_LIST
    eor #UPLOAD_A ^ UPLOAD_B
    sta z:up_list
    stz up_pos
    lda #UPLOAD_BUDGET
    sta z:up_budget
    sep #$20
    .a8
    lda #1
    sta f:UPLOAD_READY
    rep #$20
    .a16
    plp
    rts
