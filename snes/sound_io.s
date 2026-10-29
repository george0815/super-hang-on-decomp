; The sound driver's view of the Z80's bus (src/sound/driver.c through
; tools/snes/include/z80_rt.h), by hand: the hot path of the sound host
; (snes/sound.c), which keeps the chips' registers here.
;   z80_read(uint16_t a), z80_write(uint16_t a, uint8_t v): $0000-$3FFF the
;   Z80's RAM ($1C00 on: the mailbox; below, the driver's code and tables,
;   read from the ROM), $4000-$4003 the YM2612, $6000 the bank register,
;   $7F11 the PSG, $8000-$FFFF the bank's 32 KB of the 68000's ROM.
;
; Conventions as compiled code: called with jsl, A and X/Y 16-bit, the data
; bank $40, the direct page SA1_DP (CT0-CT3 free).

.p816
.smart
.include "inc/dp.inc"

.export z80_read, z80_write
.export ym_fm, ym_keys, ym_event, ym_dirty, psg_tone, psg_vol, z80_bank
.import z80_mailbox

MDROM     = $C80000         ; the Genesis ROM (shangon.cfg)
Z80_IMAGE = $07D7F4         ; the driver in it (as the game loads it)
Z80_SIZE  = $1A24

; an FM channel's registers in ym_fm (32 each): $30-$9C (kind x 4 + operator),
; then $A0, $A4, $B0, $B4
R_A0 = 28
R_A4 = 29
R_B0 = 30
R_B4 = 31

.segment "CBSS"
ym_fm:      .res 6 * 32
ym_keys:    .res 6          ; operators keyed on
ym_event:   .res 6          ; 1 key on, 2 key off, since the host looked
ym_dirty:   .res 6          ; 1 frequency, 2 level or pan written since
ym_addr:    .res 2          ; each part's register latch
psg_tone:   .res 8          ; words
psg_vol:    .res 4          ; attenuation (15 off)
psg_latch:  .res 2
z80_bank:   .res 2          ; 68000 address bits 15-23 for $8000 on

.segment "CODE2"
.a16
.i16

; uint8_t z80_read(uint16_t a)  (a at 4, s)
z80_read:
    lda 4, s
    bmi @window
    cmp #$4000
    bcs @none
    and #$1FFF
    cmp #$1C00
    bcc @image
    tax
    lda f:z80_mailbox - $1C00, x
    and #$00FF
    rtl
@image:
    cmp #Z80_SIZE
    bcs @zero
    tax
    lda f:MDROM + Z80_IMAGE, x
    and #$00FF
    rtl
@none:
    cmp #$6000
    bcc @zero               ; YM2612 status: never busy
    lda #$00FF
    rtl
@zero:
    lda #0
    rtl
@window:                    ; MDROM + bank << 15 | (a & $7FFF)
    and #$7FFF
    sta CT0
    lda f:z80_bank
    lsr                     ; bank bit 0 to the word's bit 15
    bcc :+
    pha
    lda CT0
    ora #$8000
    sta CT0
    pla
:   clc
    adc #^MDROM
    sta CT1
    lda [CT0]
    and #$00FF
    rtl

; void z80_write(uint16_t a, uint8_t v)  (v at 4, s, a at 6, s)
z80_write:
    lda 6, s
    bmi @done
    cmp #$4000
    bcs @io
    and #$1FFF
    cmp #$1C00
    bcc @done               ; (the code and tables are ROM here)
    tax
    lda 4, s
    sep #$20
    .a8
    sta f:z80_mailbox - $1C00, x
    rep #$20
    .a16
@done:
    rtl
@io:
    cmp #$6000
    beq @bank
    bcs @psg
    and #3
    lsr
    tax                     ; the part
    bcs @data
    lda 4, s                ; the register latch
    sep #$20
    .a8
    sta f:ym_addr, x
    rep #$20
    .a16
    rtl
@data:
    lda f:ym_addr, x        ; the register (part 2: + $100)
    and #$00FF
    cpx #0
    beq :+
    ora #$0100
:   sta CT0
    lda 4, s
    and #$00FF
    sta CT1
    jmp ym_reg
