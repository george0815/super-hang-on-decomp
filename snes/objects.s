; Game objects (src/game/objects.c).
;
; 29 records of $40 bytes at OBJECTS, in the Genesis RAM mirror (so
; big-endian; see inc/macros.inc). Each frame objects_update runs every
; object's handler, then turns the layer lists into the Genesis sprite
; table (kept for comparison with the original) and the SNES's OAM
; (sprites.s).

.p816
.smart
.include "inc/mem.inc"
.include "inc/dp.inc"
.include "inc/macros.inc"

.export objects_update
.import sprites_begin, sprite_object, sprites_end

OBJECTS         = $C900
OBJ_SIZE        = $40
OBJ_COUNT       = 29

OBJ_HANDLER     = $00   ; word: handler index x4 (0 = free)
OBJ_TILE_BASE   = $02
OBJ_X           = $04   ; long, 16.16
OBJ_Y           = $08
OBJ_Z           = $0C   ; long; its high word doubles as draw layer
OBJ_SCREEN_X    = $10
OBJ_SCREEN_Y    = $14
OBJ_VX          = $18
OBJ_VY          = $1C
OBJ_VZ          = $20
OBJ_SPRITE_MAP  = $24
OBJ_SPRITE_SET  = $2D   ; byte

LAYER_LISTS     = $D400 ; 97 lists of $20 bytes: count (bytes), object words
OBJ_LAYER_LISTS = $E000 ; (the list of layer 0; deeper layers below it)
LAYER_LIST_MAX  = $1E
SPRITE_TABLE    = $D100
MAX_SPRITES     = 80

ROM_SPRITE_SETS        = $000204
ROM_DEPTH_SPRITES_NEAR = $006EC8

; direct page: $D8-$FF
o_obj       = $D8       ; the object
o_t0        = $DA
o_t1        = $DC
o_list      = $DE       ; build_sprite_table: the layer list,
o_layer     = $E0       ; its number,
o_entry     = $E2       ; the entry in it,
o_sprite    = $E4       ; the next sprite table entry,
o_left      = $E6       ; sprites left,
o_map       = $E8       ; the sprite map (24 bits),
o_pieces    = $EC
o_x0        = $EE
o_y0        = $F0
o_tile0     = $F2

.segment "CODE"
.a16
.i16

; The big-endian long at X + dest += the one at X + source.
.macro add_long dest, source
    lda a:dest + 2, x
    xba
    sta z:o_t0
    lda a:source + 2, x
    xba
    clc
    adc z:o_t0
    xba
    sta a:dest + 2, x
    lda a:dest, x
    xba
    sta z:o_t0
    lda a:source, x
    xba
    adc z:o_t0
    xba
    sta a:dest, x
.endmacro

; ---------------------------------------------------------------- loop

; Run every object, then build the sprite table ($716E).
objects_update:
    lda #$1CC8              ; $C81C, big-endian
    sta a:$C81A
    lda #$FFFF
    sta a:$C81C
    sta a:$C81E
    sta a:$C820
    sta a:$C822
    sta a:$C824
    lda #OBJECTS
    sta z:o_obj
@object:
    ldx z:o_obj
    ldw OBJ_HANDLER, x
    and #$FFFE
    beq @next
    lsr                     ; index / 4 - 1, x2
    dec
    dec
    cmp #handlers_end - handlers
    bcs @next               ; (a handler not ported yet)
    tax
    jsr (handlers, x)
@next:
    lda z:o_obj
    clc
    adc #OBJ_SIZE
    sta z:o_obj
    cmp #OBJECTS + OBJ_COUNT * OBJ_SIZE
    bcc @object
    jsr build_sprite_table
    jmp link_sprites

; Handlers by index ($710A on); the rest are not ported yet.
handlers:
    .word obj_debris
handlers_end:

; ------------------------------------------------------------- helpers

; X += VX; screen x = X + $120 ($70BA).
obj_move_x:
    ldx z:o_obj
    add_long OBJ_X, OBJ_VX
    ldw OBJ_X, x
    clc
    adc #$120
    stw OBJ_SCREEN_X, x
    rts

