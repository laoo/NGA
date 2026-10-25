; The memory the Atari OS keeps its variables in: half the zero page and the
; pages from $0200 to $06FF. Sections that hold no bytes, pinned where the OS
; keeps them, so the solver sees memory that is taken rather than a Region it may
; never allocate from. See 0038 for why what occupies memory for a while is a
; Module, and 0229 for why the OS is two of them.
;
; **Resident wherever the OS is used at all**, and not only in the Phases whose
; ROM is in: a Phase without `atari/osrom.asm` runs with the ROM switched out and
; the OS asleep, and the variables have to be as the OS left them when a later
; Phase switches it back in.
;
; Every Section here is `root`: the OS and the hardware reach them with no
; Reference in any Chunk, so nothing else would keep them. None holds bytes, so
; none has a Payload and no Transition loads one. This is the OS as an XL or XE
; ships it; a machine whose OS lives elsewhere declares a Module of its own.

.export RTCLOK, SDMCTL, SDLSTL, SDLSTH, CH

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
