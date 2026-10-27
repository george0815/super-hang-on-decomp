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

; ------------------------------------------------------------ road builder

.export race_build_road
.import race__isqrt_hex, race__track_heading, f_divs

ROAD_SCRATCH      = $0730
ROAD_BEND_LINES   = $0B00
COURSE_PROGRESS   = $0616
TRACK_GEOMETRY    = $0800   ; long: the course's direction vectors
ROM_SEGMENT_LINES = $00FA14 ; screen line of each of 33 segments
MA_L              = $002251
MB_L              = $002253
MR_L              = $002306
MCNT_L            = $002250

; direct page: $80-$9F (race_build_road)
rb_p        = $80       ; the track pointer (24 bits; in the ROM copy)
rb_vx       = $84
rb_vy       = $86
rb_dirx     = $88
rb_diry     = $8A
rb_px       = $8C       ; 32 bits
rb_py       = $90       ; 32 bits
rb_x        = $94
rb_y        = $96
rb_seg      = $98
rb_out      = $9A
rb_prevx    = $9C
rb_prevl    = $9E
rb_side     = $A4
rb_line     = $A6
rb_t0       = $A8       ; 32 bits
rb_t1       = $AC       ; 32 bits
rb_step     = $B0
rb_v        = $B2
rb_l        = $B4

