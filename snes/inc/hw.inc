; SNES and SA-1 registers

; PPU
INIDISP  = $2100
OBSEL    = $2101
OAMADDL  = $2102
OAMDATA  = $2104
BGMODE   = $2105
BG1SC    = $2107
BG2SC    = $2108
BG12NBA  = $210B
BG1HOFS  = $210D
BG1VOFS  = $210E
BG2HOFS  = $210F
BG2VOFS  = $2110
VMAIN    = $2115
VMADDL   = $2116
VMDATAL  = $2118
CGADD    = $2121
CGDATA   = $2122
TM       = $212C

; CPU
NMITIMEN = $4200
RDNMI    = $4210
HVBJOY   = $4212
JOY1L    = $4218
MDMAEN   = $420B
HDMAEN   = $420C
MEMSEL   = $420D

; DMA channel n
.define DMAP(n) $4300 + n * $10
.define BBAD(n) $4301 + n * $10
.define A1T(n)  $4302 + n * $10
.define A1B(n)  $4304 + n * $10
.define DAS(n)  $4305 + n * $10

; SA-1, written by the S-CPU
CCNT     = $2200        ; SA-1 control: $20 holds it in reset
SIE      = $2201
CRV      = $2203        ; SA-1 reset vector
CNV      = $2205        ; SA-1 NMI vector
CIV      = $2207        ; SA-1 IRQ vector
CXB      = $2220        ; ROM banks for $C0-$FF, 1 MB each
DXB      = $2221
EXB      = $2222
FXB      = $2223
BMAPS    = $2224        ; S-CPU's BW-RAM block at $6000
SBWE     = $2226        ; S-CPU BW-RAM write enable
SIWP     = $2229        ; S-CPU I-RAM write enable

; SA-1, written by the SA-1
CBWE     = $2227        ; SA-1 BW-RAM write enable
CIWP     = $222A        ; SA-1 I-RAM write enable
CIE      = $220A        ; SA-1 interrupt enable
CIC      = $220B        ; SA-1 interrupt clear
MCNT     = $2250        ; arithmetic: 0 multiply, 1 divide
MA       = $2251
MB       = $2253
MR       = $2306
