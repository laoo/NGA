; The shape every screen has: a title, and then whatever the Phase draws under
; it. Neither is here — this Module knows only that there are two of them, and
; which Phase is running it never learns.

; tag slot
.export title, draw, showScreen

.slot title, pointer, zeropage
.slot draw, vector
; end slot

; tag second
; A default, for a Phase that has nothing of its own to draw. This Module is
; resident, so it is live in every Phase.
.implements draw, runNothing
; end second

; tag screen
.proc showScreen
        ldy #0
@count  lda (title),y           ; the Phase's title, as long as it turns out
        beq @print
        iny
        bne @count
@print
        tya
        ldx title
        ldy title+1
        jsr printLine
        jsr draw
        rts
.endp
; end screen

.proc runNothing
        rts
.endp
