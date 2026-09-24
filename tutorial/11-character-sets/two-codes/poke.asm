SAVMSC = $58                            ; where the OS put screen memory

; tag word
; Written where the display looks rather than through the character I/O, so
; these are the display's numbers and not the machine's.
.section
word
        .byte screen"NGA"
wordEnd
.ends
; end word

; tag assert
; The same letter has a number in each, and nothing has to be remembered: the
; tool is held to both, here, at the line that says so.
.assert atascii'A'  == $41
.assert screen'A'   == $21
.assert atascii'\n' == $9B
; end assert

.proc entry
        ldy #wordEnd - word - 1
@copy   lda word,y
        sta (SAVMSC),y
        dey
        bpl @copy
@stop   jmp @stop
.endp
