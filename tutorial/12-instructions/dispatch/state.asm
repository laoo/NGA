.section
state
        .res 1
.ends

.section
speed
        .res 1
.ends

; tag step
; Four positions of this Proc, chosen by what is in `A`. The clamp is the
; writer's: the statement indexes a table and does not check, exactly as any
; other index does not.
.proc step
        lda state
        cmp #3
        jcc @ok
        lda #3
@ok     .dispatch @idle, @walk, @run, @stop
@idle   lda #0
        jmp @set
@walk   lda #1
        jmp @set
@run    lda #4
        jmp @set
@stop   lda #0
@set    sta speed
        rts
.endp
; end step