; Y += VY; screen y = Y + $F0 ($70D0).
obj_move_y:
    ldx z:o_obj
    add_long OBJ_Y, OBJ_VY
    ldw OBJ_Y, x
    clc
    adc #$F0
    stw OBJ_SCREEN_Y, x
    rts

; Z += VZ ($70E6).
obj_move_z:
    ldx z:o_obj
    add_long OBJ_Z, OBJ_VZ
    rts

; Pick the sprite map for the object's depth ($7060): the sprite set
; selects a 20-byte row of map addresses; the depth table at A (a ROM
; address) gives the offset in it.
obj_pick_sprite:
    sta z:o_t1
    ldx z:o_obj
    ldw OBJ_Z, x
    tay                     ; (flags from the whole word)
    beq @none
    cmp #$C1                ; above $C0 (signed): none
    bcc :+
    cmp #$8000
    bcc @none
:   clc
    adc z:o_t1
    tax
    lda f:MDROM, x          ; the offset (signed byte)
    and #$00FF
    cmp #$80
    bcc :+
    ora #$FF00
:   sta z:o_t1
    ldx z:o_obj
    lda a:OBJ_SPRITE_SET, x
    and #$00FF
    asl
    asl
    sta z:o_t0              ; set * 4
    asl
    asl
    clc
    adc z:o_t0              ; set * 20
    clc
    adc #ROM_SPRITE_SETS
    clc
    adc z:o_t1
    tax
    lda f:MDROM, x          ; (big-endian on both sides)
    ldx z:o_obj
    sta a:OBJ_SPRITE_MAP, x
@none:
    rts

; Queue the object on its layer's list ($709A): a count (in bytes)
; followed by the low words of the objects. Full lists drop it.
obj_add_to_layer:
    ldx z:o_obj
    ldw OBJ_Z, x            ; the layer: Z's high word, even
    and #$FFFE
    asl
    asl
    asl
    asl
    eor #$FFFF
    sec
    adc #OBJ_LAYER_LISTS    ; OBJ_LAYER_LISTS - layer * 16
    tax
    ldw 0, x
    cmp #LAYER_LIST_MAX
    bcc :+
    cmp #$8000
    bcc @full
:   pha
    clc
    adc #2
    stw 0, x
    pla
    sta z:o_t0
    txa
    clc
    adc z:o_t0
    tax
    lda z:o_obj
    stw 2, x
@full:
    rts

; ------------------------------------------------------------ handlers

; Thrown/falling piece ($710A): moves on all three axes with gravity on X,
; stops at depth $B8, and at depth $1C signals the title screen.
obj_debris:
    lda #ROM_DEPTH_SPRITES_NEAR
    jsr obj_pick_sprite
    jsr obj_add_to_layer
    ldx z:o_obj
    ldw OBJ_Z, x
    cmp #$1C
    bne @not_1c
    jsr obj_move_z
    stz a:$C712
    lda #$18
    stw $C714
    sep #$20
    .a8
    lda a:$C72C
    clc
    adc #4
    sta a:$C72C
    rep #$20
    .a16
    rts
@not_1c:
    cmp #$B8
    bne @move
    jsr obj_move_z
    ldx z:o_obj
    stz a:OBJ_VX, x
    stz a:OBJ_VX + 2, x
    stz a:OBJ_VY, x
    stz a:OBJ_VY + 2, x
    stz a:OBJ_VZ, x
    stz a:OBJ_VZ + 2, x
    rts
@move:
    jsr obj_move_x
    jsr obj_move_y
    jsr obj_move_z
    ldx z:o_obj
    ldw OBJ_Z, x
    cmp #$BC
    beq :+
    lda a:OBJ_VX + 2, x     ; VX += $FFFF5000
    xba
    clc
    adc #$5000
    xba
    sta a:OBJ_VX + 2, x
    lda a:OBJ_VX, x
    xba
    adc #$FFFF
    xba
    sta a:OBJ_VX, x
