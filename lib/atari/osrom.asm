; The Atari OS ROM, in the two ranges the hardware registers leave between them,
; and the entry points a program calls in the second. Sections that hold no
; bytes, pinned where the ROM is, so that a Phase that needs this Module has
; the ranges taken and one without it has the RAM underneath — see 0229.
;
; **A ROM the driver switches out.** A variant lists this Module in `resident`,
; so every Phase has the OS unless it says `without osrom`, which the driver's
; `.driver rom osrom` is what allows; a Phase without it has the RAM under the
; ROM, its own vectors at $FFFA to look after, and no call into the OS — a
; `jsr` to an entry point below is a Reference to a Section that Phase does not
; have, and refused as one.
;
; The high range begins at **$E000 and not at $D800**: $D800-$DFFF is the
; floating-point pack, which a device on the parallel bus maps its own ROM
; over once it is selected, so it is `atari/mathpack.asm` — a Module a variant
; may leave out, and one whose absence is a fact about the machine rather than
; an omission. A variant that lists this file lists that one too, unless it
; has something else to say about those addresses.
;
; Every Section is `root`, since the OS and the hardware reach them with no
; Reference in any Chunk. This is the ROM an XL or XE ships.

.export SETVBV, XITVBV

.section absolute at $C000, root
osRomLow
        .res $1000
.ends

.section absolute at $E000, root
osRomHigh
        .res $45C
SETVBV  .res 3                          ; $E45C: A = the stage, X/Y = the routine
        .res 3
XITVBV  .res 3                          ; $E462: the end of a deferred routine
        .res $1B9B
.ends
