; The floating-point routines the OS keeps at $D800-$DFFF, as a Section that
; holds no bytes, so that the solver sees memory that is taken.
;
; **A Module of its own and not part of `atari/osrom.asm`**, because this is the
; one range of the OS that something else can take away. A device on the
; parallel bus maps its ROM over the whole of it once it is selected — the
; kernel's own PBI scan says so in as many words, and looks for a device's
; identifying bytes at $D803 and $D80B, which is why the math pack must not
; begin with them. A variant for a machine whose PBI device is selected for
; the life of the program therefore does not list this file, and says what
; stands there instead.
;
; **Switched out with the OS.** On an XL the one bit of PORTB that takes the OS
; ROM out takes this out too, so a driver that switches ROMs declares it as
; `.driver rom mathpack osrom`: a Phase that needs this needs the OS's ranges
; taken as well, and cannot do without `osrom` — see 0229.
;
; Nothing is exported. The pack's entry points are documented by Atari and a
; program that calls one declares it, as it declares any address of the OS
; `atari/osrom.asm` does not name; nothing the tool ships calls into it.
;
; `root`, because the OS reaches it with no Reference in any Chunk of the
; program, and holding no bytes it has no Payload and no Transition loads it.

.section absolute at $D800, root
mathPack
        .res $800
.ends
