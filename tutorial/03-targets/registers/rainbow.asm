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
