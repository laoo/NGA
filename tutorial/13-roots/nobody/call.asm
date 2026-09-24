.section zeropage
vector
        .res 2
.ends

.proc handler
        rts
.endp

; tag nobody
; The address of `handler` goes into a pointer and the pointer is jumped
; through. Neither line says the two are the same jump.
.section
address
        .word handler
.ends

.proc entry
        lda address
        sta vector
        lda address + 1
        sta vector + 1
        jmp (vector)
.endp
; end nobody
