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
.export vdp_ctrl_write, vdp_data_write, vdp_data_read, vdp_status
.export vdp_tiles, vdp_flush, vdp_copy, vdp_copy_rect, vdp_fill, vblank__write_words
.export vdp_pending, tile_gen, hdma_bufs
.import upload, upload_vram, upload_fits

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

UPLOAD_TILES = 128              ; at most this many tiles per frame,
UPLOAD_ROWS  = 48               ; and map rows

; direct page (SA1_DP + offset; sa1.s uses $00-$1F, logo.s $20-$3F)
vdp_addr    = $40       ; VRAM/CRAM/VSRAM address
vdp_code    = $42       ; 1 VRAM write, 3 CRAM write, 5 VSRAM write, 0 VRAM read
next_slot   = $44       ; next free background slot
cmd_hi      = $46       ; vdp_command: the command's high word
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
ctrl_pending = $72      ; the first half of a command was written
cmd_first   = $74       ; (that half)
fill_pending = $76      ; a DMA fill waits for its data word
dma_left    = $78       ; DMA: words left,
dma_src     = $7A       ; source (24 bits; $7A-$7C)
cram_dirty  = $7E

; the HDMA builder's own (I-RAM, past the direct page's other users)
hdma_cur    = $D8       ; (free since the modes are compiled C)
win_end     = $DA
win_down    = $DC
vs_table    = $DE
hs_mode     = $9A
hline       = $9C
gline       = $9E
hsc         = $A0
hofs        = $A2
row_md      = $A4       ; convert_row: the name table's address,
row_off     = $A6       ;  the entry's offset in it,
row_left    = $A8       ;  entries left
hs_ptr      = $AA       ; hdma_build: the horizontal scroll table (24 bits)
hfast       = $AE       ;  and whether the race's per-line form is in use
dv_changed  = $B6       ; dma_vram: words of the tile changed (it runs in the
                        ;  VBlank interrupt: past race_build_road's $80-$B5)
vofs_a      = $B8       ; hdma_build with whole-screen vertical scroll: the
vofs_b      = $BA       ;  layers' VOFS values, and that form's use
vconst      = $BC
; race_lines's (road.s's $80-$8F: idle while the HDMA tables are built)
rl_count    = $80
rl_v        = $82
rl_last_a   = $84
rl_last_b   = $86
rl_gline    = $88
rl_sc       = $8A       ; BG1SC above the window's edge line, and below
; upload_rows's (the same: idle then)
ur_lo       = $8C       ; the row's changed columns
ur_hi       = $8E
ur_dst      = $90       ; the map's VRAM address
ur_src      = $92       ; and its copy (bank of map_a)

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
    stz z:ctrl_pending
    stz z:fill_pending
    stz z:hdma_cur
    lda #0
    sta f:hdma_valid
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
    ldx #0
:   lda #1                  ; every row, every column
    sta f:rows_a, x
    lda #0
    sta f:row_lo, x
    lda #63
    sta f:row_hi, x
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
    cpx #3
    bne :+
    pha                     ; CRAM
    lda z:vdp_addr
    and #$007E
    tax
    pla
    sta f:cram, x
    lda #1
    sta z:cram_dirty
    bra @next
:   cpx #5
    bne @next
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

; ------------------------------------------------------------ the ports

; A word written to the control port: a register, or half of a command.
; A command with CD5 set starts a DMA (when register 1 allows it).
vdp_ctrl_write:
    ldx z:ctrl_pending
    bne @second
    pha
    and #$C000
    cmp #$8000
    bne @first
    pla
    jmp vdp_register
@first:
    pla
    sta z:cmd_first
    lda #1
    sta z:ctrl_pending
    rts
@second:
    stz z:ctrl_pending
    tax
    lda z:cmd_first
    jsr vdp_command
    lda z:vdp_code
    and #$0020
    beq @done
    lda z:vdp_code
    and #$001F
    sta z:vdp_code
    lda z:VDP_REGS + 1
    and #$0010
    beq @done
    lda z:VDP_REGS + 23
    and #$00C0
    cmp #$0080
    beq @fill
    bcs @done               ; (VRAM copies: not used)
    jmp dma_copy
@fill:
    lda #1
    sta z:fill_pending
@done:
    rts

; A word written to the data port (the data of a pending DMA fill).
vdp_data_write:
    stz z:ctrl_pending
    ldx z:fill_pending
    beq :+
    jmp dma_fill
:   jmp vdp_write

; A word read from the data port.
vdp_data_read:
    stz z:ctrl_pending
    jmp vdp_read

; The status register: the FIFO empty, no DMA running.
vdp_status:
    stz z:ctrl_pending
    lda #$3400
    rts

; The DMA length (registers 19-20; 0 = 65536 words) into dma_left.
dma_length:
    lda z:VDP_REGS + 19
    and #$00FF
    sta z:dma_left
    lda z:VDP_REGS + 20
    and #$00FF
    xba
    ora z:dma_left
    sta z:dma_left
    rts

