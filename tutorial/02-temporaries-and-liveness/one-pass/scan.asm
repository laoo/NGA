.section
input
        .byte 3, 1, 4, 1, 5
.ends

.section zeropage
largest
        .res 1
.ends

.section zeropage
above
        .res 1
.ends

; tag body
.proc entry
best    .ztemp 1
tally   .ztemp 1

        lda #0
        sta best
        sta tally
        ldx #4
@scan   lda input,x
        cmp best
        jcc @kept
        sta best
@kept   lda input,x
        cmp #3
        jcc @next
        inc tally
@next   dex
        bpl @scan
        lda best
        sta largest
        lda tally
        sta above
        rts
.endp
; end body
