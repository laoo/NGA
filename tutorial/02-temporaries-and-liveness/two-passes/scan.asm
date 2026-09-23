; tag data
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

; end data

; tag body
.proc entry
best    .ztemp 1
tally   .ztemp 1

        lda #0
        sta best
        ldx #4
@pick   lda input,x
        cmp best
        jcc @kept
        sta best
@kept   dex
        bpl @pick
        lda best
        sta largest

        lda #0
        sta tally
        ldx #4
@count  lda input,x
        cmp #3
        jcc @next
        inc tally
@next   dex
        bpl @count
        lda tally
        sta above
        rts
.endp
; end body
