.section
counter
        .res 1
.ends

; tag unrolled
; What stands between a branch and its target, written as a number rather than
; as lines. `n` is how many copies there are, and nothing in the source counts
; the bytes they come to.
.macro unrolled n
.if n > 0
        lda counter
        clc
        adc #7
        sta counter
        unrolled n - 1
.endif
.endm
; end unrolled

; tag loop
; The same mnemonic twice, and two different instructions. The first reaches
; forward over one instruction; the second reaches back over the whole
; unrolled body. Each is held to its size at the line below, and the source
; says nothing about either.
.proc entry
        ldx #0
@loop
        lda counter
@a      jcc @skip
@b      inc counter
@skip
        unrolled 16
        inx
        cpx #5
@c      jcc @loop
@d
        rts

        .assert @b - @a == 2
        .assert @d - @c == 5
.endp
; end loop
