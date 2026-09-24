; The map is not here any more. What is here is what draws it, and the row
; width it walks by.

levelWidth = 37

; tag fills
.implements title, levelTitle
.implements draw, levelDraw

.section
levelTitle
        .byte "LEVEL ONE", 0
.ends
; end fills

; A Trampoline: it stands in base RAM, out of the Window, but runs with `level`
; shown, which is what lets it name what the Pane holds.
.proc drawMap, under level
at      .ztemp 2
left    .ztemp 1

        lda #<levelMap
        sta at
        lda #>levelMap
        sta at+1
        lda #levelMap.runtimeSectionSize / levelWidth
        sta left
@row
        ldx at
        ldy at+1
        lda #levelWidth
        jsr printLine
        clc
        lda at
        adc #levelWidth
        sta at
        bcc @counted
        inc at+1
@counted
        dec left
        bne @row
        rts
.endp

.proc levelDraw
        .with level
        jsr drawMap
        rts
.endp

.proc levelStart
        jsr showScreen
@stop   jmp @stop
.endp
