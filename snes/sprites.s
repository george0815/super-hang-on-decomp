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
; Maps that are not on one grid, and frames over 256 pixels across or
; 144 down, are not drawn yet.

.p816
.smart
.include "inc/hw.inc"
.include "inc/mem.inc"
.include "inc/dp.inc"
.include "inc/macros.inc"
.include "gen/assets.inc"

.export sprites_reset, sprites_begin, sprite_object, sprites_end, sprites_hide
.export objects__build_sprite_table
.import upload, upload_vram, upload_fits, tile_gen

VRAM_MIRROR = $420000
PLANAR      = $430000   ; the SNES form of each Genesis tile, 32 bytes
OBJ_TILES   = $6000     ; OBJ VRAM (word address), 512 tiles
BLOCKS      = 128       ; 16x16 blocks of it
.ifdef TRACE
ENTRIES     = 24        ; cached frames (trace builds: room for the ring)
.else
ENTRIES     = 27        ; cached frames
.endif
MAX_BOXES   = 40
MAX_PHASES  = 32        ; groups of a frame's pieces
ENTRY_SIZE  = 14 + MAX_BOXES * 8
GRID        = 32        ; cells across (256 pixels): a row of the grid
GRID_H      = 18        ; cells down (144 pixels)
BLOCK_BUDGET = 24       ; new blocks per frame
CROP        = 32

; an entry
E_MAP       = 0         ; 0: free
E_TILE0     = 2
E_SUM       = 4         ; tile_gen sum of its tiles when built
E_USED      = 6         ; the frame it was last drawn in
E_BOXES     = 8
E_RESUME    = 10        ; unfinished: the group + 1, row and column to go on
                        ;  from (bytes; 0: finished)
E_BOX       = 14        ; boxes: block, attributes, dx (word), dy (word),
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
sby         = $18       ; build_from: the group's first row of boxes,
sbx         = $1A       ;  and that row's first column
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
sptr        = $6C       ; 24 bits: stage_tile's destination
vptr        = $70       ; 24 bits: planar's source, in the VRAM copy
bnext       = $74       ; new_block: where the search for a free one starts
phase       = $76       ; build_entry: the group's (x & 7) | (y & 7) << 3,
nphase      = $78       ;  the groups,
iphase      = $7A       ;  and the one being built
gfresh      = $7C       ; cut_boxes: nothing drawn yet from this 2x2 group
stage_list  = $7E       ; up_count when blocks were last staged
tw_attr     = oam_x     ; find_twin's (the drawing's, idle while building):
tw_bx       = oam_y     ;  the attributes, the group of cells,
tw_by       = col
tw_box      = row       ;  the box being compared

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
    stz z:bnext
    stz z:stage_pos
    lda f:SA1_DP + up_count
    sta z:stage_list
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
    ; the staged blocks' uploads: until the list they are in has been
    ; handed over and carried out, new blocks go after them
    lda f:SA1_DP + up_count
    cmp z:stage_list
    beq :+
    lda f:UPLOAD_READY
    and #$00FF
    bne :+
    stz z:stage_pos
:   lda z:stage_pos         ; budget = the staging blocks left
    asl
    xba
    and #$00FF              ; (/ 128)
    eor #$FFFF
    sec
    adc #BLOCK_BUDGET
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

