; tag both
; Both readings of a pack in one statement: the name alone is how many
; elements it has, and the name with the dots spreads them where it stands.
.macro table values...
        .byte values, values...
.endm
; end both

; tag walk
; The other way to walk one: a case per shape, and a recursion that takes an
; element off each turn. The empty case is where it stops.
.macro poke at, values...
.match values
.case first, rest...
        lda #first
        sta at
        poke at + 1, rest...
.case
.endmatch
.endm
; end walk

.section
counts
        table 3, 1, 4, 1, 5
.ends

.section
buffer
        .res 3
.ends

; tag counted
; The third shape, and the one the library uses: a count that comes down by one
; each turn, with `.if` on it deciding whether there is another. It writes as
; many of the list as the number says and drops the rest, so a list may be
; longer than the machine it is written for.
.macro take n, values...
.if n > 0
.match values
.case head, rest...
        .byte head
        take n - 1, rest...
.endmatch
.endif
.endm
; end counted

.section
shorter
        take 3, 3, 1, 4, 1, 5
.ends

; tag uses
.proc entry
        poke buffer, 9, 2, 6
        lda counts
        lda shorter
        rts
.endp
; end uses
