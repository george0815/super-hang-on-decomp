; The S-CPU: boot, then the VBlank work. The game itself runs on the SA-1
; (sa1.s); each VBlank this side copies the frame the SA-1 prepared into
; the PPU, reads the pads and counts the VBlank, as the Genesis's VBlank
; handler does.

.p816
.smart
.include "inc/hw.inc"
.include "inc/mem.inc"

.import sa1_reset

.segment "ZEROPAGE"
joy:        .res 2
pad:        .res 1
nmi_busy:   .res 1

.segment "CODE"

reset:
    sei
    clc
    xce
    rep #$38
    ldx #$1FFF
    txs
    lda #$0000
    tcd
    sep #$20
    .a8
    .i16
    pha
    plb

    lda #$80
    sta INIDISP             ; forced blank
    stz NMITIMEN
    ldx #0                  ; work RAM up to the stack, cleared
:   stz $00, x
    inx
    cpx #$1F00
    bcc :-
    stz HDMAEN
    lda #1
    sta MEMSEL              ; fast ROM
    jsr clear_ppu
    jsr clear_vram
    jsr clear_cgram
    jsr hide_objs

    ; the SA-1: power-up ROM banks, BW-RAM and I-RAM writable, then start
    ; it at sa1_reset
    stz CXB
    lda #1
    sta DXB
    lda #2
    sta EXB
    lda #3
    sta FXB
    stz BMAPS
    lda #$80
    sta SBWE
    lda #$FF
    sta SIWP
    stz UPLOAD_READY
    lda #$80
    sta SHOWN_INIDISP
    rep #$20
    lda #.loword(sa1_reset)
    sta CRV
    sep #$20
    stz CCNT

    lda #$81
    sta NMITIMEN            ; NMI and the automatic pad read
:   wai
    bra :-

; Zero the PPU registers that power up undefined.
clear_ppu:
    ldx #$2101
:   stz $00, x
    inx
    cpx #$2134
    bne :-
    ldx #$210D              ; the scroll registers are written twice
:   stz $00, x
    stz $00, x
    inx
    cpx #$2115
    bne :-
    rts

zero: .word 0

clear_vram:
    lda #$80
    sta VMAIN
    ldx #0
    stx VMADDL
    lda #$09                ; fixed source, two registers
    sta DMAP(0)
    lda #<VMDATAL
    sta BBAD(0)
    ldx #.loword(zero)
    stx A1T(0)
    lda #^zero
    sta A1B(0)
    ldx #0                  ; 64 KB
    stx DAS(0)
    lda #1
    sta MDMAEN
    rts

clear_cgram:
    stz CGADD
    lda #$08                ; fixed source, one register
    sta DMAP(0)
    lda #<CGDATA
    sta BBAD(0)
    ldx #.loword(zero)
    stx A1T(0)
    lda #^zero
    sta A1B(0)
    ldx #512
    stx DAS(0)
    lda #1
    sta MDMAEN
    rts

; All OBJs below the screen.
hide_objs:
    ldx #0
    stx OAMADDL
:   stz OAMDATA             ; x
    lda #240
    sta OAMDATA             ; y
    stz OAMDATA
    stz OAMDATA
    inx
    cpx #128
    bne :-
    ldx #32
:   stz OAMDATA
    dex
    bne :-
    rts

; ------------------------------------------------------------------ NMI

nmi:
    rep #$30
    pha
    phx
    phy
    phb
    phd
    lda #$0000
    tcd
    sep #$20
    .a8
    pha
    plb
    lda RDNMI
    lda nmi_busy            ; a long upload ran into the next VBlank
    bne @return
    inc nmi_busy

    lda UPLOAD_READY
    beq @no_uploads
    lda SH_INIDISP          ; turning the display off: first, so the
    bpl :+                  ; uploads all reach VRAM
    sta INIDISP
    sta SHOWN_INIDISP
:   jsr run_uploads
    jsr apply_shadows
    stz UPLOAD_READY
@no_uploads:
    jsr read_pad
    rep #$20
    .a16
    lda f:MIRROR + VBLANK_COUNT ; (big-endian)
    xba
    inc
    xba
    sta f:MIRROR + VBLANK_COUNT
    sep #$20
    .a8
    stz nmi_busy
