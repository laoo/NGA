; tag fills
.implements title, introTitle
.implements draw, introDraw

.section
introTitle
        .byte "INTRO", 0
.ends
; end fills

.section
introText
        .byte "THE FIRST SCREEN"
introTextEnd
.ends

.proc introDraw
        ldx #<introText
        ldy #>introText
        lda #introTextEnd - introText
        jsr printLine
        rts
.endp

.proc introStart
        jsr showScreen
        .transition level
.endp
