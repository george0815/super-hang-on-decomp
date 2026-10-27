; The SA-1's direct page (I-RAM at SA1_DP): the variables more than one
; file uses. Each file keeps its own in its range:
;   $00-$1F sa1.s, $40-$7F vdp.s, $C0-$D7 the VDP registers,
;   $E0-$EF compiled code, $F0-$FF rt.s

up_pos      = $00       ; next free byte in the upload list (sa1.s)
up_src      = $02       ; arguments of upload_vram: source (24 bits),
up_dst      = $06       ; VRAM word address,
up_len      = $08       ; length in bytes
up_budget   = $10       ; bytes the NMI can still upload this frame (sa1.s)
vdp_on      = $70       ; nonzero once vdp_reset has run: wait_vblank flushes
VDP_ADDR    = $40       ; vdp.s's port state, kept across the VBlank
VDP_CODE    = $42       ;   interrupt (sa1.s)
VDP_CTRL_PENDING = $72
VDP_CMD_FIRST = $74
VDP_FILL_PENDING = $76
VDP_REGS    = $C0       ; the Genesis VDP's registers 0-23 (vdp.s)
up_list     = $12       ; the upload list being built (bank $41 address)

; the compiled code's temporaries (tools/snes/c816.py); rt.s uses $F0-$FF
CHI         = $E0       ; high word of 32-bit values
CT0         = $E2
CT1         = $E4
CT2         = $E6
CPTR        = $E8       ; 24-bit pointer
CT3         = $EC
