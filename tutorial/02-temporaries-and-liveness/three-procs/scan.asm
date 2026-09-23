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

.section zeropage
spread
        .res 1
.ends

.proc entry
        jsr findLargest
        jsr countAbove
        jsr report
        rts
.endp

.proc findLargest
best    .ztemp 1
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
        rts
.endp

.proc countAbove
tally   .ztemp 1
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

; tag report
.proc report
least   .ztemp 1
        lda #$FF
        sta least
        ldx #4
@pick   lda input,x
        cmp least
        jcs @kept
        sta least
@kept   dex
        bpl @pick
        jsr findLargest
        lda largest
        sec
        sbc least
        sta spread
        rts
.endp
; end report
