.section
input
        .byte 3, 1, 4, 1, 5
.ends

.section
out
        .res 1
.ends

; tag dispatch
; The largest of five, put into one of three bands. The two Temporaries are
; used in stretches that do not overlap.
.proc entry
best    .ztemp 1
band    .ztemp 1

        lda #0
        sta best
        ldx #4
@pick   lda input,x
        cmp best
        jcc @kept
        sta best
@kept   dex
        bpl @pick

; tag choose
; Where the three bands are chosen. The statement names the positions it goes
; to, so they are Successors of it and the Proc's flow is known.
        lda best
        cmp #3
        jcc @ok
        lda #2
@ok     .dispatch @low, @mid, @high
; end choose

@low    lda #0
        jmp @done
@mid    lda #1
        jmp @done
@high   lda #2
@done
        sta band
        lda band
        sta out
        rts
.endp
; end dispatch
