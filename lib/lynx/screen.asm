; The buffer the display is given: 80 bytes a line and 102 lines, which is 8160
; bytes, at the address every program on this machine puts it — so that it runs
; through the pages Suzy and Mikey answer at, which the CPU cannot use for
; anything and the display reads as memory. That overlap is what `foreign` is
; for, and `align 4` is what the display address register wants, since it
; ignores the low two bits and would round a buffer down in silence. See 0220.
;
; The CPU reaches `$E000` to `$FBFF` of this and no more, so a Payload cannot
; land here and nothing of the tool will put one here. What draws into it is the
; sprite engine, which the program points at it through `VIDBAS`, and what reads
; it out is the display, through `DISPADR`.
.export screen

.section absolute at $E000 align 4, root, foreign
screen
        .res 8160
.ends
