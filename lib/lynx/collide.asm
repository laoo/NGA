; The buffer the sprite engine builds collisions in, which the program points at
; through `COLLBAS`. The same 8160 bytes as a display buffer, and `foreign` for
; the same reason — but no `align`, since nothing rounds this address down: it is
; the display address register alone that ignores its low two bits.
.export collide

.section absolute at $A000, root, foreign
collide
        .res 8160
.ends
