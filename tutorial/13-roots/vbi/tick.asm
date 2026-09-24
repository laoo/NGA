SAVMSC = $58                            ; where the OS put screen memory

.section zeropage
ticks
        .res 1
.ends

; tag handler
; The OS calls this sixty times a second once its address is in the vector,
; and no instruction in the program ever names it.
.proc onVerticalBlank
        inc ticks
        jmp XITVBV
.endp
; end handler

.section
word
        .byte screen"VBI RAN"
wordEnd
.ends

; tag install
; `SETVBV` takes the stage in `A` and the routine in `X` and `Y`, so the
; address is taken twice and each taking says who follows it. The hardware
; does, and `.root` is how that is written.
.proc entry
        lda #0
        sta ticks

        lda #6                          ; the deferred vertical blank
        .root
        ldx #>onVerticalBlank
        .root
        ldy #<onVerticalBlank
        jsr SETVBV

@wait   lda ticks                       ; a second of them, and then say so
        cmp #50
        jcc @wait

        ldy #wordEnd - word - 1
@copy   lda word,y
        sta (SAVMSC),y
        dey
        bpl @copy
@stop   jmp @stop
.endp
; end install
