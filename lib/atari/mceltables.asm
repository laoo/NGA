; The blitter's two input tables, and the seven bytes beside the first of them.
; They live in the blitter's own BRAM, which the window at $4000 shows one
; block at a time, so each is a Section in a Pane pinned to the state that
; shows it — the variant declares both Panes — and code reaches them under a
; `.with`, or from a Proc declared `under` one.
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
; What the records hold is the blitter's, and this file deliberately says none
; of it: twenty bytes a sprite and sixteen a texture, field by field, are the
; device's documentation and would be a second copy of it here. The addresses
; and the extents are what a program cannot work out for itself.

.export sprList, texTab
.export celVpXLo, celVpXHi, celVpYLo, celVpYHi
.export celColA, celColB, celColBMax

; SPRLIST: 256 records of twenty bytes, addressed page-major — the record's
; field chooses the page and its index the offset within it, so a record is not
; contiguous and a loop over one sprite walks the pages.
;
; It comes to 5120 bytes and the window is 16384, and what stands above it is
; not defined: a read there answers nothing in particular and a write is
; ignored. Hence two Sections and not one — the seven bytes below are the
; documented exception, and the gap between is in neither.
.section absolute at $4000, root, in sprlist
sprList
        .res 5120
.ends

; The viewport and the collision scan, which are registers of the blitter that
; happen to answer inside this window rather than beside CELCTL. They are here
; and not in the variant's `register` list for that reason: an address in this
; range is a register only while the window shows this block.
;
; `celVpX`/`celVpY` are the position of the display window over the scene,
; signed, in the sprites' own 12.4 — a pixel is sixteen.
;
; The collision scan is **latched**, and writing `celColA` is what starts it:
; the status it gives back on a read is not what was written there, so no
; read-modify-write instruction may be used on it — `inc celColA` after a hit
; would start a scan against a sprite index nobody chose. `celColB` takes one
; freely.
.section absolute at $5400, root, in sprlist
celVpXLo
        .res 1                          ; $5400
celVpXHi
        .res 1                          ; $5401
celVpYLo
        .res 1                          ; $5402
celVpYHi
        .res 1                          ; $5403
celColA
        .res 1                          ; $5404: written, a scan starts; read, the status
celColB
        .res 1                          ; $5405
celColBMax
        .res 1                          ; $5406
.ends

; TEXTAB: 256 descriptors of sixteen bytes, addressed record-major — a
; descriptor stands at its texture's number times sixteen. Four kilobytes
; exactly, which is the whole of what the third window would show; a Project
; that wants both large windows for something else can reach it there instead,
; and this file is then the wrong one to include.
.section absolute at $4000, root, in textab
texTab
        .res 4096
.ends
