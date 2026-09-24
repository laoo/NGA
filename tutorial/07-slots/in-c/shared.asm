; Nothing of the program uses this. It is pinned into the Window's range to
; show what the rule permits: a Section in no Pane is in the Window's base
; state, and the Pane's Sections are in another, so the two never collide
; however their Phases overlap. `root`, because nothing names it.

.section absolute at $4000, root
spare
        .res 64
.ends
