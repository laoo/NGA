; What the Phase is for: a map of ten rows, which is what waits in storage
; while the program is in `intro`. Thirty-seven characters is what a row of
; the screen holds once the OS's left margin is taken off.
;
; It lives in the Pane `level`, so it stands in a Bank and is read there.

levelWidth = 37

; tag section
.section in level
levelMap
        .byte "#####################################"
        .byte "#...................................#"
        .byte "#...................................#"
        .byte "#......########.....................#"
        .byte "#......#......#.....................#"
        .byte "#......#......#.....................#"
        .byte "#......########.....................#"
        .byte "#...................................#"
        .byte "#...................................#"
        .byte "#####################################"
levelMapEnd
.ends
; end section

; tag draw
; A Trampoline: it stands in base RAM, out of the Window, but runs with `level`
; shown — which is what lets it name what the Pane holds. Chapter five's loop,
; moved here whole.
.proc drawMap, under level
at      .ztemp 2
left    .ztemp 1

        lda #<levelMap
        sta at
        lda #>levelMap
        sta at+1
        lda #( levelMapEnd - levelMap ) / levelWidth
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

.proc levelStart
        .with level
        jsr drawMap
@stop   jmp @stop
.endp
; end draw
