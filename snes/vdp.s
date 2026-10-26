; The Genesis VDP as the ported code sees it, for the menu screens.
;
; Ported code writes VRAM the way the original does, by Genesis address
; (vdp_command, vdp_write and the bulk helpers below). This file keeps:
;  - a byte-exact copy of the Genesis VRAM (VRAM_MIRROR), so reads and
;    VRAM-to-VRAM copies work;
;  - SNES tile maps converted from the Genesis name tables: plane A
;    ($C000, 64x64) -> BG1 map, plane B ($E000, 64x64) -> BG2 map, the
;    window plane ($D000, 64x32) -> its own map, which BG1 shows when the
;    window covers the screen;
;  - SNES background tile slots: a Genesis tile gets one when a map first
;    uses it, and is uploaded again whenever its pixels change.
; vdp_flush, called in each frame's wait_vblank, queues what changed.
;
; Conventions as in sa1.s (A, X, Y 16-bit; data bank $40).

.p816
.smart
.include "inc/hw.inc"
.include "inc/mem.inc"
.include "inc/dp.inc"
.include "inc/macros.inc"
.include "gen/assets.inc"

.export vdp_reset, vdp_command, vdp_write, vdp_read, vdp_register
.export vdp_copy, vdp_fill, vdp_copy_rect, vdp_clear, vdp_tiles, vdp_flush
.export vdp_pending, tile_gen
.import upload_vram, upload_fits

ROM_VDP_REGISTER_DEFAULTS = $0086A0     ; 19 register-write words
VDP_MODE_SHADOW = $C700

VRAM_MIRROR = $420000

; Genesis VRAM layout (the game never moves these)
PLANE_A     = $C000
WINDOW      = $D000
PLANE_B     = $E000
HSCROLL     = $CC00

; SNES VRAM layout (word addresses), as tools/snes/vdp2snes.py
BG_TILES    = $0000
WINDOW_MAP  = $3800
PLANE_A_MAP = $4000
PLANE_B_MAP = $5000
MAX_SLOTS   = $3800 / 16        ; 896 background tiles
CROP        = 32                ; Genesis columns cut off on the left

UPLOAD_TILES = 192              ; at most this many tiles per frame,
UPLOAD_ROWS  = 48               ; and map rows

; direct page (SA1_DP + offset; sa1.s uses $00-$1F, logo.s $20-$3F)
vdp_addr    = $40       ; VRAM/CRAM/VSRAM address
vdp_code    = $42       ; 1 VRAM write, 3 CRAM write, 5 VSRAM write, 0 VRAM read
next_slot   = $44       ; next free background slot
cmd_hi      = $46       ; arguments of vdp_copy, vdp_fill, vdp_copy_rect: the
cmd_lo      = $6C       ; command's high and low words
entry       = $48
map_off     = $4A
ptr         = $4C       ; 24 bits
count       = $50
width       = $52
rows        = $54
tile        = $56
pend_head   = $58       ; pending tiles: taken from head, added at tail
pend_tail   = $5A
stage       = $5C       ; next free byte in the tile staging buffer
row01       = $5E
row23       = $60
src         = $62       ; 24 bits
budget      = $66
t0          = $68
t1          = $6A
run_end     = $6E       ; upload_tiles: VRAM address after the last upload

.segment "CODE"
.a16
.i16

; ------------------------------------------------------------------ reset

; The VDP after power-up, with the game's register values ($81E8):
; everything clear, the planes empty.
vdp_reset:
    ldx #0
:   stz z:VDP_REGS, x
    inx
    inx
    cpx #24
    bcc :-
    ldx #0
:   phx
    ldrom ROM_VDP_REGISTER_DEFAULTS, x
    jsr vdp_register
    plx
    inx
    inx
    cpx #19 * 2
    bcc :-
    lda f:MDROM + ROM_VDP_REGISTER_DEFAULTS
    sta a:VDP_MODE_SHADOW
    lda f:MDROM + ROM_VDP_REGISTER_DEFAULTS + 2
    sta a:VDP_MODE_SHADOW + 2
    lda #1
    sta z:vdp_on
    lda #0
    ldx #0
