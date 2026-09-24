.section
counter
        .res 1
.ends

.macro unrolled n
.if n > 0
        lda counter
        clc
        adc #7
        sta counter
        unrolled n - 1
.endif
.endm

; tag branch
; The same loop with the processor's own branch, which has one form and
; reaches what that form reaches.
.proc entry
        ldx #0
@loop
        unrolled 16
        inx
        cpx #5
        bcc @loop
        rts
.endp
; end branch
