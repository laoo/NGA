; The map is not here any more. What is here is what draws it, and the row
; width it walks by.

levelHero = $2A                         ; `*` in ATASCII

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
; A Trampoline: it stands in base RAM, out of the Window, but runs with `level`
; shown, which is what lets it name what the Pane holds. It builds each line in
; `levelRow` rather than drawing out of the map, so that the hero can stand on
; a square without the map being written over.
.proc drawMap, under level
at      .ztemp 2
row     .ztemp 1
left    .ztemp 1

        lda #<levelMap
        sta at
        lda #>levelMap
        sta at+1
        lda #levelRows
        sta left
        lda #0
        sta row
@row
        ldy #levelWidth - 1
@copy
        lda (at),y
        sta levelRow,y
        dey
        bpl @copy

        lda row                 ; where the map said the level starts
        cmp #levelStartRow
        bne @print
        lda #levelHero
        sta levelRow + levelStartColumn
@print
        ldx #<levelRow
        ldy #>levelRow
        lda #levelWidth
        jsr printLine

        clc
        lda at
        adc #levelWidth
        sta at
        bcc @counted
        inc at+1
@counted
        inc row
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
