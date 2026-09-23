COLBK  = $D01A
COLPF2 = $D018
WSYNC  = $D40A
VCOUNT = $D40B

.proc entry
@top    lda VCOUNT
        jne @top

        ldx #0
@line   sta WSYNC
        stx COLBK
        stx COLPF2
        inx
        cpx #240
        jcc @line

        jmp @top
.endp
