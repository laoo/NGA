; tag name
; The way every other assembler writes it, and the one thing a branch may not
; hold: whether `frames` exists at all would depend on the condition.
.if NTSC
frames = 60
.endif
; end name

.section
second
        .byte frames
.ends

.proc entry
        lda second
        rts
.endp
