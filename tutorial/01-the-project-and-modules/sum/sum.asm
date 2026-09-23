.section
total
        .res 1
.ends

.proc entry
        lda #0
        ldx #4
@add    clc
        adc numbers,x
        dex
        bpl @add
        sta total
        rts
.endp

.section
numbers
        .byte 3, 1, 4, 1, 5
.ends
