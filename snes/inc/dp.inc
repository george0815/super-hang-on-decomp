; The SA-1's direct page (I-RAM at SA1_DP): the variables more than one
; file uses. Each file keeps its own in its range:
;   $00-$1F sa1.s, $20-$3F logo.s, $40-$7F vdp.s, $C0-$D7 the VDP registers

up_pos      = $00       ; next free byte in the upload list (sa1.s)
up_src      = $02       ; arguments of upload_vram: source (24 bits),
up_dst      = $06       ; VRAM word address,
up_len      = $08       ; length in bytes
vdp_on      = $70       ; nonzero once vdp_reset has run: wait_vblank flushes
VDP_REGS    = $C0       ; the Genesis VDP's registers 0-23 (vdp.s)
