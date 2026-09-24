.section
introText
        .byte "INTRO"
introTextEnd
.ends

.proc introStart
        ldx #<introText
        ldy #>introText
        lda #introTextEnd - introText
        jsr printLine
        .transition level
.endp