@bank:
    lda f:z80_bank          ; bank = bank >> 1 | (v & 1) << 8
    lsr
    sta CT0
    lda 4, s
    and #1
    xba
    ora CT0
    sta f:z80_bank
    rtl
@psg:
    cmp #$7F11
    bne @done
    lda 4, s
    and #$00FF
    jmp psg_byte

; YM2612 register CT0 (part 2 at $100 on) = CT1.
ym_reg:
    lda CT0
    cmp #$0028
    bne :+
    jmp @key
:   and #$00FF
    cmp #$0030
    bcc @done
    tay                     ; (the register's low byte)
    and #3
    cmp #3
    beq @done
    sta CT2                 ; the channel: + 3 on part 2
    lda CT0
    cmp #$0100
    bcc :+
    lda CT2
    clc
    adc #3
    sta CT2
:   lda CT2
    asl
    asl
    asl
    asl
    asl
    sta CT3                 ; its registers' offset
    tya
    cmp #$00A0
    bcs @a0
    lsr                     ; $30-$9C: (kind - 3) x 4 + operator
    lsr
    and #3
    sta CT0
    tya
    lsr
    lsr
    lsr
    lsr
    sec
    sbc #3
    asl
    asl
    ora CT0
    tax
    lda #2                  ; (a level: the volume)
    bra @store
@a0:
    ldx #R_A0
    cmp #$00A4
    bcc @freq
    ldx #R_A4
    cmp #$00A8
    bcc @freq
    cmp #$00B0
    bcc @done
    ldx #R_B0
    cmp #$00B4
    bcc @level
    ldx #R_B4
    cmp #$00B8
    bcc @level
@done:
    rtl
@freq:
    lda #1
    bra @store
@level:
    lda #2
@store:                     ; ym_fm[CT3 + X] = CT1, ym_dirty[ch] |= A
    pha
    txa
    clc
    adc CT3
    tax
    lda CT1
    sep #$20
    .a8
    sta f:ym_fm, x
    ldx CT2
    pla
    ora f:ym_dirty, x
    sta f:ym_dirty, x
    pla                     ; (the high byte pushed)
    rep #$20
    .a16
    rtl
@key:                       ; $28: operators << 4 | part << 2 | channel
    lda CT1
    and #3
    cmp #3
    beq @done
    sta CT2
    lda CT1
    and #4
    beq :+
    lda CT2
    clc
    adc #3
    sta CT2
:   ldx CT2
    lda CT1
    lsr
    lsr
    lsr
    lsr
    sep #$20
    .a8
    sta CT0                 ; the operators now
    beq @off
    lda #1                  ; keyed (again): a note
    bra :+
@off:
    lda f:ym_keys, x
    beq :++                 ; (off already)
    lda #2
:   sta f:ym_event, x
:   lda CT0
    sta f:ym_keys, x
    rep #$20
    .a16
    rtl

; A PSG byte (A): a latch with data, or more data for the latched register.
psg_byte:
    bit #$0080
    beq @more
    sta f:psg_latch
    sta CT0
    lsr
    lsr
    lsr
    lsr
    lsr
    and #3
    tax                     ; the channel
    lda CT0
    bit #$0010
    bne @vol
    txa                     ; tone: the low 4 bits
    asl
    tax
    lda f:psg_tone, x
    and #$03F0
    sta CT1
    lda CT0
    and #$000F
    ora CT1
    sta f:psg_tone, x
    rtl
@vol:
    and #$000F
    sep #$20
    .a8
    sta f:psg_vol, x
    rep #$20
    .a16
    rtl
@more:
    sta CT0
    lda f:psg_latch
    lsr
    lsr
    lsr
    lsr
    lsr
    and #3
    tax
    lda f:psg_latch
    bit #$0010
    bne @vol_more
    cpx #3
    beq @none               ; (the noise: its mode only)
    txa                     ; tone: the high 6 bits
    asl
    tax
    lda CT0
    and #$003F
    asl
    asl
    asl
    asl
    sta CT1
    lda f:psg_tone, x
    and #$000F
    ora CT1
    sta f:psg_tone, x
@none:
    rtl
@vol_more:
    lda CT0
    bra @vol
