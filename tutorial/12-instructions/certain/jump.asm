.section
flag
        .res 1
.ends

.macro unrolled n
.if n > 0
        lda flag
        clc
        adc #7
        sta flag
        unrolled n - 1
.endif
.endm

; tag certain
; Four jumps that are always taken, and none of them five bytes: there is no
; opposite branch and nothing to skip. The `clc` above each `jmpcc` is what
; makes the claim true; `jra` claims nothing and needs no such line.
.proc entry
        clc
@a      jmpcc @near
@b      inc flag
@near
        clc
@c      jmpcc @far
@d      unrolled 16
@far
@e      jra @beyond
@f      inc flag
@beyond
@g      jra @end
@h      unrolled 16
@end
        rts

        .assert @b - @a == 2
        .assert @d - @c == 3
        .assert @f - @e == 2
        .assert @h - @g == 3
.endp
; end certain