:   sta f:VRAM_MIRROR, x
    inx
    inx
    bne :-
    jsr forget_slots
    ldx #0
    lda #0
:   sta f:tile_gen, x
    inx
    inx
    cpx #2048
    bcc :-
    lda #0                  ; every entry is tile 0: give it slot 0
    jsr convert_entry
    ldx #0
:   sta f:map_a, x          ; (map_a, map_b and map_w are one block)
    inx
    inx
    cpx #MAPS_SIZE
    bcc :-
    sep #$20
    .a8
    lda #1
    ldx #0
:   sta f:rows_a, x
    inx
    cpx #ROWS_SIZE
    bcc :-
    rep #$20
    .a16
    rts

; Forget the slots and convert every name table again (when they run
; out): every map row is uploaded again.
reconvert:
    jsr forget_slots
    ldx #PLANE_A
@word:
    phx
    lda f:VRAM_MIRROR, x
    xba
    jsr map_word
    plx
    inx
    inx
    bne @word
    rts

forget_slots:
    ldx #0
    lda #0
:   sta f:md2snes, x
    inx
    inx
    cpx #2048 * 2
    bcc :-
    stz z:next_slot
    stz z:pend_head
    stz z:pend_tail
    ldx #0
:   sta f:tile_dirty, x
    inx
    inx
    cpx #256
    bcc :-
    rts

; ---------------------------------------------------------------- port

; Set the address from a command, A = its high word (written first), X =
; its low word: CD1 CD0 A13-A0, then CD5-CD2 at bits 5-2 and A15 A14.
vdp_command:
    sta z:cmd_hi
    and #$3FFF
    sta z:vdp_addr
    txa
    and #3
    xba
    asl
    asl
    asl
    asl
    asl
    asl
    ora z:vdp_addr
    sta z:vdp_addr
    lda z:cmd_hi
    xba
    lsr
    lsr
    lsr
    lsr
    lsr
    lsr
    and #3
    sta z:vdp_code
    txa
    lsr
    lsr
    and #$3C
    ora z:vdp_code
    sta z:vdp_code
    rts

; A register write, A = $8000 | register << 8 | value.
vdp_register:
    pha
    xba
    and #$1F
    tax
    pla
    sep #$20
    .a8
    sta z:VDP_REGS, x
    rep #$20
    .a16
    rts

; Write A through the data port.
vdp_write:
    ldx z:vdp_code
    cpx #1
    bne @not_vram
    ldx z:vdp_addr
    jsr vram_word
    bra @next
@not_vram:
    cpx #5
    bne @next               ; (the palette goes through the RAM buffer)
    ldx z:vdp_addr
    cpx #$50
    bcs @next
    sta f:vsram, x
@next:
    lda z:VDP_REGS + 15
    and #$FF
    clc
    adc z:vdp_addr
    sta z:vdp_addr
    rts

; Read a word through the data port into A (VRAM reads only).
vdp_read:
    ldx z:vdp_addr
    lda f:VRAM_MIRROR, x
    xba
    pha
    lda z:VDP_REGS + 15
    and #$FF
    clc
    adc z:vdp_addr
    sta z:vdp_addr
    pla
    rts

; ----------------------------------------------------------- VRAM words

; Write the word A at VRAM address X: into the copy, marking its tile
; changed, and into the SNES maps if it is in a name table. Writing what
; is there already does nothing (the menus redraw texts every frame).
vram_word:
    xba
    cmp f:VRAM_MIRROR, x
    bne :+
    rts
:   sta f:VRAM_MIRROR, x
    xba
    pha
    phx
    txa
    lsr
    lsr
    lsr
    lsr
    lsr
    jsr mark_tile
    plx
    pla
    cpx #PLANE_A
    bcc :+
    jmp map_word
:   rts

; Mark tile A changed (and count the change, for sprites.s).
mark_tile:
    pha
    tax
    sep #$20
    .a8
    lda f:tile_gen, x
    inc
    sta f:tile_gen, x
    rep #$20
    .a16
    lda 1, s
    lsr
    lsr
    lsr
    tay                     ; the byte
    pla
    and #7
    tax
    sep #$20
    .a8
    lda f:bit_masks, x
    tyx
    ora f:tile_dirty, x
    sta f:tile_dirty, x
    rep #$20
    .a16
    rts

