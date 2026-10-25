; A buffer of the program's own, placed wherever the solver finds room, and the
; code that uses it.
.section
buffer
        .res $100
.ends

.proc entry
        lda #0
        sta buffer
        rts
.endp
