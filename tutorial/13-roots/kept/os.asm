; The machine's OS keeps its variables in page two, and reads and writes them
; on every interrupt. Nothing in this program reaches them, so `root` says that
; something outside the program does.
.section absolute at $0200, root
osVars
        .res $100
.ends