bit_masks: .byte $01, $02, $04, $08, $10, $20, $40, $80

; The name table entry A at VRAM address X (>= PLANE_A), into the maps.
map_word:
    stx z:map_off
    jsr convert_entry
    sta z:entry
    lda z:map_off
    cmp #PLANE_B
    bcs @plane_b
    sec
    sbc #PLANE_A
    jsr map_index
    lda z:entry
    sta f:map_a, x
    tyx
    sep #$20
    .a8
    lda #1
    sta f:rows_a, x
    rep #$20
    .a16
    lda z:map_off
    cmp #WINDOW
    bcc @done
    sec
    sbc #WINDOW
    jsr map_index
    lda z:entry
    sta f:map_w, x
    tyx
    sep #$20
    .a8
    lda #1
    sta f:rows_w, x
    rep #$20
    .a16
@done:
    ldx z:map_off
    rts
@plane_b:
    sec
    sbc #PLANE_B
    jsr map_index
    lda z:entry
    sta f:map_b, x
    tyx
    sep #$20
    .a8
    lda #1
    sta f:rows_b, x
    rep #$20
    .a16
    ldx z:map_off
    rts

; Byte offset A in a 64-wide Genesis name table -> X = byte offset in the
; SNES map (32x32 screens: left/right, then top/bottom), Y = row.
map_index:
    pha
    asl
    xba
    and #$003F              ; row = offset >> 7
    tay
    pla
    lsr
    and #$003F              ; col
    sta z:t0
    and #$0020              ; the right-hand screens: + 1024 entries
    beq :+
    lda #1024
:   sta z:t1
    tya
    and #$0020              ; the bottom ones: + 2048
    beq :+
    lda #2048
:   ora z:t1
    sta z:t1
    lda z:t0
    and #$001F
    ora z:t1
    sta z:t1
    tya
    and #$001F
    asl
    asl
    asl
    asl
    asl
    ora z:t1
    asl                     ; bytes
    tax
    rts

; Genesis entry A (P PL PL V H, tile) -> SNES (V H P PAL PAL PAL, tile);
; the tile gets a slot if it has none.
convert_entry:
    pha
    and #$07FF
    asl
    tax
    lda f:md2snes, x
    bne :+
    jsr new_slot
:   dec
    sta z:tile
    pla
    xba
    lsr
    lsr
    and #$3E                ; (entry >> 11) * 2
    tax
    lda f:entry_attributes, x
    ora z:tile
    rts

; Give the tile at X/2 the next slot; A = slot + 1.
new_slot:
    lda z:next_slot
    cmp #MAX_SLOTS
    bcc :+
    lda #1                  ; out of slots: show slot 0
    rts
:   inc
    sta f:md2snes, x
    sta z:next_slot
    txa
    lsr
    jsr pend
    lda z:next_slot
    rts

; Queue tile A for upload to its slot.
pend:
    ldx z:pend_tail
    sta f:pending, x
    inx
    inx
    cpx #PENDING_SIZE
    bcc :+
    ldx #0
:   stx z:pend_tail
    rts

; P PL1 PL0 V H -> V H P PL1 PL0 of a SNES entry, for (entry >> 11)
entry_attributes:
.repeat 32, I
    .word ((I >> 1) & 1) << 15 | (I & 1) << 14 | ((I >> 4) & 1) << 13 | ((I >> 2) & 3) << 10
.endrep

; --------------------------------------------------------- bulk helpers

; ptr (24 bits) from a Genesis address in A (high word) and X (low):
; RAM ($FFxxxx) is the mirror, the rest the ROM copy. Both are
; big-endian.
source:
    cmp #$00FF
    bne :+
    stx z:ptr
    lda #^MIRROR
    sta z:ptr + 2
    rts
:   clc
    adc #^MDROM
    sta z:ptr + 2
    stx z:ptr
    rts

