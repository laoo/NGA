; The Atari BASIC ROM an XL or XE carries at $A000-$BFFF, as a Section that
; holds no bytes, so that a Phase that needs it has the range taken and one
; that does not has the RAM there — see 0229.
;
; **Not resident.** A program on these machines rarely wants BASIC, and a
; variant lists this Module without `resident`, so a Phase has it only by
; naming it in its `needs`; where the driver declares `.driver rom basic` the
; Phases that do not are run with BASIC switched out, whatever the machine was
; started with, and `finalize` puts back what the user had.
;
; `root`, because nothing in a program reaches it by Reference: a program that
; calls into BASIC declares the address it calls.

.section absolute at $A000, root
basicRom
        .res $2000
.ends