; No OBJs (the sprite table cleared).
sprites_hide:
    jsr sprites_begin
    ; fall through

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
; build_sprite_table() (src/game/objects.c, left out of the compiled C):
; the 97 layer lists into the Genesis sprite table ($FFD100), each object
; also drawn as SNES OBJs (sprite_object). Returns the sprite count (A, and
; CHI = 0). The RAM mirror is the data bank.
SPRITE_TABLE = $D100
LAYER_LISTS  = $D400
LAYER_END    = LAYER_LISTS + 97 * $20
MAX_SPRITES  = 80
TABLE_DP     = SPRITES_DP + $80     ; its own direct page (past sprites.s's)
bt_sprite   = $00       ; the next sprite's address
bt_left     = $02       ; sprites still allowed
bt_list     = $04
bt_entries  = $06       ; objects left in the list
bt_entry    = $08
bt_obj      = $0A
bt_map      = $0C       ; 24 bits
bt_pieces   = $10       ; pieces left after this one
bt_y0       = $12
bt_x0       = $14
bt_tile0    = $16

objects__build_sprite_table:
    phd
    pea TABLE_DP
    pld
    lda #SPRITE_TABLE
    sta z:bt_sprite
    lda #MAX_SPRITES
    sta z:bt_left
    lda #LAYER_LISTS
    sta z:bt_list
@list:
    ldx z:bt_list
    lda a:0, x
    bne :+
    jmp @next_list
:   stz a:0, x              ; (emptied as it is read)
    xba
    cmp #$8000              ; objects: count >> 1 (signed)
    ror
    beq :+
    bpl :++
:   jmp @next_list
:   sta z:bt_entries
    inx
    inx
    stx z:bt_entry
@entry:
    ldx z:bt_entry
    ldw 0, x
    sta z:bt_obj
    tax
    lda a:OBJ_SPRITE_MAP, x
    bne :+
    jmp @next_entry
:   jsr sprite_object
    ldx z:bt_obj
    ldw OBJ_SPRITE_MAP, x
    sta z:bt_map
    cmp #$8000
    lda #^MDROM
    bcc :+
    lda #^MIRROR
:   sta z:bt_map + 2
    lda [bt_map]
    xba
    sta z:bt_pieces
    ldw OBJ_SCREEN_Y, x
    sta z:bt_y0
    ldw OBJ_SCREEN_X, x
    sta z:bt_x0
    ldw OBJ_TILE_BASE, x
    sta z:bt_tile0
    ldy #2
@piece:
    ldx z:bt_sprite
    lda [bt_map], y         ; y offset (byte, from -256), size
    pha
    and #$00FF
    ora #$FF00
    clc
    adc z:bt_y0
    xba
    sta a:0, x
    pla
    xba
    sep #$20
    .a8
    sta a:2, x
    rep #$20
    .a16
    iny
    iny
    lda [bt_map], y         ; tile
    xba
    clc
    adc z:bt_tile0
    xba
    sta a:4, x
    iny
    iny
    lda [bt_map], y         ; x offset
    xba
    clc
    adc z:bt_x0
    xba
    sta a:6, x
    xba
    iny
    iny
    sec                     ; kept if $5F <= x < $1BF (signed)
    sbc #$005F
    cmp #$0160
    bcs @dropped
    txa
    clc
    adc #8
    sta z:bt_sprite
    dec z:bt_left
    beq @full
@dropped:
    lda z:bt_pieces
    beq @next_entry
    dec z:bt_pieces
    bra @piece
@next_entry:
    lda z:bt_entry
    inc
    inc
    sta z:bt_entry
    dec z:bt_entries
    beq @next_list
    jmp @entry
@next_list:
    lda z:bt_list
    clc
    adc #$20
    sta z:bt_list
    cmp #LAYER_END
    bcs :+
    jmp @list
:   lda #MAX_SPRITES
    sec
    sbc z:bt_left
    bra @done
@full:
    lda #MAX_SPRITES
@done:
    pld
    ldx #0
    stx z:CHI
    rtl

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
    bcc @found
    lda z:entry
    cmp #$FFFF
    beq @done               ; no entry to spare
    jsr build_entry
    bra @draw
@found:
    ldx z:entry             ; unfinished: go on with it
    lda f:entries + E_RESUME, x
    and #$00FF
    beq @draw
    jsr resume_entry
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
    lda f:entries, x        ; the block: one user less
    and #$00FF
    tax
    sep #$20
    .a8
    lda f:block_owner, x
    dec
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

; Build the frame into entry. Its pieces are split into groups by their
; place modulo 8 (most frames are one group), in the order of each group's
; first piece; each group fills the cell grid, and its 2x2 groups of
; cells become boxes. Earlier groups' boxes are drawn on top. When this
; frame's blocks or uploads run short, the build stops between 2x2
; groups and goes on in the next frame (E_RESUME); the boxes built so far
; are drawn meanwhile.
build_entry:
    ldx z:entry
    lda z:sum
    sta f:entries + E_SUM, x
    lda #0
    sta f:entries + E_BOXES, x
    stz z:iphase
    stz z:sby
    stz z:sbx
    bra build_from

resume_entry:
    ldx z:entry
    lda f:entries + E_RESUME, x
    and #$00FF
    dec
    sta z:iphase
    lda f:entries + E_RESUME + 1, x
    and #$00FF
    sta z:sby
    lda f:entries + E_RESUME + 2, x
    and #$00FF
    sta z:sbx

build_from:
    ldx z:entry
    lda #0
    sta f:entries + E_RESUME, x
    sta f:entries + E_RESUME + 2, x
    jsr find_phases
@group:
    ldx z:iphase
    lda f:phases, x
    and #$00FF
    sta z:phase
    jsr measure
    bcs :+                  ; too big
    jsr fill_grid
    jsr cut_boxes
    bcs @done               ; stopped
:   stz z:sby
    stz z:sbx
    inc z:iphase
    lda z:iphase
    cmp z:nphase
    bcc @group
@done:
    rts

; The groups' phases, in order of first appearance, into phases (nphase).
find_phases:
    lda [map]
    xba
    inc
    sta z:pieces
    stz z:nphase
    ldy #2
@piece:
    jsr piece
    jsr piece_phase
    sep #$20
    .a8
    ldx #0
:   cpx z:nphase
    beq @new
    cmp f:phases, x
    beq @next
    inx
    bra :-
@new:
    cpx #MAX_PHASES
    beq @next
    sta f:phases, x
    inc z:nphase
@next:
    rep #$20
    .a16
    dec z:pieces
    bne @piece
    rts

; A = the piece's phase: (px & 7) | (pyy & 7) << 3.
piece_phase:
    lda z:pyy
    and #7
    asl
    asl
    asl
    sta z:t0
    lda z:px
    and #7
    ora z:t0
    rts

; The group's extent (the pieces in phase); carry set if it spans more
; than GRID cells across or GRID_H down.
measure:
    lda [map]
    xba
    inc
    sta z:pieces
    ldy #2
@first:
    jsr piece
    jsr piece_phase
    cmp z:phase
    beq :+
    dec z:pieces            ; (the group has a piece: it ends the search)
    bra @first
:   lda z:px                ; the first piece starts the extent
    sta z:min_x
    sta z:max_x
    lda z:pyy
    sta z:min_y
    sta z:max_y
@piece:
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
:
@next:
    dec z:pieces
    beq @done
    jsr piece
    jsr piece_phase
    cmp z:phase
    bne @next
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
    bcs @big
    lda z:max_y
    sec
    sbc z:min_y
    lsr
    lsr
    lsr
    sta z:gh
    cmp #GRID_H + 1
    bcs @big
    clc
    rts
@big:
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
    lda z:gh                ; the rows in use cleared
    xba
    lsr
    lsr
    lsr                     ; x GRID
    sta z:t0
    ldx #0
    lda #0
:   sta f:cells_used, x
    inx
    inx
    cpx z:t0
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
    jsr piece_phase
    cmp z:phase
    beq :+
    jmp @next_piece         ; another group's
:   stz z:col
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
    bcs :+
    jmp @row
:   inc z:col
    lda z:col
    cmp z:pw
    bcs @next_piece
    jmp @col
@next_piece:
    dec z:pieces
    bmi :+
    jmp @piece
:   rts

; Each 2x2 group of cells becomes an OBJ per palette and priority in it,
; from row sby, column sbx on; carry set if the build stopped: for this
; frame (E_RESUME set), or for good (out of boxes; out of blocks: built
; again next frame).
cut_boxes:
    lda z:sby
    sta z:by
    lda z:sbx
    sta z:bx
    bra @group
@by:
    stz z:bx
@group:
    lda #1
    sta z:gfresh
@bx:
    ; the first used cell of the group not drawn yet gives the key
    stz z:quad
@find:
    jsr quad_cell
    bcs @next_quad
    lda f:cells, x
    and #$E000              ; priority and palette
    sta z:key
    jsr find_twin           ; the same cells as a box before: its block
    bcs :+
    jsr add_box
    bcs @boxes_full
    bra @bx
:   lda z:gfresh            ; the group's first new block: room for four?
    beq :+
    stz z:gfresh
    jsr room4
    bcs @suspend
:   jsr new_block
    bcs @full
    jsr compose
    jsr add_box
    bcs @boxes_full
    bra @bx                 ; (again: cells with another key may remain)
@next_quad:
    inc z:quad
    lda z:quad
    cmp #4
    bcc @find
    lda z:bx
    inc
    sta z:bx
    asl
    cmp z:gw
    bcs :+
    jmp @group
:   lda z:by
    inc
    sta z:by
    asl
    cmp z:gh
    bcs :+
    jmp @by
:   clc
    rts
@suspend:
    ldx z:entry             ; go on from this group next frame
    sep #$20
    .a8
    lda z:iphase
    inc
    sta f:entries + E_RESUME, x
    lda z:by
    sta f:entries + E_RESUME + 1, x
    lda z:bx
    sta f:entries + E_RESUME + 2, x
    rep #$20
    .a16
    sec
    rts
@full:
    ldx z:entry             ; out of blocks: try again next frame
    lda #$FFFF
    sta f:entries + E_SUM, x
@boxes_full:
    sec
    rts

; The attribute byte of a box with key: priority 3 for high, 2 for low;
; palette << 1.
key_attr:
    lda z:key
    and #$6000
    xba
    lsr
    lsr
    lsr
    lsr
    sta z:t0
    lda z:key
    and #$8000
    beq :+
    lda #$30
    bra :++
:   lda #$20
:   ora z:t0
    rts

; A = the entry of cell quad of group bx, by if it is used with key, else
; 0 (zero flag set); X = its index if so.
cell_of:
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
    asl                     ; x GRID
    clc
    adc z:t0
    tax
    lda f:cells_used, x
    and #$00FF
    beq @no
    txa
    asl
    tax
    lda f:cells, x
    and #$E000
    cmp z:key
    bne @no
    lda f:cells, x
    ora #0
    rts
@no:
    lda #0
    rts

; If a box of the entry from this group (iphase) has the same cells for key
; as group bx, by, share its block: carry clear, block set (one user
; more), the cells marked drawn and qmask set as compose would.
find_twin:
    ldy #0                  ; this group's four cells (Y: quad x2)
    stz z:qmask
:   tya
    lsr
    sta z:quad
    jsr cell_of
    tyx
    sta f:twin_cells, x
    cmp #0
    beq :+
    ldx z:quad
    lda f:quad_bits, x
    and #$00FF
    tsb z:qmask
:   iny
    iny
    cpy #8
    bcc :--
    jsr key_attr
    sta z:tw_attr
    lda z:bx
    sta z:tw_bx
    lda z:by
    sta z:tw_by
    ldx z:entry
    lda f:entries + E_BOXES, x
    bne :+
    jmp @none
:   sta z:nbox
    txa
    clc
    adc #E_BOX
    sta z:tw_box
@box:
    ldx z:tw_box
    lda f:entries + 7, x    ; this group's, with these attributes?
    and #$00FF
    cmp z:iphase
    bne @next
    lda f:entries + 1, x
    and #$00FF
    cmp z:tw_attr
    bne @next
    lda f:entries + 2, x    ; its group of cells
    sec
    sbc z:min_x
    lsr
    lsr
    lsr
    lsr
    sta z:bx
    lda f:entries + 4, x
    sec
    sbc z:min_y
    lsr
    lsr
    lsr
    lsr
    sta z:by
    ldy #0
:   tya
    lsr
    sta z:quad
    jsr cell_of
    tyx
    cmp f:twin_cells, x
    bne @next
    iny
    iny
    cpy #8
    bcc :-
    ; found: share its block, the cells drawn
    lda z:tw_bx
    sta z:bx
    lda z:tw_by
    sta z:by
    ldx z:tw_box
    lda f:entries, x
    and #$00FF
    sta z:block
    tax
    sep #$20
    .a8
    lda f:block_owner, x
    inc
    sta f:block_owner, x
    rep #$20
    .a16
    stz z:quad
:   jsr cell_of
    beq :+
    txa
    lsr
    tax
    sep #$20
    .a8
    lda #2                  ; drawn
    sta f:cells_used, x
    rep #$20
    .a16
:   inc z:quad
    lda z:quad
    cmp #4
    bcc :--
    clc
    rts
@next:
    lda z:tw_box
    clc
    adc #8
    sta z:tw_box
    dec z:nbox
    beq @none
    jmp @box
@none:
    lda z:tw_bx
    sta z:bx
    lda z:tw_by
    sta z:by
    sec
    rts

; Carry clear if this frame can still take 4 blocks (budget and uploads).
room4:
    lda z:budget
    cmp #4
    bcc @no
    lda #4 * (128 + 2 * UPLOAD_ITEM) - UPLOAD_ITEM
    jsr upload_fits_main
    bcc @no
    clc
    rts
@no:
    sec
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
    asl                     ; x GRID
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
    lda #128 + UPLOAD_ITEM  ; its tiles must reach VRAM this frame (two
    jsr upload_fits_main    ; entries)
    bcc @none
