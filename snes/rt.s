; The runtime of the compiled game code (tools/snes/c816.py): memory and
; hardware accesses the compiler could not resolve, 32-bit arithmetic,
; interrupts, waits, and the game functions the SNES does its own way.
;
; All are called with jsl. Addresses come as A (low word) and CHI (high)
; for reads; writes take the value in A (and CHI) and the address in CT0
; (low) and CT1 (high). The hardware behaves as the native port's
; (src/runtime/genesis.c).

.p816
.smart
.include "inc/hw.inc"
.include "inc/mem.inc"
.include "inc/dp.inc"
.include "inc/macros.inc"
.include "gen/assets.inc"

.export rt_read8, rt_read16, rt_read32, rt_write8, rt_write16, rt_write32
.export rt_mul32, rt_muls16, rt_mulu16, rt_shl16, rt_shl32, rt_lsr16, rt_asr16, rt_lsr32, rt_asr32
.export rt_divu16, rt_modu16, rt_divs32, rt_divu32, rt_mods32, rt_modu32
.export rt_get_sr, rt_set_sr, rt_idle, rt_fault, rt_call_ptr
.export rt_vdp_data16, rt_vdp_data32, rt_vdp_ctrl16, rt_vdp_ctrl32
.export trace_ring, trace_pos, pad_frames, pad_last
.export nemesis_to_vdp, nemesis_to_ram, memset, f_divs, f_divu, set_dn, f_logic
.export sprites_begin_c, sprite_object_c, sprites_end_c, sprites_hide_c, vdp_flush_c, frame_sync_c
.import vdp_ctrl_write, vdp_data_write, vdp_data_read, vdp_status, vdp_tiles
.import vdp_flush, upload_handover
.import sprites_begin, sprite_object, sprites_end, sprites_hide

