; The SNES port's sound program (SPC700; tools/snes/spc700.py). The game's
; sound driver runs on the SA-1 (src/sound/driver.c), and what it plays is
; turned into DSP register writes there (snes/sound.s); the S-CPU passes
; them on, and this program makes them:
;   port 1: the register, port 2: the value, then port 0: a new count;
;   the count is echoed on port 0 once the write is done.
;
; ARAM: $0200 this program, $0300 the sample directory (DIR = 3, 128
; samples at most), $0500 on
; the samples (build/snes/gen/samples.bin, tools/snes/sound.py).

DSPADDR = $F2
DSPDATA = $F3
PORT0   = $F4
PORT1   = $F5
PORT2   = $F6

.org $0200
start:
    mov x, #$FF
    mov sp, x
    mov y, PORT0            ; the count the upload ended with
wait:
    cmp y, PORT0
    beq wait
    mov a, PORT1
    mov DSPADDR, a
    mov a, PORT2
    mov DSPDATA, a
    mov y, PORT0
    mov PORT0, y
    bra wait

.org $0300
directory:
    .incbin "build/snes/gen/samples_dir.bin"
.org $0500
samples:
    .incbin "build/snes/gen/samples.bin"
