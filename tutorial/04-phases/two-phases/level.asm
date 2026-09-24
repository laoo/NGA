; What the Phase is for: a map of ten rows, which is what waits in storage
; while the program is in `intro`. Thirty-seven characters is what a row of
; the screen holds once the OS's left margin is taken off.

levelWidth = 37

.section
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

; A row at a time. `at` and `left` are Temporaries of this Proc, as chapter
; two's were, and `printLine` keeps neither of them.
.proc levelStart
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
@stop   jmp @stop
.endp
