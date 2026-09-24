; Advance a zero-page pointer by a constant. There is a shortest way for each
; of them, and which one is written out is decided where the macro is used.
.macro skip p, n
.if n == 1
        inc p
.elsif n == 2
        inc p
        inc p
.else
        clc
        lda p
        adc #n
        sta p
.endif
.endm

.proc entry
at      .ztemp 1

        skip at, 1
        skip at, 2
        skip at, 7
        rts
.endp
