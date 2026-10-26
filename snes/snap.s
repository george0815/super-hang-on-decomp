; Genesis screen viewer: shows one converted VDP snapshot
; (tools/snes/vdp2snes.py) to check the graphics mapping on the SNES.
;
; BG1 = plane A or the window plane, BG2 = plane B, mode 1. HDMA sets
; BG1's map and both layers' scroll on every line, as the Genesis's
; per-line scroll and window split do.

.p816
.smart

; VRAM layout, word addresses (see vdp2snes.py)
BG_TILES    = $0000
WINDOW_MAP  = $3800
PLANE_A_MAP = $4000
PLANE_B_MAP = $5000
OBJ_TILES   = $6000

INIDISP = $2100
OBSEL   = $2101
OAMADDL = $2102
BGMODE  = $2105
BG1SC   = $2107
BG2SC   = $2108
BG12NBA = $210B
VMAIN   = $2115
VMADDL  = $2116
CGADD   = $2121
TM      = $212C
NMITIMEN = $4200
MDMAEN  = $420B
HDMAEN  = $420C

; DMA channel n registers
.define DMAP(n) $4300 + n * $10
.define BBAD(n) $4301 + n * $10
.define A1T(n)  $4302 + n * $10
.define A1B(n)  $4304 + n * $10
.define DAS(n)  $4305 + n * $10

; DMA `len` bytes from `src` to B-bus register `reg` (low byte of $21xx)
; with transfer mode `mode`, on channel 0. A 8-bit, X/Y 16-bit.
.macro dma mode, reg, src, len
    lda #mode
    sta DMAP(0)
    lda #reg
    sta BBAD(0)
    ldx #.loword(src)
    stx A1T(0)
    lda #^src
    sta A1B(0)
    ldx #len
    stx DAS(0)
    lda #1
    sta MDMAEN
.endmacro

.macro vram_dma addr, src, len
    ldx #addr
    stx VMADDL
    dma $01, $18, src, len
.endmacro

; HDMA channel n: table at `src`, mode `mode`, register `reg`
.macro hdma n, mode, reg, src
    lda #mode
    sta DMAP(n)
    lda #reg
    sta BBAD(n)
    ldx #.loword(src)
    stx A1T(n)
    lda #^src
    sta A1B(n)
.endmacro

.segment "CODE"

reset:
    sei
    clc
    xce                     ; native mode
    rep #$38                ; 16-bit A and X/Y, binary
    ldx #$1FFF
    txs
    lda #$0000
    tcd                     ; direct page 0
    sep #$20
    .a8
    .i16
    pha
    plb                     ; data bank 0

    lda #$80
    sta INIDISP             ; forced blank
    stz NMITIMEN
    stz HDMAEN
    jsr clear_ppu

    lda #$80
    sta VMAIN               ; increment after the high byte
    vram_dma BG_TILES,    bg_tiles,  bg_tiles_end - bg_tiles
    vram_dma WINDOW_MAP,  map_w,     map_w_end - map_w
    vram_dma PLANE_A_MAP, map_a,     map_a_end - map_a
    vram_dma PLANE_B_MAP, map_b,     map_b_end - map_b
    vram_dma OBJ_TILES,   obj_tiles, obj_tiles_end - obj_tiles

    stz CGADD
    dma $00, $22, cgram, 512
    ldx #0
    stx OAMADDL
    dma $00, $04, oam, 544

    lda #$01
    sta BGMODE              ; mode 1, 8x8 tiles
    lda #(PLANE_A_MAP >> 10 << 2) | 3
    sta BG1SC
    lda #(PLANE_B_MAP >> 10 << 2) | 3
    sta BG2SC
    lda #(BG_TILES >> 12) | (BG_TILES >> 12 << 4)
    sta BG12NBA
    lda #$60 | (OBJ_TILES >> 13)
    sta OBSEL               ; 16x16 and 32x32 OBJs
    lda #$13
    sta TM                  ; OBJ, BG2, BG1

    hdma 1, $00, $07, hdma_bg1sc
    hdma 2, $03, $0D, hdma_bg1ofs
    hdma 3, $03, $0F, hdma_bg2ofs

    ; turn on at the start of a frame so HDMA starts from its table tops
:   lda $4212
    bpl :-
    lda #$0E
    sta HDMAEN
    lda #$0F
    sta INIDISP
forever:
    bra forever

; Zero the PPU registers that power up undefined.
clear_ppu:
    ldx #$2101
:   stz $00, x
    inx
    cpx #$2134
    bne :-
    ; the scroll registers are written twice
    ldx #$210D
:   stz $00, x
    stz $00, x
    inx
    cpx #$2115
    bne :-
    rts

nmi:
irq:
    rti

.segment "HEADER"
    .byte "SUPER HANG-ON SNAP   "   ; 21 bytes
    .byte $20                   ; LoROM, slow
    .byte $00                   ; ROM only
    .byte $07                   ; 128 KB
    .byte $00                   ; no RAM
    .byte $01                   ; North America
    .byte $00                   ; developer
    .byte $00                   ; version
    .word $FFFF, $0000          ; checksum complement, checksum (fixed later)

.segment "VECTORS"
    ; native: -, -, COP, BRK, ABORT, NMI, -, IRQ
    .word 0, 0, irq, irq, irq, nmi, 0, irq
    ; emulation: -, -, COP, -, ABORT, NMI, RESET, IRQ/BRK
    .word 0, 0, irq, 0, irq, nmi, reset, irq

.segment "BANK1"
bg_tiles:   .incbin "bg_tiles.bin"
bg_tiles_end:

.segment "BANK2"
map_a:      .incbin "map_a.bin"
map_a_end:
map_b:      .incbin "map_b.bin"
map_b_end:
map_w:      .incbin "map_w.bin"
map_w_end:

.segment "BANK3"
obj_tiles:  .incbin "obj_tiles.bin"
obj_tiles_end:
cgram:      .incbin "cgram.bin"
oam:        .incbin "oam.bin"
hdma_bg1sc: .incbin "hdma_bg1sc.bin"
hdma_bg1ofs: .incbin "hdma_bg1ofs.bin"
hdma_bg2ofs: .incbin "hdma_bg2ofs.bin"
