; The race's per-line road tables by hand: the hot spots of the compiled
; race (src/game/race.c scroll_tables and road_rows), translated line for
; line so the RAM they leave stays the original's. They replace the
; compiled ones (Makefile SNES_C_SKIP) under the same names.
;
; Conventions as compiled code: called with jsl, A and X/Y 16-bit, data
; bank $40 (the Genesis RAM, big-endian), direct page at SA1_DP.

.p816
.smart
.include "inc/mem.inc"
.include "inc/dp.inc"
.include "inc/macros.inc"

.export race__scroll_tables, race__road_rows

ROAD_CENTRE_LINES = $0D00
ROAD_HEADING      = $061A
ROAD_Y_LINES      = $0F00
ROAD_ROW_LINES    = $1300
ROAD_ROWS_BLANK   = $0754
ROAD_HIDDEN_FROM  = $0758
BG_SCROLL         = $0570   ; word far, word near
BG_HEADING        = $0578   ; 16.16
SCROLL_BUFFER_SEL = $0580
SCROLL_FLIP       = $06C4
VSCROLL_LINES     = $1500
HSCROLL_LINES     = $1880
ROM_STRIPES       = $00FE6E ; 32 patterns of 96 bytes

; direct page: $20-$3F
delta       = $20       ; 32 bits
bg          = $24
d4          = $26
d5          = $28
blank_rows  = $2A
stripes     = $2C       ; 24 bits
half        = $30
hs          = $32
vs          = $34
row         = $36
top         = $38
line        = $3A
v           = $3C
count       = $3E

.segment "CODE2"
.a16
.i16

; ---------------------------------------------------------- scroll tables