; direct page: $F0-$FF (the runtime's own temporaries)
r_t0        = $F0
r_t1        = $F2
r_t2        = $F4
r_t3        = $F6
r_t4        = $F8
r_t5        = $FA
m_a         = $FC       ; mulu16's operands
m_b         = $FE

.segment "CODE"
.a16
.i16

; ---------------------------------------------------------------- memory

; Read 8 bits at A:CHI into A.
rt_read8:
    jsr where
    bcs :+
    lda [CPTR]
    and #$00FF
    rtl
:   jsr hw_read
    pha
    lda r_t0                ; the byte of the word
    lsr
    pla
    bcs :+
    xba
:   and #$00FF
    rtl

; Read 16 bits at A:CHI into A.
rt_read16:
    jsr where
    bcs :+
    lda [CPTR]
    xba
    rtl
:   jsr hw_read
    rtl

; Read 32 bits at A:CHI into A (low) and CHI.
rt_read32:
    jsr where
    bcs :+
    lda [CPTR]
    xba
    sta CHI
    ldy #2
    lda [CPTR], y
    xba
    rtl
:   pha
    jsr hw_read
    sta r_t1
    pla
    clc
    adc #2
    sta r_t0
    jsr hw_read_at
    pha
    lda r_t1
    sta CHI
    pla
    rtl

; Memory at A:CHI: CPTR = where it is and carry clear (RAM, ROM), or carry
; set for the hardware (r_t0, r_t2 = the address).
where:
    sta CPTR
    sta r_t0
    ldx CHI
    stx r_t2
    cpx #$00E0
    bcc :+
    lda #^MIRROR            ; $E0-$FF: the RAM, mirrored every 64 KB
    sta CPTR + 2
    clc
    rts
:   cpx #$0040
    bcs :+
    txa
    clc
    adc #^MDROM
    sta CPTR + 2
    clc
    rts
:   sec
    rts

; A hardware register's word at r_t2:r_t0.
hw_read_at:
hw_read:
    lda r_t2
    cmp #$00C0
    bne @not_vdp
    lda r_t0
    and #$001E
    cmp #$0004
    bcc @data
    cmp #$0008
    bcc @status
    lda #0                  ; (the HV counter)
    rts
@data:
    php
    sei
    jsr vdp_data_read
    plp
    rts
@status:
    php
    sei
    jsr vdp_status
    plp
    rts
@not_vdp:
    cmp #$00A1
    bne @not_io
    lda r_t0
    cmp #$1100
    bne :+
    lda #0                  ; the Z80's bus: granted
    rts
:   cmp #$0020
    bcs @zero
    and #$001E
    tax
    jmp (@io, x)
@io:
    .word @version, @pad1, @pad2, @zero, @ctrl1, @ctrl2, @zero, @zero
    .word @zero, @zero, @zero, @zero, @zero, @zero, @zero, @zero
@version:
    lda #$A0A0              ; overseas, NTSC, no expansion
    rts
@pad1:
    jsr pad_read
    rts
@pad2:
    lda #$7F7F
    rts
@ctrl1:
    lda f:pad_ctrl
    and #$00FF
    rts
@ctrl2:
    lda f:pad_ctrl + 1
    and #$00FF
    rts
@not_io:
    cmp #$00A0
    bne @zero
    lda r_t0                ; Z80 RAM: the mailbox
    and #$7FFF
    cmp #$4000
    bcs @zero
    and #$03FF
    tax
    lda f:z80_mailbox, x
    and #$00FF
    sta r_t1
    xba
    ora r_t1
    rts
@zero:
    lda #0
    rts

; The first pad's port as the Genesis reads it: TH high gives C B R L D U,
; TH low Start and A (active low; the pad is the SNES's, sa1.s maps it).
pad_read:
.ifdef PADTABLE
    jsr pad_from_table
.else
    lda f:PAD_HELD_MD       ; S A C B R L D U, 1 = down
.endif
    and #$00FF
    sta r_t1
    lda f:pad_th
    and #$0040
    beq @low
    lda r_t1
    and #$003F
    eor #$007F
    rts
@low:
    lda r_t1                ; S A -> bits 5, 4; D U stay
    and #$00C0
    lsr
    lsr
    sta r_t3
    lda r_t1
    and #$0003
    ora r_t3
    eor #$003F
    rts

.ifdef PADTABLE
; (Test builds) the pad of a recording, by game frame: the main loop's
; count at $FFC726, carried past 255 here, indexes the table built from
; the native port's pad log (tools/snes/padtable.py).
.import pad_table, pad_table_end
pad_from_table:
    lda a:$C726
    and #$00FF
    sec
    sbc f:pad_last
    and #$00FF
    clc
    adc f:pad_frames
    sta f:pad_frames
    lda a:$C726
    and #$00FF
    sta f:pad_last
    lda f:pad_frames
    cmp #.loword(pad_table_end - pad_table)
    bcs @none
    tax
    lda f:pad_table, x
    rts
@none:
    lda #0
    rts
.endif

; Write 8 bits (A) at CT1:CT0.
rt_write8:
    sta r_t4
    lda CT1
    cmp #$00E0
    bcc :+
    ldx CT0
    sep #$20
    lda r_t4
    sta a:0, x
    rep #$20
    rtl
:   lda r_t4
    and #$00FF
    sta r_t4
    lda CT1
    cmp #$00C0
    bne @not_vdp
    lda CT0
    and #$001F
    cmp #$0011              ; the PSG
    beq @done
    lda r_t4                ; byte writes repeat on the bus
    xba
    ora r_t4
    jsr vdp_word
@done:
    rtl
@not_vdp:
    cmp #$00A1
    bne @not_io
    lda CT0
    cmp #$0003
    beq @th
    cmp #$0009
    beq @ctrl
    cmp #$000B
    beq @ctrl2
    rtl
@th:
    lda r_t4
    sep #$20
    sta f:pad_th
    rep #$20
    rtl
@ctrl:
    lda r_t4
    sep #$20
    sta f:pad_ctrl
    rep #$20
    rtl
@ctrl2:
    lda r_t4
    sep #$20
    sta f:pad_ctrl + 1
    rep #$20
    rtl
@not_io:
    cmp #$00A0
    bne @done
    lda CT0
    and #$7FFF
    cmp #$4000
    bcs @done
    and #$1FFF
    cmp #$1C00
    bcc @done               ; (only the mailbox is kept)
    and #$03FF
    tax
    lda r_t4
    sep #$20
    sta f:z80_mailbox, x
    rep #$20
    rtl

; Write 16 bits (A) at CT1:CT0.
rt_write16:
    sta r_t4
    lda CT1
    cmp #$00E0
    bcc :+
    ldx CT0
    lda r_t4
    xba
    sta a:0, x
    rtl
:   cmp #$00C0
    bne :+
    lda r_t4
    jsr vdp_word
    rtl
:   cmp #$00A0
    bne :+
    lda r_t4                ; word writes store the high byte
    xba
    jmp rt_write8
:   rtl

; Write 32 bits (A, CHI) at CT1:CT0.
rt_write32:
    sta r_t5
    lda CT1
    cmp #$00E0
    bcc :+
    ldx CT0
    lda CHI
    xba
    sta a:0, x
    lda r_t5
    xba
    sta a:2, x
    rtl
:   cmp #$00C0
    bne :+
    lda CHI                 ; the high word first
    jsr vdp_word
    lda r_t5
    jsr vdp_word
:   rtl

; A word to the VDP port at CT0 (data or control).
vdp_word:
    tay
    lda CT0
    and #$001E
    cmp #$0004
    bcc @data
    cmp #$0008
    bcs @none
    tya
    php
    sei
    jsr vdp_ctrl_write
    plp
    rts
@data:
    tya
    php
    sei
    jsr vdp_data_write
    plp
@none:
    rts

; The VDP's ports at constant addresses: A (and CHI: its high word goes
; first) through the data or the control port.
rt_vdp_data16:
    php
    sei
    jsr vdp_data_write
    plp
    rtl

rt_vdp_data32:
    php
    sei
    pha
    lda CHI
    jsr vdp_data_write
    pla
    jsr vdp_data_write
    plp
    rtl

rt_vdp_ctrl16:
    php
    sei
    jsr vdp_ctrl_write
    plp
    rtl

rt_vdp_ctrl32:
    php
    sei
    pha
    lda CHI
    jsr vdp_ctrl_write
    pla
    jsr vdp_ctrl_write
    plp
    rtl

; Call the function at CPTR (24 bits), returning to the caller's caller:
; its address pushed as a return address (minus one), then rtl. (jml [ ]
; would read the pointer from bank 0, not the direct page.)
rt_call_ptr:
    sep #$20
    .a8
    lda CPTR + 2
    pha
    rep #$20
    .a16
    lda CPTR
    dec
    pha
    rtl

; -------------------------------------------------------------- arithmetic

; The 32-bit value on the caller's stack (at 4, s here: under the return
; address) * A:CHI -> A:CHI.
rt_mul32:
    sta r_t0
    lda CHI
    sta r_t1
    lda 4, s
    sta r_t2
    lda 6, s
    sta r_t3
    ; (a1 * 2^16 + a0) * (b1 * 2^16 + b0) mod 2^32
    ;   = a0 * b0 + 2^16 * (a0 * b1 + a1 * b0)
    lda r_t0
    ldx r_t2
    jsr mulu16              ; A:CHI = r_t0 * r_t2
    sta r_t4
    lda CHI
    sta r_t5
    lda r_t0
    ldx r_t3
    jsr mulu16
    clc
    adc r_t5
    sta r_t5
    lda r_t1
    ldx r_t2
    jsr mulu16
    clc
    adc r_t5
    sta CHI
    lda r_t4
    rtl

