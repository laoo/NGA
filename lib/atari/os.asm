; The Atari OS as a Module: what it occupies while it is in memory, and the
; names a program reaches it by. Sections that hold no bytes, pinned where
; the OS lives, so the solver sees memory that is taken rather than a Region
; it may never allocate from — which is what lets a Phase that switches the
; ROM out have those addresses back, once there is a way to say so. See
; 0038 for why what occupies memory for a while is a Module, and 0051 for
; what this first cut settles and what it leaves.
;
; Every Section here is `root`: the OS and the hardware reach them with no
; Reference in any Chunk, so nothing else would keep them. None holds bytes,
; so none has a Payload and no Transition loads one.
;
; This is the OS as an XL or XE ships it. A machine whose OS lives elsewhere
; declares a Module of its own; the variant is what chooses.

.export RTCLOK, SDMCTL, SDLSTL, SDLSTH, CH, SETVBV, XITVBV

; The OS's half of the zero page. The program's own variables live above it,
; in the half the variant leaves to the solver.
.section zeropage at $0000, root
osZero
        .res $12
RTCLOK  .res 3                          ; $0012: the frame counter, three bytes
        .res $6B
.ends

; The OS's variables and buffers. The shadow registers are among them and
; are Labels here rather than registers of the variant: they are the OS's
; memory, which the OS copies to the hardware on every vertical blank, and
; a `register` Region inside this Section would be an address the Section
; may not cover.
.section absolute at $0200, root
osRam
        .res $2F
SDMCTL  .res 1                          ; $022F: shadows DMACTL
SDLSTL  .res 1                          ; $0230: the display list address, low
SDLSTH  .res 1                          ; $0231: and high
        .res $CA
CH      .res 1                          ; $02FC: the last key pressed, $FF for none
        .res $403
.ends

; The ROM, in the two ranges the hardware registers leave between them, and
; the entry points a program calls in the second. The RAM underneath is what
; a Phase without the OS would be given.
.section absolute at $C000, root
osRomLow
        .res $1000
.ends

.section absolute at $D800, root
osRomHigh
        .res $C5C
SETVBV  .res 3                          ; $E45C: A = the stage, X/Y = the routine
        .res 3
XITVBV  .res 3                          ; $E462: the end of a deferred routine
        .res $1B9B
.ends
