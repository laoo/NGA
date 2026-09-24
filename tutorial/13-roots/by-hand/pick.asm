.section
input
        .byte 3, 1, 4, 1, 5
.ends

.section
out
        .res 1
.ends

; tag by-hand
; The same routine with the dispatch written out: this is the instruction the
; tool writes for a `.dispatch` on a 6502, and the table is of this Proc's own
; positions, so the Proc is what follows the addresses in it.
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
; The same choice as the instruction the tool would have written: the `rts`
; trick over two half tables of each target less one. The tables hold this
; Proc's own positions, so `.own` with no name is what declares them.
        lda best
        cmp #3
        jcc @ok
        lda #2
@ok     tax
        lda @hi,x
        pha
        lda @lo,x
        pha
        rts
        .own
@hi     .byte >(@low - 1), >(@mid - 1), >(@high - 1)
        .own
@lo     .byte <(@low - 1), <(@mid - 1), <(@high - 1)
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
; end by-hand
