CIOV   = $E456
ICCOM  = $0342
ICBAL  = $0344
ICBLL  = $0348
PUTCHR = $0B

; tag box
; The picture is the source. Every character here is the Unicode the Atari's
; own glyph is drawn as, and `atascii` is where the two are put side by side.
; `▄` and `▀` are one glyph and its inverse, which is the same byte with bit
; seven set — and Unicode draws both, so neither is a number here. So is a
; letter in inverse video: `🆂` is `S` with the bit set, and stands in a line of
; ordinary text without the line being cut in two.
.section
box
        .byte atascii"┌───────┐\n"
        .byte atascii"│ HELLO │\n"
        .byte atascii"└───────┘\n"
        .byte atascii"▄▀▄▀▄▀▄▀▄\n"
        .byte atascii"PRESS 🆂\n"
boxEnd
.ends
; end box

.proc entry
        lda #<box
        sta ICBAL
        lda #>box
        sta ICBAL+1
        lda #<( boxEnd - box )
        sta ICBLL
        lda #>( boxEnd - box )
        sta ICBLL+1
        lda #PUTCHR             ; the bytes as they are, end of line included
        sta ICCOM
        ldx #0
        jsr CIOV
@stop   jmp @stop
.endp
