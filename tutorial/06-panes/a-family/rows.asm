rowWidth = 37

; tag family
; One layout, three times. `slotRow` stands at the same address in every
; member of the family, so the two routines below name it once and reach a
; different Bank's bytes depending on which member is shown.
.section in slots
slotRow
        .res rowWidth
.ends

; Fills this member's row with `mark`. The byte comes through memory and not
; through a register: a `.with` shows the Window before the statement it
; covers, and showing it is the driver's code, which keeps nothing.
.proc fillRow, in slots
        ldy #rowWidth - 1
        lda mark
@byte   sta slotRow,y
        dey
        bpl @byte
        rts
.endp

.proc showRow, in slots
        ldx #<slotRow
        ldy #>slotRow
        lda #rowWidth
        jsr printLine
        rts
.endp
; end family

.section zeropage
mark    .res 1
which   .res 1
.ends

; tag use
; Two ways of naming a member. `slots + n` is one known where it is written;
; `slots, x` is whichever member's state is in `X`, which is how a loop walks
; them. The family's name on its own is the first member's state, so the
; counter starts there and `slots + 3` is one past the last.
.proc levelStart
        lda #'A'
        sta mark
        .with slots + 0
        jsr fillRow
        lda #'B'
        sta mark
        .with slots + 1
        jsr fillRow
        lda #'C'
        sta mark
        .with slots + 2
        jsr fillRow

        lda #slots
        sta which
@show
        ldx which
        .with slots, x
        jsr showRow
        inc which
        lda which
        cmp #slots + 3
        bne @show
@stop   jmp @stop
.endp
; end use
