; tag value
; A second is fifty frames on one machine and sixty on the other. That is one
; number with two values, not two programs, so it is written as a value.
frames = NTSC ? 60 : 50
; end value

.section
second
        .byte frames
.ends

.proc entry
        lda second
        rts
.endp