; Copy Y words from the Genesis address A:X (high and low words) to the
; VDP, after the command in cmd_hi/cmd_lo ($83DC).
vdp_copy:
    sty z:count
    jsr source
    lda z:cmd_hi
    ldx z:cmd_lo
    jsr vdp_command
    ldy #0
:   lda [ptr], y
    xba
    phy
    jsr vdp_write
    ply
    iny
    iny
    dec z:count
    bne :-
    rts

; Write the word A Y times after the command in cmd_hi/cmd_lo.
vdp_fill:
    pha
    sty z:count
    lda z:cmd_hi
    ldx z:cmd_lo
    jsr vdp_command
:   lda 1, s
    jsr vdp_write
    dec z:count
    bne :-
    pla
    rts

; Copy a width x rows block of name table entries from the Genesis
; address A:X, each row 128 bytes below the last ($840E). The command is
; in cmd_hi/cmd_lo.
vdp_copy_rect:
    jsr source
    lda z:cmd_hi
    ldx z:cmd_lo
    jsr vdp_command
    ldy #0
@row:
    lda z:vdp_addr
    pha
    lda z:width
    sta z:count
:   lda [ptr], y
    xba
    phy
    jsr vdp_write
    ply
    iny
    iny
    dec z:count
    bne :-
    pla
    clc
    adc #128
    sta z:vdp_addr
    dec z:rows
    bne @row
    rts

; Zero A bytes of VRAM from address X (a DMA fill with 0).
vdp_clear:
    sta z:count
:   lda #0
    phx
    jsr vram_word
    plx
    inx
    inx
    dec z:count
    dec z:count
    bne :-
    rts

