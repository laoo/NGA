; The machine's OS keeps its variables in page two, and reads and writes them
; on every interrupt. This program never touches them, so no instruction in it
; names them.
.section absolute at $0200
osVars
        .res $100
.ends
