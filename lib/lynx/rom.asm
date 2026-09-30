; The boot ROM's one entry a program of this tool wants. The ROM itself is a
; `reserved` Region of the variant and not a Module: the map this machine is
; described with never changes, so those 504 bytes are never the program's — see
; 0219. What this file holds is the address and the contract, which is what a
; program needs and all the tool can hold it to.
;
; `jsr romSetPage` shifts eight bits of `A` into the cartridge's address
; register, most significant first, and leaves the strobe low, so the counter is
; released and the page begins at its first byte. It does not preserve `X`. It
; leaves the cartridge powered and the strobe at rest, which is the state the
; next read wants.
;
; A program of C that wants the contract checked declares a Proc of its own that
; jumps here, since a `.declare` needs a Proc and a Proc holds code.
.export romSetPage

romSetPage = $FE00