; The scroll tables for the next frame ($EFA8 on): the background
; follows the road's heading; each road row gets its line's vertical
; offset (with the stripes' alternate rows) and horizontal centre; the
; rows above the road the background; the buffers are flipped at the next
; VBlank.
race__scroll_tables:
    ; delta = ((uint16)(heading << 5) << 16 - BG_HEADING << 6) >> 7 (signed)
    ldw BG_HEADING + 2
    sta CT0
    ldw BG_HEADING
    sta CT1
    ldx #6
:   asl CT0
    rol CT1
    dex
    bne :-
    ldw ROAD_HEADING
    asl
    asl
    asl
    asl
    asl
    sta CT2
    lda #0
    sec
    sbc CT0
    sta delta
    lda CT2
    sbc CT1                 ; (the borrow from the low word)
    sta delta + 2
    ldx #7
:   lda delta + 2
    cmp #$8000
    ror delta + 2
    ror delta
    dex
    bne :-
    ; BG_HEADING += delta
    ldw BG_HEADING + 2
    clc
    adc delta
    stw BG_HEADING + 2
    ldw BG_HEADING
    adc delta + 2
    stw BG_HEADING
    ; the background's scroll: far, and three quarters of it near
    sec
    sbc #8
    sta CT0
    ldw $C74C
    sec
    sbc CT0
    and #$07FF
    sta bg
    stw BG_SCROLL
    stw $C74A
    asl
    clc
    adc bg
    cmp #$8000
    ror
    cmp #$8000
    ror
    stw BG_SCROLL + 2
    ; the stripes pattern, the buffers being filled
    ldw $0662
    and #$001F
    asl
    asl
    asl
    asl
    asl
    sta CT0
    asl
    clc
    adc CT0                 ; x 96
    clc
    adc #.loword(MDROM + ROM_STRIPES)
    sta stripes
    lda #^(MDROM + ROM_STRIPES)
    adc #0
    sta stripes + 2
    ldw SCROLL_BUFFER_SEL
    eor #$0700
    sta half
    clc
    adc #HSCROLL_LINES
    sta hs
    lda half
    clc
    adc #VSCROLL_LINES
    sta vs
    stz d4
    stz d5
    stz row
@row:
    lda row
    asl
    tax
    ldw ROAD_ROW_LINES, x
    tay
    bne @road
    inc d4                  ; above the road
    bra @next
@road:
    lsr
    sta line
    lda #$0140              ; v = $140 - d5 + line, $C0 less on stripes
    sec
    sbc d5
    clc
    adc line
    sta v
    lda line
    lsr
    tay
    sep #$20
    .a8
    lda [stripes], y
    rep #$20
    .a16
    beq :+
    lda v
    sec
    sbc #$00C0
    sta v
:   ldx vs                  ; (-row, v)
    lda row
    eor #$FFFF
    inc
    stw 0, x
    lda v
    stw 2, x
    lda line                ; (delta's high word, centre >> 6 - 100)
    asl
    tay
    ldw ROAD_CENTRE_LINES, y
    cmp #$8000
    ror
    cmp #$8000
    ror
    cmp #$8000
    ror
    cmp #$8000
    ror
    cmp #$8000
    ror
    cmp #$8000
    ror
    sec
    sbc #100
    ldx hs
    stw 2, x
    lda delta + 2
    stw 0, x
@next:
    inc d5
    lda hs
    clc
    adc #4
    sta hs
    lda vs
    clc
    adc #4
    sta vs
    inc row
    lda row
    cmp #$00E0
    bcs :+
    jmp @row
:
    ; rows above the road: the background's scroll
    lda d4
    sta blank_rows
    dec d4
    lda half
    clc
    adc #HSCROLL_LINES
    tax
    lda d4
    inc
    sta count               ; (0: 65536)
:   ldw BG_SCROLL
    stw 0, x
    ldw BG_SCROLL + 2
    stw 2, x
    txa
    clc
    adc #4
    tax
    dec count
    bne :-
    lda half
    clc
    adc #VSCROLL_LINES
    sta vs
    ldw ROAD_ROWS_BLANK
    sta top
    bpl @top_shown
    ; above the top: the lead rows count down, the rest hold
    eor #$FFFF
    inc
    and #$00FF              ; lead = (uint8)-top
    sta line
    lda d4
    sec
    sbc line
    dec
    sta d4
    stz v
    ldx vs
    lda line
    inc
    sta count
:   stz a:0, x
    lda v
    stw 2, x
    dec v
    txa
    clc
    adc #4
    tax
    dec count
    bne :-
    stx vs
    lda #$FFFF
    sta d5
    lda d4
    bmi @tops_done
    inc
    sta count
    jsr hold_rows
    lda #$FFFF
    sta d4
    bra @tops_done
@top_shown:
    lda d4
    inc
    sta count               ; (0: 65536)
    jsr hold_rows
    lda #$FFFF
    sta d4
@tops_done:
    ; the first halves of the top rows: minus the row, then held
    ldw ROAD_ROWS_BLANK
    eor #$FFFF
    sec
    adc #$0080 - $28        ; lines = $80 - blank - $28
    sta line
    stz v
    cmp blank_rows
    bcc @below
    lda blank_rows
    dec
    sta line
    jsr count_down
    bra @end
@below:
    jsr count_down
    lda blank_rows          ; rest = blank_rows - lines - 2
    sec
    sbc line
    dec
    dec
    sta count
    bmi @end
:   lda v
    stw 0, x
    txa
    clc
    adc #4
    tax
    dec count
    bpl :-
@end:
    lda half
    clc
    adc #VSCROLL_LINES
    tax
    stz a:0, x
    stz a:2, x
    sep #$20
    .a8
    lda #$FF
    sta a:SCROLL_FLIP
    rep #$20
    .a16
    rtl

; count entries from vs on: (BG_SCROLL, top - 1).
hold_rows:
    ldx vs
:   ldw BG_SCROLL
    stw 0, x
    lda top
    dec
    stw 2, x
    txa
    clc
    adc #4
    tax
    dec count
    bne :-
    rts

; line + 1 first halves from the buffer's start: v, counting down; X ends
; past them.
count_down:
    lda half
    clc
    adc #VSCROLL_LINES
    tax
    lda line
    inc
    sta count               ; (0: 65536)
:   lda v
    stw 0, x
    dec v
    txa
    clc
    adc #4
    tax
    dec count
    bne :-
    rts

; -------------------------------------------------------------- road rows

; Road line tables for the scroll code ($EFA8 before it): each road line's
; height (>> 4) plus 8 per line; then for each screen row the odd road
; line index drawn there, found by walking the road heights from the near
; end; rows the road climbs back over (hills) are filled backwards, and
; the last such row is noted for the line marker.
race__road_rows:
    ldx #$127E              ; $FF127E - 2k,
    ldy #$107E              ; $FF107E - 2k,
    stz CT1                 ; 8k
:   lda a:0, x
    xba
    jsr asr4
    clc
    adc CT1
    xba
    sta a:0, y
    dex
    dex
    dey
    dey
    lda CT1
    clc
    adc #8
    sta CT1
    cmp #$C0 * 8
    bcc :-
    ; rows not reached: none
    ldw ROAD_ROWS_BLANK
    eor #$FFFF
    sec
    adc #$0080              ; n = $80 - blank: (uint16)(n - 1) + 1 entries
    sta count               ; (0: 65536)
    ldx #ROAD_ROW_LINES
:   stz a:0, x
    inx
    inx
    dec count
    bne :-
    lda #1
    stw ROAD_HIDDEN_FROM
    ldw ROAD_Y_LINES
    jsr asr4
    sta top                 ; (prev)
    bmi @done
    lda #$00DF
    sec
    sbc top
    bmi @done
    asl
    clc
    adc #ROAD_ROW_LINES
    tax                     ; row
    stz row                 ; k
@line:
    lda row
    asl
    inc
    sta line                ; 2k + 1
    lda row
    asl
    tay
    lda ROAD_Y_LINES + 2, y
    xba
    jsr asr4
    sta v                   ; h
    lda top
    sec
    sbc v
    beq @same
    bmi @back
    sta count               ; down: d rows
:   inx
    inx
    lda line
    stw 0, x
    dec count
    bne :-
    bra @next_line
@same:
    lda line
    stw 0, x
    bra @next_line
@back:
    sta count               ; back up over a hill: -d rows
:   dex
    dex
    lda line
    stw 0, x
    inc count
    bne :-
    lda line
    stw ROAD_HIDDEN_FROM
    txa
    stw ROAD_HIDDEN_FROM + 2
@next_line:
    lda v
    sta top
    inc row
    lda row
    cmp #$00BF
    bcc @line
@done:
    ldw ROAD_HIDDEN_FROM
    cmp #$8000
    ror
    stw ROAD_HIDDEN_FROM
    ldw ROAD_HIDDEN_FROM + 2
    and #$01FF
    lsr
    stw ROAD_HIDDEN_FROM + 2
    rtl

; A >> 4, signed.
asr4:
    cmp #$8000
    ror
    cmp #$8000
    ror
    cmp #$8000
    ror
    cmp #$8000
    ror
    rts