@return:
    rep #$30
    pld
    plb
    ply
    plx
    pla
    rti

.a8
; Run the SA-1's upload list (see mem.inc) on DMA channel 0.
run_uploads:
    ldy #0
    tyx
@next:
    lda f:UPLOADS, x
    beq @done
    cmp #UP_CGRAM
    beq @cgram
    bcs @oam
    lda #$80
    sta VMAIN
    rep #$20
    lda f:UPLOADS + 4, x
    sta VMADDL
    sep #$20
    lda #$01
    ldy #<VMDATAL
    bra @go
@cgram:
    lda f:UPLOADS + 4, x
    sta CGADD
    lda #$00
    ldy #<CGDATA
    bra @go
@oam:
    rep #$20
    lda f:UPLOADS + 4, x
    sta OAMADDL
    sep #$20
    lda #$00
    ldy #<OAMDATA
@go:
    sta DMAP(0)
    tya
    sta BBAD(0)
    rep #$20
    lda f:UPLOADS + 1, x
    sta A1T(0)
    lda f:UPLOADS + 6, x
    sta DAS(0)
    sep #$20
    lda f:UPLOADS + 3, x
    sta A1B(0)
    lda #1
    sta MDMAEN
    rep #$20
    txa
    clc
    adc #8
    tax
    sep #$20
    bra @next
@done:
    rts

apply_shadows:
    lda SH_BGMODE
    sta BGMODE
    lda SH_BG1SC
    sta BG1SC
    lda SH_BG2SC
    sta BG2SC
    lda SH_BG12NBA
    sta BG12NBA
    lda SH_TM
    sta TM
    lda SH_OBSEL
    sta OBSEL
    lda SH_BG1HOFS
    sta BG1HOFS
    lda SH_BG1HOFS + 1
    sta BG1HOFS
    lda SH_BG1VOFS
    sta BG1VOFS
    lda SH_BG1VOFS + 1
    sta BG1VOFS
    lda SH_BG2HOFS
    sta BG2HOFS
    lda SH_BG2HOFS + 1
    sta BG2HOFS
    lda SH_BG2VOFS
    sta BG2VOFS
    lda SH_BG2VOFS + 1
    sta BG2VOFS
    lda SH_INIDISP
    sta INIDISP
    sta SHOWN_INIDISP
    rts

; The pad as the Genesis's: held buttons S A C B R L D U, and the newly
; pressed ones. SNES Y, B and A stand for the Genesis's A, B and C.
read_pad:
:   lda HVBJOY
    lsr
    bcs :-                  ; the automatic read is still running
    rep #$20
    .a16
    lda JOY1L
    sta joy
    sep #$20
    .a8
    stz pad
    ldx #0
@button:
    rep #$20
    .a16
    lda pad_map, x
    and joy
    sep #$20
    .a8
    beq :+
    lda pad_map + 2, x
    tsb pad
:   inx
    inx
    inx
    cpx #8 * 3
    bne @button
    lda f:MIRROR + PAD1_HELD
    eor pad                 ; changed
    and pad                 ; and down now
    sta f:MIRROR + PAD1_PRESSED
    lda pad
    sta f:MIRROR + PAD1_HELD
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

irq:
    rti

.segment "HEADER"
    .byte "SUPER HANG-ON SNES   "   ; 21 bytes
    .byte $23                   ; SA-1
    .byte $34                   ; ROM, SA-1, RAM
    .byte $0A                   ; 1 MB
    .byte $08                   ; 256 KB BW-RAM
    .byte $01                   ; North America
    .byte $00                   ; developer
    .byte $00                   ; version
    .word $FFFF, $0000          ; checksum complement, checksum (fixed later)

.segment "VECTORS"
    ; native: -, -, COP, BRK, ABORT, NMI, -, IRQ
    .word 0, 0, irq, irq, irq, nmi, 0, irq
    ; emulation: -, -, COP, -, ABORT, NMI, RESET, IRQ/BRK
    .word 0, 0, irq, 0, irq, nmi, reset, irq
