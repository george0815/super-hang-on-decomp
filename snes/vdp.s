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
.export vdp_tiles, vdp_flush
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
    bne dma_fill
    jmp vdp_write

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
    asl
    xba
    and #$001F
    tax
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
    asl
    xba
    and #$003F
    tax
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
    sta z:up_dst
    stx z:up_src
    phy
    jsr convert_row
    jsr queue_row
    ply
@next:
    iny
    cpy #ROWS_SIZE
    bcc @row
@done:
    rts

; Convert row t0 of the name table at row_md into the map at up_src.
convert_row:
    lda z:t0
    pha                     ; (map_index uses t0)
    xba
    lsr                     ; row * 128
    sta z:row_off
    lda #64
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
    lda z:hdma_cur          ; the set not shown now
    eor #HDMA_BUF
    sta z:hdma_cur
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
    ldx #0                  ; no rows on screen yet
    lda #0
:   sta f:vis_a, x
    inx
    inx
    cpx #ROWS_SIZE
    bcc :-
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
@line:
    lda z:hline
    beq :+
    dec                     ; the Genesis line
:   sta z:gline
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
    beq :+
    jmp @line
:   ldx z:hsc               ; the ends
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
vis_a:          .res 64             ; rows on screen this frame, laid out as
vis_b:          .res 64             ;  rows_a, rows_b, rows_w
vis_w:          .res 32 + 1
pending:        .res PENDING_SIZE
tile_gen:       .res 2048           ; a count of each tile's changes
tile_staging:   .res UPLOAD_TILES * 32
vsram:          .res $50
