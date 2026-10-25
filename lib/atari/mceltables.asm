; The blitter's two input tables, which live in its own BRAM and are shown at
; $4000 one block at a time. Each is a Section in a Pane pinned to the state
; that shows it — the variant declares both Panes — and code reaches them
; under a `.with`, or from a Proc declared `under` one.
;
; **None of them holds bytes**, and none could: a Pane pinned to a named state
; has no loader to come from, and BRAM is not memory a Container can write. The
; game fills them through the window, the sprite list once a frame and the
; texture table once a set of textures.
;
; `root`, because the blitter reads them with no Reference in any Chunk of the
; program. Not `foreign`: the CPU reads and writes every byte of them, unlike a
; Lynx's display buffer, and nothing of the hardware answers at these
; addresses — the window is what puts the blitter there.
;
; Everything here is in the Namespace `cel`, so that a program says
; `sta cel.angle,x` and carries no name of its own for any of it.

.namespace cel

; ---------------------------------------------------------------- SPRLIST
;
; **The sprite list is addressed by page and not by record**: the field picks
; the page and the sprite's number the offset within it, so sprite *i*'s angle
; is one byte at `cel.angle + i` and a loop over one field of every sprite is
; `ldx` counting to 256. A record is therefore *not* contiguous, and a loop
; over one sprite walks the pages.
;
; Which is why this is twenty Sections of 256 bytes and not one of 5120: a
; page is an address, of the same kind as the window's own, and naming it
; costs the library nothing it does not already know. What a page's byte
; *means* — the four bits `PALXOR` takes in `opcode`, the 8.8 of a scale, the
; 12.4 of a position — is the board's documentation and is deliberately not
; repeated here.
;
; The eight below are every sprite's, whatever it draws.

.section absolute at $4000, root, in sprlist
draw
        .res 256                        ; a 2x2 pattern; zero puts the sprite out
.ends

.section absolute at $4100, root, in sprlist
texid
        .res 256
.ends

.section absolute at $4200, root, in sprlist
dstXLo
        .res 256
.ends

.section absolute at $4300, root, in sprlist
dstXHi
        .res 256
.ends

.section absolute at $4400, root, in sprlist
dstYLo
        .res 256
.ends

.section absolute at $4500, root, in sprlist
dstYHi
        .res 256
.ends

.section absolute at $4600, root, in sprlist
flip
        .res 256
.ends

.section absolute at $4700, root, in sprlist
opcode
        .res 256                        ; and PALXOR in the top four bits
.ends

; **The twelve pages above are read differently for each opcode**, so each is
; one Section carrying a Label per reading. They are the same byte: writing
; `cel.scaleXLo` and reading `cel.euXLo` is one address, and which of them the
; blitter believes is what stands in `cel.opcode`.
;
; That is what a `union` is for, and it is written here as several names for
; one place because that is all it takes in this language — where the same
; record in C would cost a union, a tag and the ceremony of reading it back.

.section absolute at $4800, root, in sprlist
dstWLo                                  ; SCALE
scaleXLo                                ; ROTSCALE
euXLo                                   ; AFFINE and QUAD
        .res 256
.ends

.section absolute at $4900, root, in sprlist
dstWHi
scaleXHi
euXHi
        .res 256
.ends

.section absolute at $4A00, root, in sprlist
dstHLo
scaleYLo
euYLo
        .res 256
.ends

.section absolute at $4B00, root, in sprlist
dstHHi
scaleYHi
euYHi
        .res 256
.ends

.section absolute at $4C00, root, in sprlist
pivXLo                                  ; ROTSCALE
evXLo                                   ; AFFINE and QUAD
        .res 256
.ends

.section absolute at $4D00, root, in sprlist
pivXHi
evXHi
        .res 256
.ends

.section absolute at $4E00, root, in sprlist
pivYLo
evYLo
        .res 256
.ends

.section absolute at $4F00, root, in sprlist
pivYHi
evYHi
        .res 256
.ends

.section absolute at $5000, root, in sprlist
angle                                   ; ROTSCALE: one byte the whole turn
ewXLo                                   ; QUAD alone reads the EW pages
        .res 256
.ends

.section absolute at $5100, root, in sprlist
ewXHi
        .res 256
.ends

.section absolute at $5200, root, in sprlist
ewYLo
        .res 256
.ends

.section absolute at $5300, root, in sprlist
ewYHi
        .res 256
.ends

; The window is 16384 bytes and the twenty pages come to 5120. What stands
; between them and the seven bytes below is not defined: a read answers
; nothing in particular and a write is ignored, which is why there is no
; Section over it.

; ------------------------------------------------- viewport and collisions
;
; Registers of the blitter that answer inside this window rather than beside
; CELCTL, which is why they are here and not in the variant's `register` list:
; an address in this range is a register only while the window shows this
; block.
;
; `vpX`/`vpY` are the position of the display window over the scene, signed,
; in the sprites' own 12.4 — a pixel is sixteen.
;
; The collision scan is **latched, and writing `colA` is what starts it**: the
; status it gives back on a read is not what was written there, so no
; read-modify-write instruction may be used on it — `inc cel.colA` after a hit
; would start a scan against a sprite index nobody chose. `colB` takes one
; freely.
.section absolute at $5400, root, in sprlist
vpXLo
        .res 1                          ; $5400
vpXHi
        .res 1
vpYLo
        .res 1
vpYHi
        .res 1
colA
        .res 1                          ; written, a scan starts; read, the status
colB
        .res 1
colBMax
        .res 1                          ; $5406
.ends

; ----------------------------------------------------------------- TEXTAB
;
; **The texture table is addressed by record**, unlike the list above: a
; descriptor stands at its texture's number times sixteen. Four kilobytes
; exactly, which is the whole of what the board's third window would show, so
; a Project that wants both large windows for something else can reach it
; there instead — and this file is then the wrong one to include.
;
; One Section and not sixteen, because record-major is what the hardware does
; here; the offsets within a descriptor are the board's documentation.
.section absolute at $4000, root, in textab
texTab
        .res 4096
.ends

; Every name here is the Namespace's, so the exports stand inside the block and
; are written bare; from outside they are `cel.angle` and the rest.
.export draw, texid, dstXLo, dstXHi, dstYLo, dstYHi, flip, opcode
.export dstWLo, dstWHi, dstHLo, dstHHi
.export scaleXLo, scaleXHi, scaleYLo, scaleYHi
.export euXLo, euXHi, euYLo, euYHi
.export pivXLo, pivXHi, pivYLo, pivYHi
.export evXLo, evXHi, evYLo, evYHi
.export angle, ewXLo, ewXHi, ewYLo, ewYHi
.export vpXLo, vpXHi, vpYLo, vpYHi, colA, colB, colBMax
.export texTab

.endnamespace