; Load tiles into VRAM at the current address: Y tiles in Genesis format
; at A:X (24 bits; the build's decoded Nemesis data).
vdp_tiles:
    stx z:ptr
    sta z:ptr + 2
    sty z:count
    ldy #0
@tile:
    ldx z:vdp_addr
    cpx #PLANE_A
    bcc @fast
    lda #16                 ; into the name tables: word by word
    sta z:width
:   lda [ptr], y
    xba
    phy
    ldx z:vdp_addr
    jsr vram_word
    ply
    iny
    iny
    lda z:vdp_addr
    clc
    adc #2
    sta z:vdp_addr
    dec z:width
    bne :-
    bra @next
@fast:
    lda #16
    sta z:width
:   lda [ptr], y
    sta f:VRAM_MIRROR, x
    iny
    iny
    inx
    inx
    dec z:width
    bne :-
    lda z:vdp_addr
    lsr
    lsr
    lsr
    lsr
    lsr
    phy
    jsr mark_tile
    ply
    lda z:vdp_addr
    clc
    adc #32
    sta z:vdp_addr
@next:
    dec z:count
    bne @tile
    rts

; ------------------------------------------------------------------ flush

; Anything still to upload? (A nonzero if so.)
vdp_pending:
    lda z:pend_head
    eor z:pend_tail
    bne :+
    jsr dirty_rows
:   rts

; Queue this frame's uploads: changed tiles that have slots, then the map
; rows that changed, as far as the budgets go; then the scroll and the
; layers from the VDP registers.
vdp_flush:
    lda z:next_slot         ; slots running out: start again from the maps
    cmp #MAX_SLOTS - 128
    bcc :+
    jsr reconvert
:   jsr dirty_tiles
    jsr upload_tiles
    jsr upload_rows
    jmp ppu_setup

; Changed tiles with a slot join the pending list.
dirty_tiles:
    ldx #0
@byte:
    lda f:tile_dirty, x
    and #$00FF
    beq @next
    sta z:count
    sep #$20
    .a8
    lda #0
    sta f:tile_dirty, x
    rep #$20
    .a16
    txa
    asl
    asl
    asl
    sta z:tile
    phx
@bit:
    lsr z:count
    bcc @skip
    lda z:tile
    asl
    tax
    lda f:md2snes, x
    beq @skip
    lda z:tile
    jsr pend
@skip:
    inc z:tile
    lda z:count
    bne @bit
    plx
@next:
    inx
    cpx #256
    bcc @byte
    rts

; Convert pending tiles into the staging buffer and queue them; tiles
; going to consecutive slots make one upload.
upload_tiles:
    stz z:stage
    lda #UPLOAD_TILES
    sta z:budget
    lda #$FFFF
    sta z:run_end
@tile:
    ldx z:pend_head
    cpx z:pend_tail
    beq @done
    dec z:budget
    bmi @done
    lda #32
    jsr upload_fits
    bcc @done
    lda f:pending, x
    sta z:tile
    inx
    inx
    cpx #PENDING_SIZE
    bcc :+
    ldx #0
:   stx z:pend_head
    ; its 32 bytes from the VRAM copy
    lda z:tile
    asl
    asl
    asl
    asl
    asl
    sta z:src
    lda #^VRAM_MIRROR
    sta z:src + 2
    ldx z:stage
    jsr planar_tile
    ; its slot's VRAM address (16 words a tile)
    lda z:tile
    asl
    tax
    lda f:md2snes, x
    dec
    asl
    asl
    asl
    asl
    cmp z:run_end
    bne @new
    ldx z:up_pos            ; right after the last upload: make it longer
    lda f:UPLOADS - 8 + 6, x
    clc
    adc #32
    sta f:UPLOADS - 8 + 6, x
    lda z:run_end
    bra @staged
@new:
    sta z:up_dst
    lda z:stage
    clc
    adc #.loword(tile_staging)
    sta z:up_src
    lda #^tile_staging
    sta z:up_src + 2
    lda #32
    sta z:up_len
    jsr upload_vram
    lda z:up_dst
@staged:
    clc
    adc #16
    sta z:run_end
    lda z:stage
    clc
    adc #32
    sta z:stage
    jmp @tile
@done:
    rts

; The Genesis tile at [src] into SNES planar form at tile_staging + X.
planar_tile:
    ldy #0
@row:
    lda [src], y
    and #$00FF
    asl
    asl
    phx
    tax
    lda f:planar_lut, x
    sta z:row01
    lda f:planar_lut + 2, x
    sta z:row23
    iny
    lda [src], y
    and #$00FF
    asl
    asl
    tax
    lda f:planar_lut + $400, x
    tsb z:row01
    lda f:planar_lut + $402, x
    tsb z:row23
    iny
    lda [src], y
    and #$00FF
    asl
    asl
    tax
    lda f:planar_lut + $800, x
    tsb z:row01
    lda f:planar_lut + $802, x
    tsb z:row23
    iny
    lda [src], y
    and #$00FF
    asl
    asl
    tax
    lda f:planar_lut + $C00, x
    tsb z:row01
    lda f:planar_lut + $C02, x
    tsb z:row23
    iny
    plx
    lda z:row01
    sta f:tile_staging, x
    lda z:row23
    sta f:tile_staging + 16, x
    inx
    inx
    cpy #32
    bcc @row
    rts

; Any map rows waiting? (A nonzero if so.)
dirty_rows:
    ldx #0
:   lda f:rows_a, x
    bne :+
    inx
    inx
    cpx #ROWS_SIZE
    bcc :-
    lda #0
:   rts

; Queue the changed map rows, up to UPLOAD_ROWS of them.
upload_rows:
    lda #UPLOAD_ROWS
    sta z:budget
    ldy #0                  ; index in rows_a, rows_b, rows_w
@row:
    tyx
    lda f:rows_a, x
    and #$00FF
    beq @next
    dec z:budget
    bmi @done
    lda #128
    jsr upload_fits
    bcc @done
    tyx
    sep #$20
    .a8
    lda #0
    sta f:rows_a, x
    rep #$20
    .a16
    tya
    cmp #64
    bcs :+
    sta z:t0
    lda #PLANE_A_MAP
    ldx #.loword(map_a)
    bra @queue
:   cmp #128
    bcs :+
    sbc #64 - 1             ; (carry clear)
    sta z:t0
    lda #PLANE_B_MAP
    ldx #.loword(map_b)
    bra @queue
:   sbc #128
    sta z:t0
    lda #WINDOW_MAP
    ldx #.loword(map_w)
@queue:
    sta z:up_dst
    stx z:up_src
    phy
    jsr queue_row
    ply
@next:
    iny
    cpy #ROWS_SIZE
    bcc @row
@done:
    rts

; Queue row t0 of the map at up_src (VRAM up_dst): its left and right
; halves, each 32 entries in a different screen.
queue_row:
    lda z:t0
    and #$0020
    beq :+
    lda #2048
:   sta z:t1
    lda z:t0
    and #$001F
    asl
    asl
    asl
    asl
    asl
    ora z:t1                ; the left half's first entry
    pha
    clc
    adc z:up_dst
    sta z:up_dst
    pla
    asl
    clc
    adc z:up_src
    sta z:up_src
    lda #^map_a
    sta z:up_src + 2
    lda #64
    sta z:up_len
    jsr upload_vram
    lda z:up_dst
    clc
    adc #1024
    sta z:up_dst
    lda z:up_src
    clc
    adc #2048
    sta z:up_src
    jmp upload_vram

; The SNES layers and scroll from the VDP registers: display on/off, BG1
; showing the window plane (when it covers the screen) or plane A, and the
; first line's scroll.
ppu_setup:
    sep #$20
    .a8
    lda z:VDP_REGS + 1
    and #$40
    beq :+
    lda #$0F
    bra :++
:   lda #$80
:   sta f:SH_INIDISP
    lda #$01
    sta f:SH_BGMODE
    lda #(PLANE_B_MAP >> 10 << 2) | 3
    sta f:SH_BG2SC
    lda #0
    sta f:SH_BG12NBA
    lda #$13
    sta f:SH_TM
    lda #$00 | 3            ; 8x8 and 16x16 OBJs at $6000
    sta f:SH_OBSEL
    lda z:VDP_REGS + 18     ; the window from the top over all 28 rows?
    rep #$20
    .a16
    and #$009F
    cmp #28
    bcc @plane_a
    cmp #$80                ; (bit 7: from the bottom)
    bcs @plane_a
    sep #$20
    .a8
    lda #(WINDOW_MAP >> 10 << 2) | 1
    sta f:SH_BG1SC
    rep #$20
    .a16
    lda #CROP
    sta f:SH_BG1HOFS
    lda #$FFFF
    sta f:SH_BG1VOFS
    bra @plane_b
@plane_a:
    sep #$20
    .a8
    lda #(PLANE_A_MAP >> 10 << 2) | 3
    sta f:SH_BG1SC
    rep #$20
    .a16
    lda f:VRAM_MIRROR + HSCROLL
    xba
    eor #$FFFF
    sec
    adc #CROP               ; CROP - scroll
    and #$03FF
    sta f:SH_BG1HOFS
    ; (the vertical scroll, VSRAM 0)
    lda f:vsram
    dec
    and #$03FF
    sta f:SH_BG1VOFS
@plane_b:
    lda f:VRAM_MIRROR + HSCROLL + 2
    xba
    eor #$FFFF
    sec
    adc #CROP
    and #$03FF
    sta f:SH_BG2HOFS
    lda f:vsram + 2
    dec
    and #$03FF
    sta f:SH_BG2VOFS
    rts

.segment "BWRAM"
PENDING_SIZE = 4096
ROWS_SIZE    = 64 + 64 + 32
MAPS_SIZE    = (64 * 64 + 64 * 64 + 64 * 32) * 2
map_a:          .res 64 * 64 * 2
map_b:          .res 64 * 64 * 2
map_w:          .res 64 * 32 * 2
md2snes:        .res 2048 * 2       ; slot + 1 of each Genesis tile, 0 = none
tile_dirty:     .res 256 + 1        ; a bit per tile
rows_a:         .res 64             ; a byte per map row: A, then B, window
rows_b:         .res 64
rows_w:         .res 32 + 1
pending:        .res PENDING_SIZE
tile_gen:       .res 2048           ; a count of each tile's changes
tile_staging:   .res UPLOAD_TILES * 32
vsram:          .res $50
