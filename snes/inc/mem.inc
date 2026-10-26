; Memory shared by the two CPUs

; The Genesis's work RAM ($FF0000-$FFFFFF) is mirrored byte for byte in
; BW-RAM bank $40: the port's variables sit where the original's do, in
; its byte order (big-endian; see inc/macros.inc), so the SNES's RAM can be
; compared with the recordings'. The SA-1 runs with the data bank at $40.
MIRROR = $400000

; I-RAM ($3000-$37FF, both CPUs; the SA-1 reaches it with long addresses
; since its data bank is BW-RAM)
UPLOAD_READY = $003000  ; byte: the SA-1 has a frame's uploads ready
PPU_SHADOW   = $003002  ; PPU registers the NMI applies with the uploads:
SH_INIDISP   = PPU_SHADOW + 0
SH_BGMODE    = PPU_SHADOW + 1
SH_BG1SC     = PPU_SHADOW + 2
SH_BG2SC     = PPU_SHADOW + 3
SH_BG12NBA   = PPU_SHADOW + 4
SH_TM        = PPU_SHADOW + 5
SH_OBSEL     = PPU_SHADOW + 6
SH_BG1HOFS   = PPU_SHADOW + 8   ; words
SH_BG1VOFS   = PPU_SHADOW + 10
SH_BG2HOFS   = PPU_SHADOW + 12
SH_BG2VOFS   = PPU_SHADOW + 14
PPU_SHADOW_END = PPU_SHADOW + 16

; The SA-1's direct page and stack are also in I-RAM
SA1_DP       = $3100
SA1_STACK    = $37FF

; The upload list (BW-RAM): 8-byte entries, built by the SA-1 during a
; frame and run by the S-CPU's NMI handler.
;   +0 kind (UP_END, UP_VRAM, UP_CGRAM, UP_OAM), +1 source (24 bits),
;   +4 destination (VRAM word address, CGRAM colour), +6 length in bytes
UPLOADS      = $41F000
UP_END       = 0
UP_VRAM      = 1
UP_CGRAM     = 2
UP_OAM       = 3

; Genesis RAM the frame loop and VBlank use (offsets in the mirror)
VBLANK_COUNT = $0500    ; word, counted by every VBlank
GAME_MODE    = $C704    ; word; bit 0: the mode's setup is done
VBLANK_HANDLER = $C722  ; word: 0 none, 4 menus, 8 races
PAD1_HELD    = $C706    ; byte: S A C B R L D U, 1 = down
PAD1_PRESSED = $C707    ; byte: newly pressed
PALETTE_DIRTY = $C708   ; byte, bit 0: upload the palette
FRAME_COUNTER_BYTE = $C726
PALETTE_BUFFER = $D380  ; 64 Genesis colours
