.section zeropage
pointer
        .res 2
.ends

.section
table
        .res 4
.ends

; tag four
; Four operands that begin with a parenthesis, and four different
; instructions. What each one came to is its size, and each is held to it
; below: two bytes where the parenthesis indirected, three where it grouped.
.proc entry
        ldy #0
        ldx #0
@a      lda (pointer),y
@b      lda (pointer,x)
@c      lda (table)+1
@d      lda (table),x
@e
        rts

        .assert @b - @a == 2
        .assert @c - @b == 2
        .assert @d - @c == 3
        .assert @e - @d == 3
.endp
; end four