; Signed 16-bit A * X -> A:CHI (32 bits), on the SA-1's multiplier. (Its
; users mask the VBlank interrupt, whose code multiplies too.)
rt_muls16:
    php
    sei
    sta f:MA
    sep #$20
    .a8
    lda #0
    sta f:MCNT
    rep #$20
    .a16
    txa
    sta f:MB                ; starts it (5 cycles)
    nop
    nop
    lda f:MR + 2
    sta CHI
    lda f:MR
    plp
    rtl

; Unsigned 16-bit A * X -> A:CHI.
rt_mulu16:
    jsr mulu16
    rtl

; Unsigned A * X -> A (low) : CHI (high), on the SA-1's signed multiplier:
; with signed parts, (a - 2^16 ha)(b - 2^16 hb) gives a*b after adding
; back b*2^16 when a's top bit is set, and a*2^16 when b's is.
mulu16:
    sta m_a
    stx m_b
    php
    sei
    sep #$20
    .a8
    lda #0
    sta f:MCNT
    rep #$20
    .a16
    lda m_a
    sta f:MA
    lda m_b
    sta f:MB                ; starts the multiplication
    nop
    nop
    nop
    lda f:MR + 2
    sta CHI
    lda f:MR
    plp
    pha
    lda m_a
    bpl :+
    lda CHI
    clc
    adc m_b
    sta CHI
