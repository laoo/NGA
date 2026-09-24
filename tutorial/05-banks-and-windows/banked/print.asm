; The one thing both Phases need: a line on the screen, through the character
; I/O the OS opens on channel 0 before a program starts. `PUTREC` writes a
; record, so it ends the line itself.

CIOV   = $E456
ICCOM  = $0342
ICBAL  = $0344
ICBLL  = $0348
PUTREC = 9

; Both Phases call it, so both have to see the name.
.export printLine

; Where the line stands, and how long it is. Declaring where the two arguments
; are is what lets a Module of C call this one as a function.
.proc printLine
        .declare arg xy
        .declare arg a

        stx ICBAL
        sty ICBAL+1
        sta ICBLL
        lda #0
        sta ICBLL+1
        lda #PUTREC
        sta ICCOM
        ldx #0
        jsr CIOV
        rts
.endp
