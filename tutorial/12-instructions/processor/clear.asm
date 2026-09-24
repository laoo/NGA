.section
counter
        .res 1
.ends

; tag clear
; `stz` writes a zero without going through a register, and the 6502 has no
; such instruction.
.proc entry
        stz counter
        rts
.endp
; end clear
