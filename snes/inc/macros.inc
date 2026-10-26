; Access to the Genesis RAM mirror and ROM copy, whose words are
; big-endian. A 16-bit.
;
; After xba the flags reflect only A's low byte: test a loaded word's sign
; with `bit #$8000` or a transfer (tax, tay), not with bmi straight away.
; Zero tests need no swap: load without it.

MDROM = $C80000         ; the Genesis ROM, as is (see shangon.cfg)

; Load the big-endian word at addr (in the data bank), or addr + X/Y.
.macro ldw addr, index
    .ifblank index
        lda a:addr
    .else
        lda a:addr, index
    .endif
    xba
.endmacro

; Store A as a big-endian word at addr, or addr + X/Y. A is kept.
.macro stw addr, index
    xba
    .ifblank index
        sta a:addr
    .else
        sta a:addr, index
    .endif
    xba
.endmacro

; Load the big-endian ROM word at Genesis address addr (+ X).
.macro ldrom addr, index
    .ifblank index
        lda f:MDROM + addr
    .else
        lda f:MDROM + addr, index
    .endif
    xba
.endmacro