; A:X (high word in X) signed 32 / Y (16 bits) as divs_q: f_divs's low
; word (the quotient, or the dividend's low word on overflow).
.macro divs_q_call
    phy                     ; (f_divs(s, d): s high, s low, d high, d low)
    phy
    phx
    pha
    lda #0
    sta 7, s                ; s's high word
    jsl f_divs
    plx
    plx
    plx
    plx
.endmacro

; Signed 16-bit A * X -> A:CHI (the SA-1's multiplier).
smul:
    sta f:MA_L
    sep #$20
    .a8
    lda #0
    sta f:MCNT_L
    rep #$20
    .a16
    txa
    sta f:MB_L
    nop
    nop
    lda f:MR_L + 2
    sta CHI
    lda f:MR_L
    rts

; Project the road ahead into the per-line bend table ($ED7C): walk 33
; track segments from the bike, rotate them into the bike's heading,
; divide the sideways offset by depth and fill the scanlines between
; consecutive segments by linear interpolation; then double the 96 lines
; to 192 and derive the road centre lines (plus the sideways sweep while
; being pushed).
race_build_road:
    lda a:$C8A8
    beq :+
    rtl
:   ldw COURSE_PROGRESS     ; idx = (progress >> 1) * 4 (signed)
    cmp #$8000
    ror
    asl
    asl
    stw ROAD_SCRATCH
    sta rb_t0
    ldw TRACK_GEOMETRY + 2  ; p = base + (int16)idx
    clc
    adc rb_t0
    sta rb_p
    lda rb_t0
    cmp #$8000
    lda #0
    bcc :+
    lda #$FFFF
:   sta rb_t1
    ldw TRACK_GEOMETRY
    adc rb_t1               ; (carry from the low words)
    clc
    adc #^MDROM
    sta rb_p + 2
    ; the direction at the bike, its length and the unit vector
    ldy #0
    lda [rb_p], y
    xba
    ldy #4
    sta rb_t0
    lda [rb_p], y
    xba
    clc
    adc rb_t0
    sta rb_vx
    stw ROAD_SCRATCH + $30
    ldy #2
    lda [rb_p], y
    xba
    sta rb_t0
    ldy #6
    lda [rb_p], y
    xba
    clc
    adc rb_t0
    sta rb_vy
    stw ROAD_SCRATCH + $40
    lda rb_vx               ; vx * vx + vy * vy
    tax
    jsr smul
    sta rb_t0
    lda CHI
    sta rb_t0 + 2
    lda rb_vy
    tax
    jsr smul
    clc
    adc rb_t0
    tax
    lda CHI
    adc rb_t0 + 2
    pha                     ; isqrt_hex(uint32_t n)
    phx
    jsl race__isqrt_hex
    plx
    plx
    sta rb_t1               ; len
    lda rb_vx               ; dirx = divs_q(vx << 14, len)
    jsr shl14
    ldy rb_t1
    divs_q_call
    sta rb_dirx
    stw ROAD_SCRATCH + $50
    lda rb_vy
    jsr shl14
    ldy rb_t1
    divs_q_call
    sta rb_diry
    stw ROAD_SCRATCH + $60
    ; the heading (track_heading(uint32_t p), with p as the Genesis address)
    lda rb_p + 2
    sec
    sbc #^MDROM
    pha
    pei (rb_p)
    jsl race__track_heading
    plx
    plx
    stw ROAD_HEADING
    stz a:ROAD_SCRATCH + $70
    stz a:ROAD_SCRATCH + $72
    stz a:ROAD_SCRATCH + $80
    stz a:ROAD_SCRATCH + $82
    stz a:ROAD_SCRATCH + $90
    stz a:ROAD_SCRATCH + $A0
    stz rb_px
    stz rb_px + 2
    stz rb_py
    stz rb_py + 2
    stz rb_prevx
    stz rb_prevl
    lda #$BE
    sta rb_out
    lda #$20
    sta rb_seg
@segment:
    ; px += (int16)p[0] + (int16)p[4]; py likewise from p[2], p[6]
    ldy #0
    lda [rb_p], y
    xba
    sta rb_t0
    ldy #4
    lda [rb_p], y
    xba
    jsr add_px
    ldy #2
    lda [rb_p], y
    xba
    sta rb_t0
    ldy #6
    lda [rb_p], y
    xba
    jsr add_py
    lda rb_px + 2
    stw ROAD_SCRATCH + $70
    lda rb_px
    stw ROAD_SCRATCH + $72
    lda rb_py + 2
    stw ROAD_SCRATCH + $80
    lda rb_py
    stw ROAD_SCRATCH + $82
    lda rb_px               ; x = px >> 5, y = py >> 5 (16 bits)
    ldx rb_px + 2
    jsr asr5
    sta rb_x
    lda rb_py
    ldx rb_py + 2
    jsr asr5
    sta rb_y
    ; fwd = (x * dirx + y * diry) >> 7; the divisor is (fwd >> 7) + $410
    lda rb_x
    ldx rb_dirx
    jsr smul
    sta rb_t0
    lda CHI
    sta rb_t0 + 2
    lda rb_y
    ldx rb_diry
    jsr smul
    clc
    adc rb_t0
    sta rb_t0
    lda CHI
    adc rb_t0 + 2
    sta rb_t0 + 2           ; x * dirx + y * diry
    ldx #14                 ; >> 7 >> 7 (signed), low word only
:   lda rb_t0 + 2
    cmp #$8000
    ror rb_t0 + 2
    ror rb_t0
    dex
    bne :-
    lda rb_t0
    clc
    adc #$0410
    sta rb_t1               ; the divisor
    ; side = divs_q(x * diry - y * dirx, divisor)
    lda rb_x
    ldx rb_diry
    jsr smul
    sta rb_t0
    lda CHI
    sta rb_t0 + 2
    lda rb_y
    ldx rb_dirx
    jsr smul
    sta CT0
    lda rb_t0
    sec
    sbc CT0
    sta rb_t0
    lda rb_t0 + 2
    sbc CHI
    tax
    lda rb_t0
    ldy rb_t1
    divs_q_call
    sta rb_side
    stw ROAD_SCRATCH + $B0
    lda rb_seg
    asl
    tax
    lda f:MDROM + ROM_SEGMENT_LINES, x
    xba
    sta rb_line
    stw ROAD_SCRATCH + $C0
    sec                     ; span = line - prev_line
    sbc rb_prevl
    bpl :+
    jmp @expand
:   bne :+
    jmp @next
:   sta rb_t1               ; step = divs_q((int16)(side - prev_x), span)
    lda rb_side
    sec
    sbc rb_prevx
    ldx #0
    cmp #$8000
    bcc :+
    dex
:   ldy rb_t1
    divs_q_call
    sta rb_step
    lda rb_prevx
    sta rb_v
    lda rb_prevl
    sta rb_l
@fill:
    lda rb_v
    clc
    adc rb_step
    sta rb_v
    inc rb_l
    sec                     ; v > $3200 or v < $CE00 (signed): stop
    sbc #$3201
    bvc :+
    eor #$8000
:   bmi :+
    jmp @expand
:   lda rb_v
    sec
    sbc #$CE00
    bvc :+
    eor #$8000
:   bpl :+
    jmp @expand
:   lda rb_v
    ldx rb_out
    stw ROAD_BEND_LINES, x
    dex
    dex
    stx rb_out
    bpl :+
    jmp @expand
:   lda rb_l
    cmp rb_line
    bne @fill
    lda rb_side
    sta rb_prevx
    stw ROAD_SCRATCH + $90
    lda rb_line
    sta rb_prevl
    stw ROAD_SCRATCH + $A0
@next:
    lda rb_p                ; p += 4
    clc
    adc #4
    sta rb_p
    bcc :+
    inc rb_p + 2
:   dec rb_seg
    bmi @expand
    jmp @segment
@expand:
    ; 96 lines -> 192 by inserting midpoints, from the bottom up
    ldx #ROAD_BEND_LINES + $BE      ; src
    ldy #ROAD_BEND_LINES + $180     ; dst
    lda #$5F
    sta rb_seg
:   ldw 0, x                ; a
    sta rb_t0
    dey
    dey
    stw 0, y
    dex
    dex
    ldw 0, x                ; b
    sec
    sbc rb_t0
    cmp #$8000
    ror
    clc
    adc rb_t0
    dey
    dey
    stw 0, y
    dec rb_seg
    bne :-
    ldw 2, x                ; b = src[1], d = (src[0] - b) >> 1
    sta rb_t0
    ldw 0, x
    sec
    sbc rb_t0
    cmp #$8000
    ror
    clc
    adc rb_t0
    stw 2, x
    ; road centre lines: the bend, plus a sweep while being pushed
    ldw $0622
    asl
    asl
    asl
    asl
    asl
    asl
    sta rb_t1               ; sweep
    bne @sweep
    ldx #0
:   lda a:ROAD_BEND_LINES, x
    sta a:ROAD_CENTRE_LINES, x
    inx
    inx
    cpx #$E0 * 2
    bcc :-
    rtl
@sweep:
    ldx #0                  ; sweep = divs_q(sweep, $E0)
    cmp #$8000
    bcc :+
    dex
:   ldy #$E0
    divs_q_call
    sta rb_step
    stz rb_t0               ; acc
    ldx #0
@centre:
    ldw ROAD_BEND_LINES, x
    cmp #$3210
    beq :+
    clc
    adc rb_t0
:   stw ROAD_CENTRE_LINES, x
    lda rb_t0
    clc
    adc rb_step
    sta rb_t0
    inx
    inx
    cpx #$E0 * 2
    bcc @centre
    rtl

; A (signed 16 bits) << 14 -> A (low word), X (high word).
shl14:
    cmp #$8000
    ldx #0
    bcc :+
    dex
:   stx rb_t0 + 2
    ldx #14
:   asl
    rol rb_t0 + 2
    dex
    bne :-
    ldx rb_t0 + 2
    rts

; px/py += (int16)rb_t0 + (int16)A (32 bits).
add_px:
    jsr sum16
    clc
    adc rb_px
    sta rb_px
    txa
    adc rb_px + 2
    sta rb_px + 2
    rts

add_py:
    jsr sum16
    clc
    adc rb_py
    sta rb_py
    txa
    adc rb_py + 2
    sta rb_py + 2
    rts

; A:X (X the high word) = (int32)(int16)rb_t0 + (int32)(int16)A
sum16:
    sta CT0
    ldx #0
    cmp #$8000
    bcc :+
    dex
:   stx CT1
    lda rb_t0
    ldx #0
    cmp #$8000
    bcc :+
    dex
:   stx CT2
    clc
    adc CT0
    pha
    lda CT2
    adc CT1
    tax
    pla
    rts

; (A low, X high) >> 5, signed: the low word.
asr5:
    sta CT0
    stx CT1
    ldx #5
:   lda CT1
    cmp #$8000
    ror CT1
    ror CT0
    dex
    bne :-
    lda CT0
    rts
