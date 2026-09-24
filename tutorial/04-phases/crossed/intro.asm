.section
introText
        .byte "INTRO"
introTextEnd
.ends

; tag cross
.proc introStart
        ldx #<introText
        ldy #>introText
        lda #introTextEnd - introText
        jsr printLine
        lda levelMap            ; the Phase this belongs to has not loaded it
        .transition level
.endp
; end cross