:   rts

; -------------------------------------------------------- sprite table

; Turn the layer lists into the Genesis sprite table ($71A8), and each
; object's frame into SNES OBJs (sprites.s). A sprite map is a piece
; count (minus one) and 6-byte pieces: y offset (byte, from -256), size,
; tile (word), x offset (word). Pieces off the left/right edges are
; dropped; at most 80 sprites.
build_sprite_table:
    jsr sprites_begin
    lda #SPRITE_TABLE
    sta z:o_sprite
    lda #MAX_SPRITES
    sta z:o_left
    lda #LAYER_LISTS
    sta z:o_list
@list:
    ldx z:o_list
    lda a:0, x
    beq @next_list
    xba
    lsr
    sta z:o_layer           ; (entries)
    stz a:0, x
    stz z:o_entry
@entry:
    lda z:o_entry
    asl
    clc
    adc z:o_list
    tax
    ldw 2, x
    sta z:o_obj
    tax
    lda a:OBJ_SPRITE_MAP, x
    beq @next_entry
    jsr object_pieces
    ldx z:o_obj
    jsr sprite_object
    lda z:o_left
    beq @done
@next_entry:
    inc z:o_entry
    lda z:o_entry
    cmp z:o_layer
    bcc @entry
@next_list:
    lda z:o_list
    clc
    adc #$20
    sta z:o_list
    cmp #LAYER_LISTS + 97 * $20
    bcc @list
@done:
    jmp sprites_end

; The object's pieces into the Genesis sprite table.
object_pieces:
    ldx z:o_obj
    ldw OBJ_SPRITE_MAP, x
    jsr map_address
    ldx z:o_obj
    ldw OBJ_SCREEN_Y, x
    sta z:o_y0
    ldw OBJ_SCREEN_X, x
    sta z:o_x0
    ldw OBJ_TILE_BASE, x
    sta z:o_tile0
    lda [o_map]
    xba
    inc
    sta z:o_pieces
    ldy #2
@piece:
    ldx z:o_sprite
    lda [o_map], y          ; y offset (from -256) and size
    and #$00FF
    ora #$FF00
    clc
    adc z:o_y0
    stw 0, x
    iny
    lda [o_map], y
    sep #$20
    .a8
    sta a:2, x
    rep #$20
    .a16
    iny
    lda [o_map], y          ; tile
    xba
    clc
    adc z:o_tile0
    stw 4, x
    iny
    iny
    lda [o_map], y          ; x
    xba
    clc
    adc z:o_x0
    stw 6, x
    iny
    iny
    cmp #$1BF               ; off the left or right edge: dropped
    bpl @skip
    cmp #$5F
    bmi @skip
    txa
    clc
    adc #8
    sta z:o_sprite
    dec z:o_left
    beq @full
@skip:
    dec z:o_pieces
    bne @piece
@full:
    rts

; o_map = where the map at A (a sign-extended word) is: RAM ($FFxxxx,
; the mirror) or ROM.
map_address:
    sta z:o_map
    cmp #$8000
    lda #^MDROM
    bcc :+
    lda #^MIRROR
:   sta z:o_map + 2
    rts

; Link the sprites into one chain (hidden placeholder if none).
link_sprites:
    lda #MAX_SPRITES
    sec
    sbc z:o_left            ; the count
    bne :+
    lda #$6801              ; $0168, big-endian
    sta a:SPRITE_TABLE
    sep #$20
    .a8
    stz a:SPRITE_TABLE + 3
    rep #$20
    .a16
    rts
:   sta z:o_t0
    ldx #SPRITE_TABLE
    lda #1
@link:
    cmp z:o_t0
    bcs @last
    sep #$20
    .a8
    sta a:3, x
    rep #$20
    .a16
    inc
    pha
    txa
    clc
    adc #8
    tax
    pla
    bra @link
@last:
    sep #$20
    .a8
    stz a:3, x
    rep #$20
    .a16
    rts
