; The second buffer, for a program that draws into one while the display reads
; the other. It is wholly memory both masters agree on, so unlike `screen.asm`
; the CPU may write every byte of it — and a Payload may land here, which is how
; a picture arrives without being drawn.
;
; 32 bytes between the two buffers are left over — `$DFE0` to `$DFFF` — and the
; solver has them, since nothing declares them.
.export screen2

.section absolute at $C000 align 4, root, foreign
screen2
        .res 8160
.ends