@search:
    phy                     ; from past the last one taken, round once
    ldx z:bnext
    ldy #BLOCKS
:   lda f:block_owner, x
    and #$00FF
    beq @take
    inx
    cpx #BLOCKS
    bcc :+
    ldx #0
:   dey
    bne :--
    ply
    jsr evict               ; none free: free the frame drawn longest ago
    bcc @search
@none:
    sec
    rts
@take:
    ply
    stx z:block
    txa
    inc
    and #BLOCKS - 1
    sta z:bnext
    sep #$20
    .a8
    lda #1
    sta f:block_owner, x
    rep #$20
    .a16
    dec z:budget
    clc
    rts

; upload_fits, on the main direct page.
upload_fits_main:
    phd
    pea SA1_DP
    pld
    jsr upload_fits
    pld
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
    lda f:SA1_DP + up_count ; (the list being built carries them)
    sta z:stage_list
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
    jsr key_attr
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
    lda z:qmask             ; one quad: an 8x8; more: one 16x16 (the
    cmp #1                  ; others blank), which saves OBJs
    beq :+
    cmp #2
    beq :+
    cmp #4
    beq :+
    cmp #8
    beq :+
    lda #0
:   sta z:t0
    lda z:iphase            ; (and the group)
    xba
    ora z:t0
    sta f:entries + 6, x
    clc
    rts
@full:
    ldx z:block             ; (the block goes back)
    sep #$20
    .a8
    lda f:block_owner, x
    dec
    sta f:block_owner, x
    rep #$20
    .a16
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
; says: rows 0-7 of planes 0 and 1, then of planes 2 and 3, a word each;
; V flip reverses the rows, H flip mirrors each byte.
stage_tile:
    sta z:t1
    and #$07FF
    jsr planar
    lda z:dst
    clc
    adc #.loword(sprite_staging)
    sta z:sptr
    lda #^sprite_staging
    adc #0
    sta z:sptr + 2
    ldx z:dst
    lda z:t1
    and #$1800
    beq @plain
    cmp #$1000
    bne :+
    jmp @v
:   jmp @h
@plain:
.repeat 16, i
    ldy #2 * i
    lda [src], y
    sta f:sprite_staging + 2 * i, x
.endrep
    rts
@v:
.repeat 16, i
    ldy #(i & 8) * 2 + 14 - (i & 7) * 2
    lda [src], y
    sta f:sprite_staging + 2 * i, x
.endrep
    rts
@h:                         ; bytes through reverse_bits, rows reversed
    sep #$20                ; too with V
    .a8
    lda #0
    xba                     ; (B = 0: tax takes the byte alone)
    lda z:t1 + 1
    and #$10
    beq :+
    jmp @hv
:
.repeat 32, i
    ldy #i
    lda [src], y
    tax
    lda f:reverse_bits, x
    sta [sptr], y
.endrep
    rep #$20
    .a16
    rts
@hv:
    .a8
.repeat 32, i
    ldy #(i & 16) + 14 - (i & 14) + (i & 1)
    lda [src], y
    tax
    lda f:reverse_bits, x
    ldy #i
    sta [sptr], y
.endrep
    rep #$20
    .a16
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
    sta z:vptr
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
    lda #^VRAM_MIRROR
    sta z:vptr + 2
    ; the Genesis tile (src's offset in the VRAM copy) into SNES planes:
    ; each of a row's 4 bytes (2 pixels) through its table
.repeat 8, r
.repeat 4, b
    ldy #4 * r + b
    lda [vptr], y
    and #$00FF
    asl
    asl
    tax
    lda f:planar_lut + $400 * b, x
.if b = 0
    sta z:row01
.else
    tsb z:row01
.endif
    lda f:planar_lut + $400 * b + 2, x
.if b = 0
    sta z:row23
.else
    tsb z:row23
.endif
.endrep
    ldy #2 * r
    lda z:row01
    sta [src], y
    ldy #2 * r + 16
    lda z:row23
    sta [src], y
.endrep
    rts

.segment "BWRAM"
oam:            .res 512
oam_high:       .res 32
planar_gen:     .res 2048       ; tile_gen when the tile was converted
block_owner:    .res BLOCKS
entries:        .res ENTRIES * ENTRY_SIZE
cells:          .res GRID * GRID_H * 2
cells_used:     .res GRID * GRID_H
phases:         .res MAX_PHASES
twin_cells:     .res 8          ; find_twin: the group's four cells
sprite_staging: .res BLOCK_BUDGET * 128
