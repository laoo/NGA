; The display's memory, at the address every PET puts it: a page of RAM the CPU
; writes and the video circuit reads out, forty columns by twenty-five rows.
; Wholly memory both masters agree on — unlike a Lynx's buffer, nothing of the
; hardware answers at these addresses — so `foreign` is not wanted here and the
; CPU may write every byte of it.
;
; A thousand of the 1024 are displayed and the twenty-four above them are RAM
; the picture does not show, which a program may use for whatever it likes. The
; Section covers all of it, so that the solver allocates none of it either way.
;
; `root`, because the video circuit reaches it with no Reference in any Chunk
; and nothing else would keep it; it holds no bytes, so no Transition loads it.
; The Label is `screenRam` and not `screen`, which is the name of the Charset
; the display reads these bytes as — see pet/charsets.asm.
.export screenRam

.section absolute at $8000, root
screenRam
        .res 1024
.ends