; A DMA from 68000 memory: the source (registers 21-23, a word address)
; through the data port, as the target the command set.
dma_copy:
    jsr dma_length
    lda z:VDP_REGS + 21     ; source = (r23 & $7F) << 17 | r22 << 9 | r21 << 1
    and #$00FF
    asl
    sta z:dma_src
    lda z:VDP_REGS + 22
    and #$00FF
    xba
    asl
    ora z:dma_src
    sta z:dma_src
    lda z:VDP_REGS + 22
    and #$0080
    asl                     ; (r22 bit 7 -> bit 8 -> the bank's bit 0)
    xba
    sta z:dma_src + 2
    lda z:VDP_REGS + 23
    and #$007F
    asl
    ora z:dma_src + 2
    cmp #$00E0
    bcc :+
    lda #^MIRROR
    bra :++
:   clc
    adc #^MDROM
:   sta z:dma_src + 2
; (vdp_copy too) dma_left words from dma_src through the data port.
dma_words:
    lda z:vdp_code          ; to VRAM a word at a time: dma_vram
    cmp #1
    bne @word
    lda z:VDP_REGS + 15
    and #$00FF
    cmp #2
    bne @word
    jmp dma_vram
@word:
    lda [dma_src]
    xba
    jsr vdp_write
    lda z:dma_src
    clc
    adc #2
    sta z:dma_src
    dec z:dma_left
    bne @word
    rts

; vdp_copy(uint32_t command, uint32_t src, int count) (src/game/video.c,
; left out of the compiled C): the command, then count words from src
; through the data port, as a DMA would send them, 64 at a time with the
; interrupt masked. The arguments' slots keep what is left: count at 4, s,
; the words in this round at 6, s, the source (as a SNES address) at 8, s.
vdp_copy:
    php
    sei
    lda 15, s               ; the command, high word first
    jsr vdp_ctrl_write
    lda 13, s
    jsr vdp_ctrl_write
    plp
; write_words(uint32_t src, int count) (vblank.c): the same without the
; command.
vblank__write_words:
    lda 10, s               ; the RAM mirror, or the ROM copy
    and #$00FF
    cmp #$00E0
    bcc :+
    lda #^MIRROR
    bra :++
:   clc
    adc #^MDROM
:   sta 10, s
@round:
    lda 4, s
    beq @done
    cmp #65
    bcc :+
    lda #64
:   sta 6, s
    eor #$FFFF
    sec
    adc 4, s
    sta 4, s
    php
    sei
    lda 7, s
    sta z:dma_left
    lda 9, s
    sta z:dma_src
    lda 11, s
    sta z:dma_src + 2
    jsr dma_words
    plp
    lda 6, s                ; past this round's words
    asl
    clc
    adc 8, s
    sta 8, s
    lda 10, s
    adc #0
    sta 10, s
    bra @round
@done:
    rtl

; vdp_copy_rect(uint32_t command, uint32_t src, int width, int height)
; (video.c): vdp_copy for each row, each a plane row (128 bytes) below the
; last, the source going on. (Each six pushes of 18, s copy the command,
; the source and the width as vdp_copy's arguments.)
vdp_copy_rect:
@row:
    lda 4, s                ; rows left
    beq @done
    dec
    sta 4, s
.repeat 6
    lda 18, s
    pha
.endrep
    jsl vdp_copy
    tsc
    clc
    adc #12
    tcs
    lda 8, s                ; the source past the row
    asl
    clc
    adc 12, s
    sta 12, s
    lda 14, s
    adc #0
    sta 14, s
    lda 18, s               ; the command a row down
    clc
    adc #$0080
    sta 18, s
    bra @row
@done:
    rtl

; vdp_fill(uint32_t command, uint16_t value, int count) (video.c): the
; command, then the value count times through the data port, 64 at a time
; with the interrupt masked (count at 4, s, this round's at 6, s).
vdp_fill:
    php
    sei
    lda 13, s
    jsr vdp_ctrl_write
    lda 11, s
    jsr vdp_ctrl_write
    plp
@round:
    lda 4, s
    beq @done
    cmp #65
    bcc :+
    lda #64
:   sta 6, s
    eor #$FFFF
    sec
    adc 4, s
    sta 4, s
    php
    sei
:   lda 9, s
    jsr vdp_data_write
    lda 7, s
    dec
    sta 7, s
    bne :-
    plp
    bra @round
@done:
    rtl

; dma_copy's usual case: to VRAM with an increment of 2. vram_word's work
; a tile (16 words) at a time: the words compared as stored (big-endian)
; and copied, then the tile marked changed, and its map row, if any were.
dma_vram:
    ldx z:vdp_addr
    ldy #0
    stz z:dv_changed
@word:
    lda [dma_src], y
    cmp f:VRAM_MIRROR, x
    beq :+
    sta f:VRAM_MIRROR, x
    inc z:dv_changed
:   iny
    iny
    inx
    inx
    dec z:dma_left
    beq @last
    txa
    and #$001F
    bne @word
    jsr dv_tile             ; a tile done
    bra @word
@last:
    jsr dv_tile
    stx z:vdp_addr
    tya
    clc
    adc z:dma_src
    sta z:dma_src
    rts

; dma_vram: mark the tile before address X, if it changed.
dv_tile:
    lda z:dv_changed
    beq @done
    stz z:dv_changed
    phx
    phy
    txa
    dec
    dec
    pha
    lsr
    lsr
    lsr
    lsr
    lsr
    jsr mark_tile
    plx
    cpx #PLANE_A
    bcc :+
    jsr map_word            ; its map row: the tile's last column,
    txa
    and #$FFE0
    tax
    jsr map_word            ;  and its first (map_word keeps X)
:   ply
    plx
@done:
    rts

; A DMA fill (the game only fills with 0): the length's bytes plus one
; from the address, zeroed.
dma_fill:
    stz z:fill_pending
    jsr dma_length
    lda z:dma_left
    inc
    inc
    lsr
    sta z:dma_left          ; words
    lda z:vdp_addr
    and #$FFFE
    tax
@word:
    phx
    lda #0
    jsr vram_word
    pla
    inc
    inc
    tax
    dec z:dma_left
    bne @word
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

; A name table entry was written at VRAM address X (>= PLANE_A): its map
; row (plane A, the window plane, plane B) is to be converted again. That
; happens at the flush, and only while the row is on screen (the race
; keeps its sprite and scroll tables in plane A rows it never shows).
map_word:
    stx z:map_off
    txa
    cmp #PLANE_B
    bcs @plane_b
    sec
    sbc #PLANE_A
    asl
    xba
    and #$003F              ; row = offset >> 7
    tax
    jsr mark_row
    lda z:map_off
    cmp #WINDOW
    bcc @done
    sec
    sbc #WINDOW
    asl
    xba
    and #$001F
    clc
    adc #rows_w - rows_a
    tax
    jsr mark_row
@done:
    ldx z:map_off
    rts
@plane_b:
    sec
    sbc #PLANE_B
    asl
    xba
    and #$003F
    clc
    adc #rows_b - rows_a
    tax
    jsr mark_row
    ldx z:map_off
    rts

; Row X (an index in rows_a, then rows_b, rows_w) changed at map_off's
; column: flagged, and its range of changed columns widened to it.
mark_row:
    lda z:map_off
    and #$007E
    lsr
    sep #$20
    .a8
    pha
    lda #1
    sta f:rows_a, x
    pla
    cmp f:row_lo, x
    bcs :+
    sta f:row_lo, x
:   cmp f:row_hi, x
    bcc :+
    sta f:row_hi, x
:   rep #$20
    .a16
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
    jsr cram_upload
    lda z:next_slot         ; slots running out: start again from the maps
    cmp #MAX_SLOTS - 128
    bcc :+
    jsr reconvert
:   jsr ppu_setup           ; (the rows on screen, for upload_rows)
    jsr upload_rows
    jsr dirty_tiles
    jmp upload_tiles

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
    bra @tile
@exit:
    rts
@tile:
    ldx z:pend_head
    cpx z:pend_tail
    beq @exit
    dec z:budget
    bmi @exit
    lda #32
    jsr upload_fits
    bcc @exit
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
    lda z:up_list           ; right after the last upload: make it longer
    clc
    adc z:up_pos
    tax
    lda f:$410000 - 8 + 6, x
    clc
    adc #32
    sta f:$410000 - 8 + 6, x
    lda z:up_budget
    sec
    sbc #32
    bcs :+
    lda #0
:   sta z:up_budget
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
    and f:vis_a, x          ; changed, and on screen
    and #$00FF
    bne :+
    jmp @next
:   dec z:budget
    bpl :+
    rts
:   lda #128
    jsr upload_fits
    bcs :+
    rts
:   stz z:ur_lo             ; the changed columns (and none now)
    stz z:ur_hi
    tyx
    sep #$20
    .a8
    lda #0
    sta f:rows_a, x
    lda f:row_lo, x
    sta z:ur_lo
    lda f:row_hi, x
    sta z:ur_hi
    lda #$7F
    sta f:row_lo, x
    lda #0
    sta f:row_hi, x
    rep #$20
    .a16
    tya
    cmp #64
    bcs :+
    sta z:t0
    lda #PLANE_A
    sta z:row_md
    lda #PLANE_A_MAP
    ldx #.loword(map_a)
    bra @queue
:   cmp #128
    bcs :+
    sbc #64 - 1             ; (carry clear)
    sta z:t0
    lda #PLANE_B
    sta z:row_md
    lda #PLANE_B_MAP
    ldx #.loword(map_b)
    bra @queue
:   sbc #128
    sta z:t0
    lda #WINDOW
    sta z:row_md
    lda #WINDOW_MAP
    ldx #.loword(map_w)
@queue:
    sta z:ur_dst
    stx z:ur_src
    stx z:up_src
    phy
    jsr convert_row
    jsr queue_row
    ply
@next:
    iny
    cpy #ROWS_SIZE
    bcs @done
    jmp @row
@done:
    rts

; Convert row t0 of the name table at row_md into the map at up_src:
; columns ur_lo to ur_hi.
convert_row:
    lda z:t0
    pha                     ; (map_index uses t0)
    xba
    lsr                     ; row * 128
    sta z:row_off
    lda z:ur_lo
    asl
    clc
    adc z:row_off
    sta z:row_off
    lda z:ur_hi
    sec
    sbc z:ur_lo
    inc
    sta z:row_left
@entry:
    lda z:row_md
    clc
    adc z:row_off
    tax
    lda f:VRAM_MIRROR, x
    xba
    jsr convert_entry
    sta z:entry
    lda z:row_off
    jsr map_index           ; (X = the byte offset in the map)
    txa
    clc
    adc z:up_src
    tax
    lda z:entry
    sta f:map_a & $FF0000, x
    lda z:row_off
    clc
    adc #2
    sta z:row_off
    dec z:row_left
    bne @entry
    pla
    sta z:t0
    rts

; Queue row t0 of the map at ur_src (VRAM ur_dst), columns ur_lo to
; ur_hi: in the left half (columns 0-31) and the right (32-63), each half
; in a different screen.
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
    ora z:t1
    pha                     ; the left half's first entry
    lda z:ur_lo
    cmp #32
    bcs @right
    lda z:ur_hi             ; columns ur_lo to min(ur_hi, 31)
    cmp #32
    bcc :+
    lda #31
:   sec
    sbc z:ur_lo
    inc
    asl
    sta z:up_len
    lda 1, s
    clc
    adc z:ur_lo
    jsr queue_part
@right:
    lda z:ur_hi
    cmp #32
    bcc @done
    lda z:ur_lo             ; columns max(ur_lo, 32) to ur_hi
    cmp #32
    bcs :+
    lda #32
:   sta z:t1
    lda z:ur_hi
    sec
    sbc z:t1
    inc
    asl
    sta z:up_len
    lda 1, s
    clc
    adc #1024 - 32          ; (the right screen)
    clc
    adc z:t1
    jsr queue_part
@done:
    pla
    rts

; Upload up_len bytes of the map from entry A (from ur_src, to ur_dst).
queue_part:
    pha
    clc
    adc z:ur_dst
    sta z:up_dst
    pla
    asl
    clc
    adc z:ur_src
    sta z:up_src
    lda #^map_a
    sta z:up_src + 2
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
    rep #$20
    .a16
    jmp hdma_build

; ---------------------------------------------------------------- HDMA

; Each line's BG1 map and both layers' scroll, as HDMA tables (the NMI
; points channels 1-3 at them, snes/boot.s): BG1 shows the window plane on
; the lines the window covers (the race's HUD), plane A elsewhere. The
; horizontal scroll comes from the table in VRAM ($CC00: whole screen,
; per 8 lines or per line, register 11); the vertical one from VSRAM, or
; in races from the table the HBlank interrupt copies a line at a time
; ($FF1500 + the buffer select at $FF0580: line l takes entry l).
; 225 lines: the SNES's line 0 is not shown and copies line 1.
HDMA_LINES  = 225
SC_SIZE     = 2 + HDMA_LINES + 1          ; two headers, the bytes, the end
OFS_SIZE    = 2 + HDMA_LINES * 4 + 1
HDMA_BUF    = SC_SIZE + 2 * OFS_SIZE      ; one set of tables
VSCROLL_LINES = $1500
SCROLL_BUFFER_SEL = $0580

hdma_build:
    ; the window's lines: from the top (or bottom) to row R
    lda z:VDP_REGS + 18
    and #$001F
    asl
    asl
    asl
    sta z:win_end           ; R * 8
    lda z:VDP_REGS + 18
    and #$0080
    sta z:win_down
    ; per-line vertical scroll in races
    stz z:vs_table
    lda z:VDP_REGS
    and #$0010              ; line interrupts on
    beq :+
    ldw VBLANK_HANDLER
    cmp #8
    bne :+
    ldw SCROLL_BUFFER_SEL
    clc
    adc #VSCROLL_LINES
    sta z:vs_table
:   lda z:VDP_REGS + 11
    and #$0003
    sta z:hs_mode
    lda z:vs_table          ; one vertical scroll and nothing changed:
    bne :+                  ;  the tables shown stay
    jsr hdma_same
    bcc :+
    rts
:   lda z:hdma_cur          ; the set not shown now
    eor #HDMA_BUF
    sta z:hdma_cur
    stz z:hfast             ; per-line scroll both ways: race_lines
    stz z:vconst
    lda #HSCROLL
    sta z:hs_ptr
    lda #^VRAM_MIRROR
    sta z:hs_ptr + 2
    lda z:vs_table
    bne :+
    inc z:vconst            ; one vertical scroll: line_vconst
    bra :++
:   lda z:hs_mode
    cmp #3
    bne :+
    lda z:vs_table
    sta z:hfast
:   ldx #0                  ; no rows on screen yet
    lda #0
:   sta f:vis_a, x
    inx
    inx
    cpx #ROWS_SIZE
    bcc :-
    lda z:vconst
    beq :+
    jsr vconst_setup
:
    ; headers: 112 and 113 lines, one entry each
    ldx z:hdma_cur
    sep #$20
    .a8
    lda #$80 | 112
    sta f:hdma_bufs, x
    sta f:hdma_bufs + SC_SIZE, x
    sta f:hdma_bufs + SC_SIZE + OFS_SIZE, x
    rep #$20
    .a16
    stz z:hline             ; SNES line
    lda z:hdma_cur
    inc
    sta z:hsc               ; where the next BG1SC byte goes
    clc
    adc #SC_SIZE
    sta z:hofs              ; and the next scroll entries (BG2's OFS_SIZE on)
hdma_line:
    lda z:hline
    beq @first
    cmp #2
    bcc @dec
    ldx z:vconst
    beq @race
    jmp line_vconst
@race:
    ldx z:hfast
    beq @dec
    jmp race_lines
@dec:
    dec                     ; the Genesis line
@first:
    sta z:gline
    ; BG1: the window, or plane A
    jsr in_window
    bcc @plane_a
    lda z:gline             ; the window's row is on screen
    lsr
    lsr
    lsr
    tax
    sep #$20
    .a8
    lda #1
    sta f:vis_w, x
    lda #(WINDOW_MAP >> 10 << 2) | 1
    ldx z:hsc
    sta f:hdma_bufs, x
    rep #$20
    .a16
    lda #CROP
    ldx z:hofs
    sta f:hdma_bufs, x
    lda #$FFFF
    sta f:hdma_bufs + 2, x
    bra @plane_b
@plane_a:
    sep #$20
    .a8
    lda #(PLANE_A_MAP >> 10 << 2) | 3
    ldx z:hsc
    sta f:hdma_bufs, x
    rep #$20
    .a16
    jsr hscroll_entry       ; X = its VRAM address
    lda f:VRAM_MIRROR, x
    xba
    eor #$FFFF
    sec
    adc #CROP               ; CROP - scroll
    and #$03FF
    ldx z:hofs
    sta f:hdma_bufs, x
    jsr vscroll_a
    jsr mark_a
    dec
    and #$03FF
    ldx z:hofs
    sta f:hdma_bufs + 2, x
@plane_b:
    jsr hscroll_entry
    lda f:VRAM_MIRROR + 2, x
    xba
    eor #$FFFF
    sec
    adc #CROP
    and #$03FF
    ldx z:hofs
    sta f:hdma_bufs + OFS_SIZE, x
    jsr vscroll_b
    jsr mark_b
    dec
    and #$03FF
    ldx z:hofs
    sta f:hdma_bufs + OFS_SIZE + 2, x
    ; next line; after 112, the second header
hdma_next:
    inc z:hsc
    lda z:hofs
    clc
    adc #4
    sta z:hofs
    inc z:hline
    lda z:hline
    cmp #112
    bne :+
    ldx z:hsc
    sep #$20
    .a8
    lda #$80 | (HDMA_LINES - 112)
    sta f:hdma_bufs, x
    rep #$20
    .a16
    ldx z:hofs
    sep #$20
    .a8
    sta f:hdma_bufs, x
    sta f:hdma_bufs + OFS_SIZE, x
    rep #$20
    .a16
    inc z:hsc
    inc z:hofs
:   lda z:hline
    cmp #HDMA_LINES
    beq hdma_ends
    jmp hdma_line
hdma_ends:
    ldx z:hsc               ; the ends
    sep #$20
    .a8
    lda #0
    sta f:hdma_bufs, x
    ldx z:hofs
    sta f:hdma_bufs, x
    sta f:hdma_bufs + OFS_SIZE, x
    rep #$20
    .a16
    lda z:hdma_cur
    sta f:HDMA_TABLES
    rts

; The race's lines from the second on (per-line scroll both ways): the
; general case's work, a table at a time. hsc and hofs: line 2's entries.
race_lines:
    ; the BG1SC bytes: the window on one side of its edge line
    lda z:win_down
    bne :+
    lda #((WINDOW_MAP >> 10 << 2) | 1) | (((PLANE_A_MAP >> 10 << 2) | 3) << 8)
    bra :++
:   lda #((PLANE_A_MAP >> 10 << 2) | 3) | (((WINDOW_MAP >> 10 << 2) | 1) << 8)
:   sta z:rl_sc
    ldx z:hsc
    ldy #1                  ; the Genesis line
    sep #$20
    .a8
@sc:
    lda z:rl_sc
    cpy z:win_end
    bcc :+
    lda z:rl_sc + 1
:   sta f:hdma_bufs, x
    inx
    iny
    cpy #111                ; after line 110 (the SNES's 111): the second header
    bne :+
    lda #$80 | (HDMA_LINES - 112)
    sta f:hdma_bufs, x
    inx
:   cpy #224
    bcc @sc
    rep #$20
    .a16
    stx z:hsc
    ; the window's rows on screen
    lda z:win_end
    cmp #224
    bcc :+
    lda #224
:   ldx z:win_down
    bne @bottom
    ldy #1                  ; lines 1 up to the edge
    bra @mark
@bottom:
    tay                     ; lines from the edge (at least 1) to 224
    bne :+
    ldy #1
:   lda #224
@mark:
    php
    sei                     ; (mark_window's t0: the interrupt's VDP code)
    jsr mark_window
    plp
    ; the scroll entries, with the second header between
    lda #$FFFF
    sta z:rl_last_a
    sta z:rl_last_b
    lda #1
    sta z:rl_gline
    ldx z:hofs
    ldy #4
    lda #110
    sta z:rl_count
    jsr race_offsets
    sep #$20
    .a8
    lda #$80 | (HDMA_LINES - 112)
    sta f:hdma_bufs, x
    sta f:hdma_bufs + OFS_SIZE, x
    rep #$20
    .a16
    inx
    lda #113
    sta z:rl_count
    jsr race_offsets
    stx z:hofs
    jmp hdma_ends

; rl_count lines' BG1 and BG2 scroll entries from X (rl_gline: the first
; line's number, Y its offset in both scroll tables).
race_offsets:
@line:
    lda z:rl_gline
    cmp z:win_end
    lda z:win_down
    bne :+
    bcc @window
    bra @plane_a
:   bcs @window
@plane_a:
    lda [hs_ptr], y
    xba
    eor #$FFFF
    sec
    adc #CROP
    and #$03FF
    sta f:hdma_bufs, x
    lda (vs_table), y
    xba
    sta z:rl_v
    dec
    and #$03FF
    sta f:hdma_bufs + 2, x
    lda z:rl_v              ; the row on screen (once per row)
    clc
    adc z:rl_gline
    and #$01F8
    cmp z:rl_last_a
    beq @plane_b
    sta z:rl_last_a
    phx
    lsr
    lsr
    lsr
    tax
    sep #$20
    .a8
    lda #1
    sta f:vis_a, x
    rep #$20
    .a16
    plx
    bra @plane_b
@window:
    lda #CROP
    sta f:hdma_bufs, x
    lda #$FFFF
    sta f:hdma_bufs + 2, x
@plane_b:
    iny
    iny
    lda [hs_ptr], y
    xba
    eor #$FFFF
    sec
    adc #CROP
    and #$03FF
    sta f:hdma_bufs + OFS_SIZE, x
    lda (vs_table), y
    xba
    sta z:rl_v
    dec
    and #$03FF
    sta f:hdma_bufs + OFS_SIZE + 2, x
    lda z:rl_v
    clc
    adc z:rl_gline
    and #$01F8
    cmp z:rl_last_b
    beq :+
    sta z:rl_last_b
    phx
    lsr
    lsr
    lsr
    tax
    sep #$20
    .a8
    lda #1
    sta f:vis_b, x
    rep #$20
    .a16
    plx
:   iny
    iny
    inx
    inx
    inx
    inx
    inc z:rl_gline
    dec z:rl_count
    beq :+
    jmp @line
:   rts

; Are hdma_build's inputs (with one vertical scroll per layer) those of
; the last build? Carry set if so; if not, they are noted for next time.
hdma_same:
    lda f:hdma_valid
    beq @changed
    lda z:VDP_REGS
    cmp f:hdma_sig
    bne @changed
    lda z:VDP_REGS + 10
    cmp f:hdma_sig + 2
    bne @changed
    lda z:VDP_REGS + 18
    cmp f:hdma_sig + 4
    bne @changed
    ldw VBLANK_HANDLER
    cmp f:hdma_sig + 6
    bne @changed
    lda f:vsram
    cmp f:hdma_sig + 8
    bne @changed
    lda f:vsram + 2
    cmp f:hdma_sig + 10
    bne @changed
    ldx #HS_BYTES - 2
:   lda f:VRAM_MIRROR + HSCROLL, x
    cmp f:hs_copy, x
    bne @changed
    dex
    dex
    bpl :-
    sec
    rts
@changed:
    lda z:VDP_REGS
    sta f:hdma_sig
    lda z:VDP_REGS + 10
    sta f:hdma_sig + 2
    lda z:VDP_REGS + 18
    sta f:hdma_sig + 4
    ldw VBLANK_HANDLER
    sta f:hdma_sig + 6
    lda f:vsram
    sta f:hdma_sig + 8
    lda f:vsram + 2
    sta f:hdma_sig + 10
    ldx #HS_BYTES - 2
:   lda f:VRAM_MIRROR + HSCROLL, x
    sta f:hs_copy, x
    dex
    dex
    bpl :-
    lda #1
    sta f:hdma_valid
    clc
    rts

; Whole-screen vertical scroll: the VOFS values, and the rows on screen
; marked once (lines 0-223) rather than line by line. (With the interrupt
; masked: its VDP code shares t0-tile.)
vconst_setup:
    php
    sei
    jsr vconst_marks
    plp
    rts
vconst_marks:
    lda f:vsram
    pha
    dec
    and #$03FF
    sta z:vofs_a
    lda f:vsram + 2
    pha
    dec
    and #$03FF
    sta z:vofs_b
    ; the window's lines, and plane A's on the others
    lda z:win_end
    cmp #224
    bcc :+
    lda #224
:   sta z:t1                ; the window's edge line, at most 224
    plx                     ; plane B: every line
    lda #vis_b - vis_a
    ldy #0
    jsr mark_range_224
    lda z:win_down
    bne @bottom
    lda z:t1                ; window above the edge, plane A below
    ldy #0
    jsr mark_window
    plx
    lda z:t1
    tay
    lda #224
    bra @a
@bottom:
    lda #224                ; plane A above the edge, window below
    ldy z:t1
    jsr mark_window
    plx
    ldy #0
    lda z:t1
@a: sta z:t0                ; plane A: lines Y up to A, scroll X
    tya
    cmp z:t0
    bcs @done
    lda z:t0
    sta z:count
    lda #0
    jmp mark_range
@done:
    rts

; vis_w for Genesis lines Y up to A.
mark_window:
    sta z:t0
    tya
    cmp z:t0
    bcs @done
    lsr
    lsr
    lsr
    tax
    lda z:t0
    dec
    lsr
    lsr
    lsr
    sta z:t0
    sep #$20
    .a8
    lda #1
:   sta f:vis_w, x
    inx
    cpx z:t0
    bcc :-
    beq :-
    rep #$20
    .a16
@done:
    rts

; Mark the rows lines Y up to 224 show with vertical scroll X, in the table
; at vis_a + A.
mark_range_224:
    pha
    lda #224
    sta z:count
    pla
; (the same up to line count)
mark_range:
    sta z:t0                ; the table's offset
    txa                     ; first row: (scroll + Y) / 8
    sta z:width
    clc
    tya
    adc z:width
    and #$01F8
    lsr
    lsr
    lsr
    sta z:rows
    lda z:count             ; last row: (scroll + count - 1) / 8
    dec
    clc
    adc z:width
    and #$01F8
    lsr
    lsr
    lsr
    sta z:tile
    lda z:rows
@row:
    clc
    adc z:t0
    tax
    sep #$20
    .a8
    lda #1
    sta f:vis_a, x
    rep #$20
    .a16
    lda z:rows
    cmp z:tile
    beq @done
    inc
    and #$003F
    sta z:rows
    bra @row
@done:
    rts

; A line from the second on with one vertical scroll per layer, A = hline:
; its horizontal scroll per the mode, the window or plane A.
line_vconst:
    dec
    sta z:gline
    ldx z:hs_mode
    beq @whole
    cpx #2
    bne :+
    and #$FFF8              ; per 8 lines
:   asl
    asl
    tay
    bra :+
@whole:
    ldy #0
:   lda z:gline
    cmp z:win_end
    lda z:win_down
    bne :+
    bcc @window
    bra @plane_a
:   bcs @window
@plane_a:
    sep #$20
    .a8
    lda #(PLANE_A_MAP >> 10 << 2) | 3
    ldx z:hsc
    sta f:hdma_bufs, x
    rep #$20
    .a16
    lda [hs_ptr], y
    xba
    eor #$FFFF
    sec
    adc #CROP
    and #$03FF
    ldx z:hofs
    sta f:hdma_bufs, x
    lda z:vofs_a
    sta f:hdma_bufs + 2, x
    bra @plane_b
@window:
    sep #$20
    .a8
    lda #(WINDOW_MAP >> 10 << 2) | 1
    ldx z:hsc
    sta f:hdma_bufs, x
    rep #$20
    .a16
    lda #CROP
    ldx z:hofs
    sta f:hdma_bufs, x
    lda #$FFFF
    sta f:hdma_bufs + 2, x
@plane_b:
    iny
    iny
    lda [hs_ptr], y
    xba
    eor #$FFFF
    sec
    adc #CROP
    and #$03FF
    ldx z:hofs
    sta f:hdma_bufs + OFS_SIZE, x
    lda z:vofs_b
    sta f:hdma_bufs + OFS_SIZE + 2, x
    jmp hdma_next

; Plane A's (B's) row shown on line gline with vertical scroll A, on
; screen; A is kept.
mark_a:
    pha
    clc
    adc z:gline
    and #$01F8
    lsr
    lsr
    lsr
    tax
    sep #$20
    .a8
    lda #1
    sta f:vis_a, x
    rep #$20
    .a16
    pla
    rts

mark_b:
    pha
    clc
    adc z:gline
    and #$01F8
    lsr
    lsr
    lsr
    tax
    sep #$20
    .a8
    lda #1
    sta f:vis_b, x
    rep #$20
    .a16
    pla
    rts

; Is Genesis line gline in the window? (carry)
in_window:
    lda z:gline
    cmp z:win_end
    lda z:win_down
    bne :+
    bcc @yes                ; from the top: lines above row R
    clc
    rts
:   bcs @yes                ; from the bottom: lines from row R
    clc
    rts
@yes:
    sec
    rts

; X = the VRAM address of line gline's horizontal scroll entry.
hscroll_entry:
    lda z:hs_mode
    beq @screen
    cmp #2
    lda z:gline
    bcc @line               ; (mode 1: as per line)
    bne @line
    and #$FFF8              ; per 8 lines
@line:
    asl
    asl
    clc
    adc #HSCROLL
    tax
    rts
@screen:
    ldx #HSCROLL
    rts

; Plane A's and B's vertical scroll on line gline.
vscroll_a:
    lda z:vs_table
    beq @vsram
    lda z:gline
    beq @vsram
    asl
    asl
    clc
    adc z:vs_table
    tax
    ldw 0, x
    rts
@vsram:
    lda f:vsram
    rts

vscroll_b:
    lda z:vs_table
    beq @vsram
    lda z:gline
    beq @vsram
    asl
    asl
    clc
    adc z:vs_table
    tax
    ldw 2, x
    rts
@vsram:
    lda f:vsram + 2
    rts

; The colours (CRAM) as SNES colours, if they changed: to CGRAM 0-63 (the
; BG palettes) and 128-191 (the OBJ palettes); the backdrop is the colour
; register 7 picks.
cram_upload:
    lda z:cram_dirty
    beq @done
    lda #128 + 128
    jsr upload_fits
    bcc @done
    stz z:cram_dirty
    ldx #0
@colour:
    lda f:cram, x           ; ----BBB-GGG-RRR-
    sta z:t0
    and #$000E
    jsr level
    sta z:t1
    lda z:t0
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
    tsb z:t1
    lda z:t0
    xba
    and #$000E
    jsr level
    xba
    asl
    asl
    ora z:t1
    sta f:cgram_buffer, x
    inx
    inx
    cpx #128
    bne @colour
    lda z:VDP_REGS + 7
    and #$003F
    asl
    tax
    lda f:cgram_buffer, x
    sta f:cgram_buffer
    lda #.loword(cgram_buffer)
    sta z:up_src
    lda #^cgram_buffer
    sta z:up_src + 2
    stz z:up_dst
    lda #128
    sta z:up_len
    lda #UP_CGRAM
    jsr upload
    lda #128
    sta z:up_dst
    lda #UP_CGRAM
    jsr upload
@done:
    rts

; A colour channel, 3 bits at bit 1 (A = 2v), as 5 bits: v << 2 | v >> 1.
level:
    sta z:count
    asl
    sta z:width
    lda z:count
    lsr
    lsr
    ora z:width
    rts

.segment "BWRAM"
hdma_bufs:      .res 2 * HDMA_BUF
HS_BYTES = 224 * 4
hdma_valid:     .res 2              ; hdma_same: the inputs below are set,
hdma_sig:       .res 12             ;  registers 0, 10-11, 18, the VBlank
hs_copy:        .res HS_BYTES       ;  handler, VSRAM, the scroll table
cram:           .res 128
cgram_buffer:   .res 128
PENDING_SIZE = 2048
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
row_lo:         .res ROWS_SIZE + 1  ; each row's changed columns (row_lo
row_hi:         .res ROWS_SIZE + 1  ;  above row_hi: none)
vis_a:          .res 64             ; rows on screen this frame, laid out as
vis_b:          .res 64             ;  rows_a, rows_b, rows_w
vis_w:          .res 32 + 1
pending:        .res PENDING_SIZE
tile_gen:       .res 2048           ; a count of each tile's changes
tile_staging:   .res UPLOAD_TILES * 32
vsram:          .res $50
