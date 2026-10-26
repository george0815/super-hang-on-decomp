; Objects' sprite frames as SNES OBJs.
;
; A frame is an object's sprite map with its tile base: pieces of 1-4
; tiles a side whose tiles sit in Genesis VRAM. Most maps put every piece
; on one 8-pixel grid; such a frame is cut into 16x16 OBJs by moving whole
; tiles: a cell grid is filled with the pieces' tiles (earlier pieces on
; top), and each 2x2 group of cells becomes an OBJ (one per palette and
; priority in it) whose four tiles go into a 16x16 block of OBJ VRAM.
; Aligned blocks never overlap, which keeps the OBJ slices on a line low.
;
; Built frames stay cached with their blocks while they are drawn; a
; frame is built again when one of its tiles changes (tile_gen counts
; each tile's changes, vdp.s). Tiles are converted to SNES form once per
; change, into a cache in BW-RAM bank $43.
;
; Maps that are not on one grid, and frames over 128 pixels, are not
; drawn yet.

.p816
.smart
.include "inc/hw.inc"
.include "inc/mem.inc"
.include "inc/dp.inc"
.include "inc/macros.inc"
.include "gen/assets.inc"

.export sprites_reset, sprites_begin, sprite_object, sprites_end
.import upload, upload_vram, tile_gen

VRAM_MIRROR = $420000
PLANAR      = $430000   ; the SNES form of each Genesis tile, 32 bytes
OBJ_TILES   = $6000     ; OBJ VRAM (word address), 512 tiles
BLOCKS      = 128       ; 16x16 blocks of it
ENTRIES     = 48        ; cached frames
MAX_BOXES   = 32
ENTRY_SIZE  = 10 + MAX_BOXES * 8
GRID        = 16        ; cells a side (128 pixels)
BLOCK_BUDGET = 24       ; new blocks per frame
CROP        = 32

; an entry
E_MAP       = 0         ; 0: free
E_TILE0     = 2
E_SUM       = 4         ; tile_gen sum of its tiles when built
E_USED      = 6         ; the frame it was last drawn in
E_BOXES     = 8
E_BOX       = 10        ; boxes: block, attributes, dx (word), dy (word),
                        ; then the quads drawn as 8x8 OBJs (0: one 16x16)

OBJ_TILE_BASE   = $02
OBJ_SCREEN_X    = $10
OBJ_SCREEN_Y    = $14
OBJ_SPRITE_MAP  = $24

; this file's own direct page (I-RAM)
SPRITES_DP  = $3200
frame       = $00
oam_n       = $02       ; OBJs so far
obj         = $04
map         = $06       ; 24 bits
tile0       = $0A
x0          = $0C
y0          = $0E
sum         = $10
entry       = $12       ; its offset in entries
pieces      = $14
nbox        = $16
first_x     = $18
first_y     = $1A
min_x       = $1C
min_y       = $1E
max_x       = $20
max_y       = $22
gw          = $24
gh          = $26
pw          = $28       ; the piece: width, height (cells), x, y, entry
ph          = $2A
px          = $2C
pyy         = $2E
pt          = $30
col         = $32
row         = $34
cell        = $36
bx          = $38
by          = $3A
key         = $3C
block       = $3E
budget      = $40
src         = $42       ; 24 bits
dst         = $46       ; 24 bits
t0          = $4A
t1          = $4C
quad        = $4E
oam_x       = $50
oam_y       = $52
best        = $54
best_age    = $56
stage_pos   = $58
row01       = $5A
row23       = $5C
qmask       = $66       ; compose: the quads drawn
big         = $68       ; emit: 16x16 (2) or 8x8 (0)
quads       = $6A       ; draw_entry: quads left to draw as 8x8
usrc        = $5E       ; 24 bits: an upload's source,
udst        = $62       ; VRAM word address,
ulen        = $64       ; length

.segment "CODE"
.a16
.i16

.macro enter
    phd
    pea SPRITES_DP
    pld
.endmacro

; Nothing cached, every tile to be converted.
sprites_reset:
    enter
    ldx #0
    lda #$FFFF
:   sta f:planar_gen, x
    inx
    inx
    cpx #2048
    bcc :-
    ldx #0
    lda #0
:   sta f:block_owner, x
    inx
    inx
    cpx #BLOCKS
    bcc :-
    ldx #0
:   lda #0
    sta f:entries + E_MAP, x
    sta f:entries + E_BOXES, x
    txa
    clc
    adc #ENTRY_SIZE
    tax
    cpx #ENTRIES * ENTRY_SIZE
    bcc :-
    stz z:frame
    pld
    rts

; A new frame's OBJs.
sprites_begin:
    enter
    inc z:frame
    stz z:oam_n
    stz z:stage_pos
    lda #BLOCK_BUDGET
    sta z:budget
    ldx #0
    lda #0
:   sta f:oam_high, x
    inx
    inx
    cpx #32
    bcc :-
    pld
    rts

; The frame's OBJs: the rest hidden, then the OAM to the upload list.
sprites_end:
    enter
    lda z:oam_n
    asl
    asl
    tax
:   cpx #128 * 4
    bcs :+
    lda #$F000              ; x 0, y 240
    sta f:oam, x
    lda #0
    sta f:oam + 2, x
    inx
    inx
    inx
    inx
    bra :-
:   lda #.loword(oam)
    sta z:usrc
    lda #^oam
    sta z:usrc + 2
    stz z:udst
    lda #544
    sta z:ulen
    lda #UP_OAM
    jsr queue
    pld
    rts

; Queue an upload of kind A (UP_VRAM, UP_OAM) from usrc/udst/ulen, through
; sa1.s on its direct page.
queue:
    pha
    lda z:usrc
    sta f:SA1_DP + up_src
    lda z:usrc + 1
    sta f:SA1_DP + up_src + 1
    lda z:udst
    sta f:SA1_DP + up_dst
    lda z:ulen
    sta f:SA1_DP + up_len
    pla
    phd
    pea SA1_DP
    pld
    jsr upload
    pld
    rts

; ------------------------------------------------------------- objects

; Draw the object at X: its frame from the cache (built if need be), then
; an OBJ per box.
sprite_object:
    enter
    stx z:obj
    ldw OBJ_SPRITE_MAP, x
    sta z:map
    cmp #$8000
    lda #^MDROM
    bcc :+
    lda #^MIRROR
:   sta z:map + 2
    ldx z:obj
    ldw OBJ_TILE_BASE, x
    sta z:tile0
    ldw OBJ_SCREEN_X, x
    sta z:x0
    ldw OBJ_SCREEN_Y, x
    sta z:y0
    jsr frame_sum
    jsr find_entry
    bcc @draw
    lda z:entry
    cmp #$FFFF
    beq @done               ; no entry to spare
    jsr build_entry
@draw:
    jsr draw_entry
@done:
    pld
    rts

; sum = the tile_gen sum over every tile of the frame.
frame_sum:
    stz z:sum
    lda [map]
    xba
    inc
    sta z:pieces
    ldy #2
@piece:
    jsr piece
    lda #0                  ; cells = w * h
    ldx z:ph
:   clc
    adc z:pw
    dex
    bne :-
    sta z:t1
    lda z:pt
    and #$07FF
    tax
:   lda f:tile_gen, x
    and #$00FF
    clc
    adc z:sum
    sta z:sum
    inx
    txa
    and #$07FF
    tax
    dec z:t1
    bne :-
    dec z:pieces
    bne @piece
    rts

; Find map and tile0 in the cache: carry clear, entry = it, to be drawn as
; is. Carry set: entry is to be built (its tiles changed, or it is new),
; or $FFFF if no entry can be spared.
find_entry:
    ldx #0
@entry:
    lda f:entries + E_MAP, x
    beq @next
    cmp z:map
    bne @next
    lda f:entries + E_TILE0, x
    cmp z:tile0
    bne @next
    stx z:entry
    lda z:frame
    sta f:entries + E_USED, x
    lda f:entries + E_SUM, x
    cmp z:sum
    bne :+
    clc
    rts
:   jsr free_entry_blocks   ; its tiles changed: build it again
    sec
    rts
@next:
    txa
    clc
    adc #ENTRY_SIZE
    tax
    cpx #ENTRIES * ENTRY_SIZE
    bcc @entry
    jsr take_entry
    sec
    rts

; entry = a free entry, or else the one drawn longest ago (not this
; frame), emptied and given map and tile0; $FFFF if there is none.
take_entry:
    lda #$FFFF
    sta z:best
    stz z:best_age
    ldx #0
@entry:
    lda f:entries + E_MAP, x
    bne :+
    stx z:best
    bra @take
:   lda z:frame
    sec
    sbc f:entries + E_USED, x
    beq @next
    cmp z:best_age
    bcc @next
    sta z:best_age
    stx z:best
@next:
    txa
    clc
    adc #ENTRY_SIZE
    tax
    cpx #ENTRIES * ENTRY_SIZE
    bcc @entry
@take:
    lda z:best
    sta z:entry
    cmp #$FFFF
    beq @none
    jsr free_entry_blocks
    ldx z:entry
    lda z:map
    sta f:entries + E_MAP, x
    lda z:tile0
    sta f:entries + E_TILE0, x
    lda z:frame
    sta f:entries + E_USED, x
@none:
    rts

; Free the blocks of entry and empty its box list.
free_entry_blocks:
    ldx z:entry
    lda f:entries + E_BOXES, x
    beq @done
    sta z:nbox
    txa
    clc
    adc #E_BOX
    tax
@box:
    phx
    lda f:entries, x        ; the block
    and #$00FF
    tax
    sep #$20
    .a8
    lda #0
    sta f:block_owner, x
    rep #$20
    .a16
    pla
    clc
    adc #8
    tax
    dec z:nbox
    bne @box
    ldx z:entry
    lda #0
    sta f:entries + E_BOXES, x
@done:
    rts

; ------------------------------------------------------------- drawing

; The OBJs of each box of the entry, at the object's position: one
; 16x16, or an 8x8 per quad drawn.
draw_entry:
    ldx z:entry
    lda f:entries + E_BOXES, x
    bne :+
    rts
:   sta z:nbox
    txa
    clc
    adc #E_BOX
    tax
@box:
    phx
    lda f:entries, x        ; block, attributes
    sta z:t0
    and #$00FF
    jsr block_tile
    sta z:block             ; (its first tile)
    lda f:entries + 2, x
    clc
    adc z:x0
    sec
    sbc #128 + CROP
    sta z:col               ; (the box's x)
    lda f:entries + 4, x
    clc
    adc z:y0
    sec
    sbc #128
    sta z:row               ; (y)
    lda f:entries + 6, x
    and #$00FF
    bne @quads
    lda #2
    sta z:big
    lda z:col
    sta z:oam_x
    lda z:row
    sta z:oam_y
    lda z:block
    sta z:t1
    jsr emit
    bra @next
@quads:
    sta z:quads
    stz z:big
    stz z:quad
@quad:
    lsr z:quads
    bcc @skip
    lda z:quad              ; x + 8 (right), y + 8 (bottom)
    and #1
    asl
    asl
    asl
    clc
    adc z:col
    sta z:oam_x
    lda z:quad
    and #2
    asl
    asl
    clc
    adc z:row
    sta z:oam_y
    lda z:quad              ; tile + 1 (right), + 16 (bottom)
    and #1
    sta z:t1
    lda z:quad
    and #2
    asl
    asl
    asl
    ora z:t1
    clc
    adc z:block
    sta z:t1
    jsr emit
@skip:
    inc z:quad
    lda z:quads
    bne @quad
@next:
    pla
    clc
    adc #8
    tax
    dec z:nbox
    beq :+
    jmp @box
:   rts

; An OBJ at oam_x, oam_y with tile t1 and the attributes in t0's high
; byte, 16x16 if big; off the screen it is left out.
emit:
    lda z:oam_n
    cmp #128
    bcs @out
    lda z:oam_x
    clc
    adc #16
    cmp #256 + 16
    bcs @out
    lda z:oam_y
    clc
    adc #16
    cmp #224 + 16
    bcs @out
    lda z:oam_n
    asl
    asl
    tax
    sep #$20
    .a8
    lda z:oam_x
    sta f:oam, x
    lda z:oam_y
    sta f:oam + 1, x
    lda z:t1
    sta f:oam + 2, x
    lda z:t0 + 1
    ora z:t1 + 1            ; (tile bit 8)
    sta f:oam + 3, x
    rep #$20
    .a16
    lda z:oam_x             ; the high table: x bit 8, size
    xba
    and #1
    ora z:big
    beq :+
    jsr high_bits
:   inc z:oam_n
@out:
    rts

; Block A's first OBJ tile: (A >> 3) * 32 + (A & 7) * 2.
block_tile:
    pha
    and #7
    asl
    sta z:t1
    pla
    and #$F8
    asl
    asl
    ora z:t1
    rts

; OR the two bits in A into OBJ oam_n's place in the high table.
high_bits:
    pha
    lda z:oam_n
    lsr
    lsr
    tax                     ; its byte
    lda z:oam_n
    and #3
    tay
    pla
:   cpy #0
    beq :+
    asl
    asl
    dey
    bra :-
:   sep #$20
    .a8
    ora f:oam_high, x
    sta f:oam_high, x
    rep #$20
    .a16
    rts

; ------------------------------------------------------------- building

; Build the frame into entry: the cell grid, then its boxes.
build_entry:
    ldx z:entry
    lda z:sum
    sta f:entries + E_SUM, x
    lda #0
    sta f:entries + E_BOXES, x
    jsr measure
    bcs @skip               ; not on one grid, or too big
    jsr fill_grid
    jsr cut_boxes
@skip:
    rts

; The frame's extent; carry set if its pieces are not on one grid or it
; spans more than GRID cells.
measure:
    lda [map]
    xba
    inc
    sta z:pieces
    ldy #2
    jsr piece
    lda z:px                ; the first piece starts the extent
    sta z:first_x
    sta z:min_x
    sta z:max_x
    lda z:pyy
    sta z:first_y
    sta z:min_y
    sta z:max_y
@piece:
    lda z:px
    sec
    sbc z:first_x
    and #7
    bne @off_grid
    lda z:pyy
    sec
    sbc z:first_y
    and #7
    bne @off_grid
    lda z:px
    cmp z:min_x
    bpl :+
    sta z:min_x
:   lda z:pyy
    cmp z:min_y
    bpl :+
    sta z:min_y
:   lda z:pw
    asl
    asl
    asl
    clc
    adc z:px
    cmp z:max_x
    bmi :+
    sta z:max_x
:   lda z:ph
    asl
    asl
    asl
    clc
    adc z:pyy
    cmp z:max_y
    bmi :+
    sta z:max_y
:   dec z:pieces
    beq @done
    jsr piece
    bra @piece
@done:
    lda z:max_x
    sec
    sbc z:min_x
    lsr
    lsr
    lsr
    sta z:gw
    cmp #GRID + 1
    bcs @off_grid
    lda z:max_y
    sec
    sbc z:min_y
    lsr
    lsr
    lsr
    sta z:gh
    cmp #GRID + 1
    bcs @off_grid
    clc
    rts
@off_grid:
    sec
    rts

; The piece at map + Y into pw, ph, px, pyy, pt; Y past it.
piece:
    lda [map], y            ; y offset (from -256), size
    pha
    and #$00FF
    sec
    sbc #256
    sta z:pyy
    pla
    xba
    and #$00FF
    sta z:t0
    lsr
    lsr
    and #3
    inc
    sta z:pw
    lda z:t0
    and #3
    inc
    sta z:ph
    iny
    iny
    lda [map], y            ; tile and attributes, plus the tile base
    xba
    clc
    adc z:tile0
    sta z:pt
    iny
    iny
    lda [map], y
    xba
    sta z:px
    iny
    iny
    rts

; The pieces' tiles into the cell grid, last piece first so earlier ones
; end up on top. A cell holds the Genesis entry (attributes and tile);
; cells_used says it is used (1), or drawn already (2).
fill_grid:
    ldx #0
    lda #0
:   sta f:cells_used, x
    inx
    inx
    cpx #GRID * GRID
    bcc :-
    lda [map]
    xba
    sta z:pieces            ; (count - 1): the last piece's index
@piece:
    lda z:pieces            ; piece n is at 2 + 6n
    asl
    clc
    adc z:pieces
    asl
    inc
    inc
    tay
    jsr piece
    stz z:col
@col:
    stz z:row
@row:
    ; tiles go down the columns: pt + col * h + row
    lda #0
    ldx z:col
    beq :++
:   clc
    adc z:ph
    dex
    bne :-
:   clc
    adc z:row
    clc
    adc z:pt
    and #$07FF
    sta z:t1
    lda z:pt
    and #$F800
    ora z:t1
    sta z:t1                ; the cell's entry
    ; its place, mirrored by the flips
    lda z:pt
    and #$0800
    beq :+
    lda z:pw
    clc
    sbc z:col               ; w - 1 - col
    bra :++
:   lda z:col
:   sta z:t0
    lda z:px
    sec
    sbc z:min_x
    lsr
    lsr
    lsr
    clc
    adc z:t0
    sta z:t0                ; cell x
    lda z:pt
    and #$1000
    beq :+
    lda z:ph
    clc
    sbc z:row               ; h - 1 - row
    bra :++
:   lda z:row
:   sta z:cell
    lda z:pyy
    sec
    sbc z:min_y
    lsr
    lsr
    lsr
    clc
    adc z:cell              ; cell y
    asl
    asl
    asl
    asl                     ; x GRID
    clc
    adc z:t0
    tax
    sep #$20
    .a8
    lda #1
    sta f:cells_used, x
    rep #$20
    .a16
    txa
    asl
    tax
    lda z:t1
    sta f:cells, x
    inc z:row
    lda z:row
    cmp z:ph
    bcc @row
    inc z:col
    lda z:col
    cmp z:pw
    bcs :+
    jmp @col
:   dec z:pieces
    bmi :+
    jmp @piece
:   rts

; Each 2x2 group of cells becomes an OBJ per palette and priority in it.
cut_boxes:
    stz z:by
@by:
    stz z:bx
@bx:
    ; the first used cell of the group not drawn yet gives the key
    stz z:quad
@find:
    jsr quad_cell
    bcs :+
    lda f:cells, x
    and #$E000              ; priority and palette
    sta z:key
    jsr new_block
    bcs @full
    jsr compose
    jsr add_box
    bcs @full
    bra @bx                 ; (again: cells with another key may remain)
:   inc z:quad
    lda z:quad
    cmp #4
    bcc @find
    lda z:bx
    inc
    sta z:bx
    asl
    cmp z:gw
    bcs :+
    jmp @bx
:   lda z:by
    inc
    sta z:by
    asl
    cmp z:gh
    bcs :+
    jmp @by
:   rts
@full:
    ldx z:entry             ; out of blocks or boxes: try again next frame
    lda #$FFFF
    sta f:entries + E_SUM, x
    rts

; Cell quad (0-3: left/right, top/bottom) of group bx, by: carry clear and
; X = its index x2 if it is used and not drawn yet.
quad_cell:
    lda z:quad
    and #1
    sta z:t0
    lda z:bx
    asl
    clc
    adc z:t0
    cmp z:gw
    bcs @no
    sta z:t0                ; cell x
    lda z:quad
    lsr
    sta z:t1
    lda z:by
    asl
    clc
    adc z:t1
    cmp z:gh
    bcs @no
    asl
    asl
    asl
    asl
    clc
    adc z:t0
    tax
    lda f:cells_used, x
    and #$00FF
    cmp #1
    bne @no
    txa
    asl
    tax
    clc
    rts
@no:
    sec
    rts

; block = a free OBJ VRAM block (owned from now on); carry set if there
; is none, or this frame's budget is spent.
new_block:
    lda z:budget
    beq @none
@search:
    ldx #0
:   lda f:block_owner, x
    and #$00FF
    beq @take
    inx
    cpx #BLOCKS
    bcc :-
    jsr evict               ; none free: free the frame drawn longest ago
    bcc @search
@none:
    sec
    rts
@take:
    stx z:block
    sep #$20
    .a8
    lda #1
    sta f:block_owner, x
    rep #$20
    .a16
    dec z:budget
    clc
    rts

; Free the blocks of the entry drawn longest ago (not this frame); carry
; set if there is none.
evict:
    lda z:entry
    pha
    lda #$FFFF
    sta z:best
    stz z:best_age
    ldx #0
@entry:
    lda f:entries + E_BOXES, x
    beq @next
    lda z:frame
    sec
    sbc f:entries + E_USED, x
    beq @next
    cmp z:best_age
    bcc @next
    sta z:best_age
    stx z:best
@next:
    txa
    clc
    adc #ENTRY_SIZE
    tax
    cpx #ENTRIES * ENTRY_SIZE
    bcc @entry
    lda z:best
    cmp #$FFFF
    beq @none
    sta z:entry
    jsr free_entry_blocks
    ldx z:entry
    lda #0
    sta f:entries + E_MAP, x
    pla
    sta z:entry
    clc
    rts
@none:
    pla
    sta z:entry
    sec
    rts

; The group's cells with this key into the block's four tiles, staged and
; queued for upload; those cells are marked drawn.
compose:
    lda z:stage_pos
    sta z:dst
    stz z:quad
    stz z:qmask
@quad:
    jsr quad_cell
    bcs @blank
    lda f:cells, x
    and #$E000
    cmp z:key
    bne @blank
    lda f:cells, x
    pha
    txa
    lsr
    tax
    sep #$20
    .a8
    lda #2                  ; drawn
    sta f:cells_used, x
    rep #$20
    .a16
    ldx z:quad
    lda f:quad_bits, x
    and #$00FF
    tsb z:qmask
    pla
    jsr stage_tile
    bra @next
@blank:
    jsr stage_blank
@next:
    lda z:dst
    clc
    adc #32
    sta z:dst
    inc z:quad
    lda z:quad
    cmp #4
    bcc @quad
    ; tiles 0-1 to the block's row of OBJ VRAM, 2-3 to the row below
    lda z:block
    jsr block_tile
    asl
    asl
    asl
    asl                     ; 16 words a tile
    clc
    adc #OBJ_TILES
    sta z:udst
    lda z:stage_pos
    clc
    adc #.loword(sprite_staging)
    sta z:usrc
    lda #^sprite_staging
    sta z:usrc + 2
    lda #64
    sta z:ulen
    lda #UP_VRAM
    jsr queue
    lda z:udst
    clc
    adc #16 * 16            ; 16 tiles further on
    sta z:udst
    lda z:usrc
    clc
    adc #64
    sta z:usrc
    lda #UP_VRAM
    jsr queue
    lda z:stage_pos
    clc
    adc #128
    sta z:stage_pos
    rts

quad_bits: .byte 1, 2, 4, 8

; Add a box for block with the key's attributes at group bx, by; carry set
; if the entry is full.
add_box:
    ldx z:entry
    lda f:entries + E_BOXES, x
    cmp #MAX_BOXES
    bcs @full
    inc
    sta f:entries + E_BOXES, x
    dec
    asl                     ; offset = entry + E_BOX + 8 * boxes
    asl
    asl
    clc
    adc z:entry
    clc
    adc #E_BOX
    tax
    lda z:key               ; priority 3 for high, 2 for low; palette
    and #$6000
    xba
    lsr
    lsr
    lsr
    lsr                     ; palette << 1
    sta z:t0
    lda z:key
    and #$8000
    beq :+
    lda #$30
    bra :++
:   lda #$20
:   ora z:t0
    xba
    ora z:block             ; block, attributes
    sta f:entries, x
    lda z:bx
    asl
    asl
    asl
    asl
    clc
    adc z:min_x
    sta f:entries + 2, x
    lda z:by
    asl
    asl
    asl
    asl
    clc
    adc z:min_y
    sta f:entries + 4, x
    lda z:qmask             ; all four: one 16x16
    cmp #$0F
    bne :+
    lda #0
:   sta f:entries + 6, x
    clc
    rts
@full:
    sec
    rts

; 32 zero bytes at dst.
stage_blank:
    ldx z:dst
    ldy #16
    lda #0
:   sta f:sprite_staging, x
    inx
    inx
    dey
    bne :-
    rts

; The SNES form of Genesis entry A's tile at dst, flipped as the entry
; says.
stage_tile:
    sta z:t1
    and #$07FF
    jsr planar
    ; src = its SNES form; rows in order or reversed, bytes as they are or
    ; mirrored
    lda z:t1
    and #$1000
    beq :+
    lda #14                 ; V flip: the source rows from the bottom
:   sta z:row               ; source row x2
    stz z:col               ; destination row x2
@row:
    ldy z:row
    lda [src], y            ; planes 0, 1
    jsr flip_h
    ldx z:col
    phx
    pha
    txa
    clc
    adc z:dst
    tax
    pla
    sta f:sprite_staging, x
    tya
    clc
    adc #16
    tay
    lda [src], y            ; planes 2, 3
    jsr flip_h
    sta f:sprite_staging + 16, x
    plx
    inx
    inx
    stx z:col
    lda z:t1
    and #$1000
    beq :+
    lda z:row
    dec
    dec
    bra :++
:   lda z:row
    inc
    inc
:   sta z:row
    lda z:col
    cmp #16
    bcc @row
    rts

; Both bytes of A mirrored if the entry (t1) is H-flipped.
flip_h:
    pha
    lda z:t1
    and #$0800
    bne :+
    pla
    rts
:   pla
    phx
    pha
    and #$00FF
    tax
    lda f:reverse_bits, x
    and #$00FF
    sta z:t0
    pla
    xba
    and #$00FF
    tax
    lda f:reverse_bits, x
    xba
    and #$FF00
    ora z:t0
    plx
    rts

reverse_bits:
.repeat 256, I
    .byte ((I & 1) << 7) | ((I & 2) << 5) | ((I & 4) << 3) | ((I & 8) << 1) | ((I & 16) >> 1) | ((I & 32) >> 3) | ((I & 64) >> 5) | ((I & 128) >> 7)
.endrep

; src = the SNES form of tile A, converted first if the tile changed.
planar:
    pha
    asl
    asl
    asl
    asl
    asl
    sta z:src
    lda #^PLANAR
    sta z:src + 2
    plx                     ; the tile
    lda f:planar_gen, x
    eor f:tile_gen, x
    and #$00FF
    bne :+
    rts
:   lda f:tile_gen, x
    sep #$20
    .a8
    sta f:planar_gen, x
    rep #$20
    .a16
    ; the Genesis tile (src's offset in the VRAM copy) into SNES planes
    ldy #0
@row:
    tyx                     ; (reuse: X = src byte)
    phy
    tya
    clc
    adc z:src
    tax
    lda f:VRAM_MIRROR, x
    and #$00FF
    asl
    asl
    phx
    tax
    lda f:planar_lut, x
    sta z:row01
    lda f:planar_lut + 2, x
    sta z:row23
    plx
    lda f:VRAM_MIRROR + 1, x
    and #$00FF
    asl
    asl
    phx
    tax
    lda f:planar_lut + $400, x
    tsb z:row01
    lda f:planar_lut + $402, x
    tsb z:row23
    plx
    lda f:VRAM_MIRROR + 2, x
    and #$00FF
    asl
    asl
    phx
    tax
    lda f:planar_lut + $800, x
    tsb z:row01
    lda f:planar_lut + $802, x
    tsb z:row23
    plx
    lda f:VRAM_MIRROR + 3, x
    and #$00FF
    asl
    asl
    tax
    lda f:planar_lut + $C00, x
    tsb z:row01
    lda f:planar_lut + $C02, x
    tsb z:row23
    pla                     ; row x4 -> x2
    pha
    lsr
    tay
    lda z:row01
    sta [src], y
    tya
    clc
    adc #16
    tay
    lda z:row23
    sta [src], y
    pla
    clc
    adc #4
    tay
    cpy #32
    bcs :+
    jmp @row
:   rts

.segment "BWRAM"
oam:            .res 512
oam_high:       .res 32
planar_gen:     .res 2048       ; tile_gen when the tile was converted
block_owner:    .res BLOCKS
entries:        .res ENTRIES * ENTRY_SIZE
cells:          .res GRID * GRID * 2
cells_used:     .res GRID * GRID
sprite_staging: .res BLOCK_BUDGET * 128