:   lda m_b
    bpl :+
    lda CHI
    clc
    adc m_a
    sta CHI
:   pla
    rts

; Shifts: A (and CHI) by X.
rt_shl16:
    cpx #0
    beq :++
:   asl
    dex
    bne :-
:   rtl

rt_lsr16:
    cpx #0
    beq :++
:   lsr
    dex
    bne :-
:   rtl

rt_asr16:
    cpx #0
    beq :++
:   cmp #$8000
    ror
    dex
    bne :-
:   rtl

rt_shl32:
    cpx #0
    beq :++
:   asl
    rol CHI
    dex
    bne :-
:   rtl

rt_lsr32:
    cpx #0
    beq :++
:   lsr CHI
    ror
    dex
    bne :-
:   rtl

rt_asr32:
    cpx #0
    beq :++
:   pha
    lda CHI
    cmp #$8000
    ror
    sta CHI
    pla
    ror
    dex
    bne :-
:   rtl

; Unsigned 16-bit A / X and A % X (0 for a zero divisor).
rt_divu16:
    jsr divu16
    rtl
rt_modu16:
    jsr divu16
    lda r_t1
    rtl

divu16:
    stx r_t0
    cpx #0
    bne :+
    lda #0
    sta r_t1
    rts
:   sta r_t2                ; dividend, shifted into the remainder
    stz r_t1
    ldx #16
:   asl r_t2
    rol r_t1
    lda r_t1
    sec
    sbc r_t0
    bcc :+
    sta r_t1
    inc r_t2
:   dex
    bne :--
    lda r_t2
    rts

; 32-bit division: the stack operand (4, s) by A:CHI. Signed results
; truncate towards zero, like C.
rt_divu32:
    jsr div_setup
    jsr divu32
    lda r_t2
    rtl

rt_modu32:
    jsr div_setup
    jsr divu32
    lda r_t5
    sta CHI
    lda r_t4
    rtl

rt_divs32:
    jsr div_setup
    jsr signs
    jsr divu32
    lda CT3                 ; signs differ: negate the quotient
    beq :+
    jsr neg_quotient
:   lda r_t2
    rtl

rt_mods32:
    jsr div_setup
    jsr signs
    jsr divu32
    lda CT2                 ; the remainder takes the dividend's sign
    beq :+
    lda r_t4
    eor #$FFFF
    clc
    adc #1
    sta r_t4
    lda r_t5
    eor #$FFFF
    adc #0
    sta r_t5
:   lda r_t5
    sta CHI
    lda r_t4
    rtl

; divisor (A:CHI) -> r_t0:r_t1, dividend (the caller's stack, 6, s here
; past two return addresses) -> r_t2:r_t3
div_setup:
    sta r_t0
    lda CHI
    sta r_t1
    lda 6, s
    sta r_t2
    lda 8, s
    sta r_t3
    rts

; Make both operands positive; CT2 = the dividend was negative, CT3 = the
; signs differed.
signs:
    stz CT2
    stz CT3
    lda r_t3
    bpl :+
    inc CT2
    inc CT3
    lda r_t2
    eor #$FFFF
    clc
    adc #1
    sta r_t2
    lda r_t3
    eor #$FFFF
    adc #0
    sta r_t3
:   lda r_t1
    bpl :+
    lda CT3
    eor #1
    sta CT3
    lda r_t0
    eor #$FFFF
    clc
    adc #1
    sta r_t0
    lda r_t1
    eor #$FFFF
    adc #0
    sta r_t1
:   rts

neg_quotient:
    lda r_t2
    eor #$FFFF
    clc
    adc #1
    sta r_t2
    lda r_t3
    eor #$FFFF
    adc #0
    sta r_t3
    rts

; Unsigned r_t3:r_t2 / r_t1:r_t0: quotient in r_t3:r_t2 (CHI = r_t3),
; remainder in r_t5:r_t4. A zero divisor gives 0.
divu32:
    lda r_t0
    ora r_t1
    bne :+
    stz r_t2
    stz r_t3
    stz r_t4
    stz r_t5
    stz CHI
    rts
:   stz r_t4
    stz r_t5
    ldx #32
    lda r_t3                ; a 16-bit dividend: its first 16 steps only
    bne :+                  ; shift zeros through
    lda r_t2
    sta r_t3
    stz r_t2
    ldx #16
