; tag dropped
; A display list is what ANTIC reads to know how to draw the screen, and the
; screen it points at is forty bytes ANTIC reads as well. No instruction in
; this program names either of them, because nothing in this program looks at
; them: the hardware does.
.section
displayList
        .byte $70, $70, $70, $42
        .word row
        .byte $41
        .word displayList
.ends

.section
row
        .res 40
.ends

.proc entry
        rts
.endp
; end dropped
