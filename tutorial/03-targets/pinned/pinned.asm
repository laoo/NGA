;tag sec
.section absolute at $0800
belowDos
        .res 1
.ends
;end sec

.proc entry
        lda belowDos
        rts
.endp