:   lda r_t1
    bne @bit
    lda #0                  ; a 16-bit divisor: the remainder in A
@bit16:
    asl r_t2
    rol r_t3
    rol a
    bcs :+                  ; (17 bits: past the divisor whatever it is)
    cmp r_t0
    bcc @next16
:   sbc r_t0
    inc r_t2
@next16:
    dex
    bne @bit16
    sta r_t4
    lda r_t3
    sta CHI
    rts
@bit:
    asl r_t2
    rol r_t3
    rol r_t4
    rol r_t5
    lda r_t4
    sec
    sbc r_t0
    tay
    lda r_t5
    sbc r_t1
    bcc :+
    sta r_t5
    sty r_t4
    inc r_t2
:   dex
    bne @bit
    lda r_t3
    sta CHI
    rts

; -------------------------------------------------------------- interrupts

; The 68000's status register as the game sees it: the interrupt mask
; (7 while the SA-1's interrupts are off, else 0) in bits 8-10.
rt_get_sr:
    php
    sep #$20
    .a8
    pla
    rep #$20
    .a16
    and #$0004              ; I
    beq :+
    lda #$2700
    rtl
:   lda #$2000
    rtl

; Set it: a mask of 7 turns the VBlank interrupt off, below 6 on.
rt_set_sr:
    and #$0700
    cmp #$0600
    bcc :+
    sei
    rtl
:   cli
    rtl

; Let time pass in a wait loop: hand this frame's uploads to the next
; VBlank if the last ones are gone, then wait for an interrupt.
rt_idle:
    jsr flush
    php                     ; (with interrupts off nothing would come)
    sep #$20
    .a8
    pla
    rep #$20
    .a16
    and #$0004
    bne :+
    wai
:   rtl

; A game frame ends (system.c). In test builds, snesrun -F dumps the
; Genesis RAM now: wait for it, with the VBlank's work held back.
frame_sync_c:
.ifdef PADTABLE
    php
    sei
    sep #$20
    .a8
    lda f:SYNC_ON
    beq @off
    lda #1
    sta f:SYNC_FLAG
:   lda f:SYNC_FLAG
    bne :-
@off:
    plp
.endif
    rtl

; The VDP's changes so far to the next VBlank, if the last ones are gone
; (system.c, when a frame goes on without waiting).
vdp_flush_c:
    jsr flush
    rtl

flush:
    sep #$20
    .a8
    lda f:UPLOAD_READY
    rep #$20
    .a16
    bne @done
    lda z:vdp_on
    beq :+
    jsr vdp_flush
:   jsr upload_handover
@done:
    rts

; A fault the original would also stop at.
rt_fault:
    brk

; --------------------------------------------------------- game functions

; nemesis_to_vdp(uint32_t src): the build decoded every Nemesis block
; (assets.py); copy the one at src to the VDP's address.
nemesis_to_vdp:
    lda 6, s                ; (high word)
    sta r_t1
    lda 4, s
    sta r_t0
    jsr find_nemesis
    php
    sei
    jsr vdp_tiles
    plp
    rtl

; nemesis_to_ram(uint32_t src, uint32_t dst): the decoded block into the
; RAM mirror at dst.
nemesis_to_ram:
    lda 10, s
    sta r_t1
    lda 8, s
    sta r_t0
    jsr find_nemesis        ; A:X = the data, Y = tiles
    stx r_t2
    sta r_t2 + 2            ; (r_t3: its bank)
    tya
    asl
    asl
    asl
    asl
    asl
    sta r_t4                ; bytes
    lda 4, s                ; dst (low word)
    tax
    ldy #0
:   lda [r_t2], y
    sta a:0, x
    inx
    inx
    iny
    iny
    cpy r_t4
    bcc :-
    rtl

; The block at r_t1:r_t0 in the index: A:X = its data, Y = its tiles.
find_nemesis:
    ldx #0
@find:
    lda f:nemesis_index, x
    cmp #$FFFF
    beq @missing
    cmp r_t0
    bne @next
    lda f:nemesis_index + 2, x
    cmp r_t1
    beq @found
@next:
    txa
    clc
    adc #10
    tax
    bra @find
@found:
    lda f:nemesis_index + 8, x
    tay
    lda f:nemesis_index + 6, x
    pha
    lda f:nemesis_index + 4, x
    tax
    pla
    rts
@missing:
    brk                     ; a block the build didn't decode

; memset(void *p, int v, size_t n), for C objects (SNES addresses).
memset:
    lda 12, s
    sta r_t2
    lda 14, s
    sta r_t2 + 2
    lda 4, s                ; n (up to 64 KB)
    beq @done
    tax
    ldy #0
    sep #$20
    .a8
    lda 8, s
:   sta [r_t2], y
    iny
    dex
    bne :-
    rep #$20
    .a16
@done:
    rtl

; The 68000's divisions, which the game's arithmetic relies on:
; f_divs(uint32_t s, uint32_t d), f_divu(s, d): d / (16-bit) s as the
; remainder << 16 | the quotient; on overflow d unchanged (V set on the
; 68000); a zero divisor stops, as the original would trap. The quotient
; has to fit 16 bits, so 16 steps of shift and subtract do, starting from
; the dividend's high word.
div_overflow:
    lda m_b                 ; (the dividend, kept by div_args)
    sta CHI
    lda m_a
    rtl

f_divu:
    jsr div_args
    lda r_t0
    bne :+
    jmp rt_fault_long
:   lda r_t3                ; d's high word must be below s
    cmp r_t0
    bcs div_overflow
    jsr div16
    lda r_t4
    sta CHI
    lda r_t2
    rtl

f_divs:
    jsr div_args
    lda r_t0
    bne :+
    jmp rt_fault_long
:   lda r_t3                ; $80000000 / -1: 0
    cmp #$8000
    bne :+
    lda r_t2
    bne :+
    lda r_t0
    cmp #$FFFF
    bne :+
    stz CHI
    lda #0
    rtl
:   lda r_t0                ; the divisor sign-extended
    cmp #$8000
    lda #0
    bcc :+
    lda #$FFFF
:   sta r_t1
    jsr signs
    lda r_t3                ; |d|'s high word must be below |s|
    cmp r_t0
    bcs div_overflow
    jsr div16
    lda CT3
    beq @positive
    lda r_t2
    cmp #$8001
    bcs div_overflow
    eor #$FFFF
    inc
    sta r_t2
    bra @rem
@positive:
    lda r_t2
    bmi div_overflow
@rem:
    lda CT2                 ; the remainder takes the dividend's sign
    beq :+
    lda r_t4
    eor #$FFFF
    inc
    sta r_t4
:   lda r_t4
    sta CHI
    lda r_t2
    rtl

; r_t3:r_t2 / r_t0 (unsigned, r_t3 < r_t0): quotient in r_t2, remainder
; in r_t4.
div16:
    lda r_t3
    ldx #16
@bit:
    asl r_t2
    rol
    bcs @sub                ; (17 bits: above the divisor)
    cmp r_t0
    bcc @next
@sub:
    sbc r_t0                ; (carry set)
    inc r_t2
@next:
    dex
    bne @bit
    sta r_t4
    rts

; s -> r_t0 (16 bits); d -> r_t3:r_t2, and a copy in m_b:m_a for the
; overflow case. (Past this jsr's and the jsl's return addresses: d at
; 6, s, s at 10, s.)
div_args:
    lda 10, s
    sta r_t0
    lda 6, s
    sta r_t2
    sta m_a
    lda 8, s
    sta r_t3
    sta m_b
    rts

rt_fault_long:
    brk

; Register bookkeeping for the recompiled original: nothing here.
set_dn:
f_logic:
    rtl

; The sprite renderer (sprites.s) for objects.c's sprite table builder.
sprites_begin_c:
    jsr sprites_begin
    rtl

sprites_end_c:
    jsr sprites_end
    rtl

sprites_hide_c:
    jsr sprites_hide
    rtl

; sprite_object_c(uint32_t obj)
sprite_object_c:
    lda 4, s
    tax
    jsr sprite_object
    rtl

.segment "BWRAM"
trace_ring:     .res $400       ; (c816.py --trace) the last functions entered
trace_pos:      .res 2
z80_mailbox:    .res $400       ; Z80 RAM $1C00-$1FFF: the sound driver's mailbox
pad_th:         .res 1
pad_frames:     .res 2          ; (PADTABLE) game frames so far,
pad_last:       .res 2          ;  and $FFC726 when last read
pad_ctrl:       .res 2
