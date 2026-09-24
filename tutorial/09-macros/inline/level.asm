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

; tag loop
; The drawing loop, as a macro rather than a Proc: a use of one is a single
; statement, which is what `.with` covers, so the whole loop runs with the Pane
; shown and there is no call to make. The bytes it works in are the user's,
; because a macro body holds instructions and data and declares nothing.
.macro drawMap at, row, left
        lda #<levelMap
        sta at
        lda #>levelMap
        sta at+1
        lda #levelRows
        sta left
        lda #0
        sta row
@line
        ldy #levelWidth - 1
@copy
        lda (at),y
        sta levelRow,y
        dey
        bpl @copy

; tag hero
        lda row                 ; where the map said the level starts
        cmp #levelStartRow
        bne @print
        lda #levelHero
        sta levelRow + levelStartColumn
; end hero
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
        bne @line
.endm
; end loop

; tag use
.proc levelDraw
at      .ztemp 2
row     .ztemp 1
left    .ztemp 1

        .with level
        drawMap at, row, left
        rts
.endp
; end use

.proc levelStart
        jsr showScreen
@stop   jmp @stop
.endp
