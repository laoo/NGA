; tag unprefixed
; A literal with no prefix is the source's own bytes, so it has to be ASCII.
.section
name
        .byte "ŻÓŁW"
.ends
; end unprefixed

.proc entry
        lda name
        rts
.endp
