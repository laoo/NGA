; The driver for a Lynx cartridge, which is the one medium of this tool that is
; not in the address space at all: a page is shifted into an address register
; eight bits at a time and a counter walks it, so there is no seeking and no
; Window — `stream`, `show` and `showAt` are not declared, and the whole of the
; hardware is two of Suzy's addresses and two bits of Mikey's.
;
; A unit of storage is a **page**, and the model's unit *i* is page *i + 1*: page
; zero holds the bootstrap, which is a constant the Container and this file both
; hold since neither can tell the other — the same arrangement the diskette has
; with sector four. See docs/spec/lnx.md.
;
; `seek forward` is what this medium can do about an offset: nothing but read its
; way there. So `open` reaches one by reading, and reads as few as it can — from
; where the stream stands, where the page is the one wanted and the offset is
; ahead of it, and from the page's first byte otherwise. What makes that worth
; having is an edge written `fast`, whose images begin where a page does and cost
; no reads at all — see 0221.

.driver open  cartOpen
.driver read  cartRead
.driver seek  forward

.transform copy cartCopy

.macro cartOpen
        jsr cartOpenStream
.endm

.macro cartRead
        jsr cartReadByte
.endm

; Where the stream stands, which has to survive between one call and the next
; while nothing here is running — a plain Section and not Temporaries, as the
; diskette's is.
.section zeropage
cartPage        .res 1                  ; the page the shift register holds
cartPos         .res 2                  ; how far into it the counter has walked
cartWant        .res 2                  ; the offset `open` was asked for
cartWantPage    .res 1
.ends

; A = the unit, X the offset's low byte and Y its high one.
.proc cartOpenStream
        clc
        adc #1                          ; the model's unit i is page i + 1
        sta cartWantPage
        stx cartWant
        sty cartWant+1
        cmp cartPage
        bne @again

        ; The same page: read on from where the stream stands, but only where the
        ; offset is ahead of it. A counter that only counts cannot go back.
        lda cartWant+1
        cmp cartPos+1
        bcc @again
        bne @skip
        lda cartWant
        cmp cartPos
        bcc @again
@skip
        lda cartPos+1
        cmp cartWant+1
        bne @more
        lda cartPos
        cmp cartWant
        beq @done
@more
        jsr cartReadByte
        jmp @skip
@done
        rts

@again
        lda cartWantPage
        jsr cartShift
        jmp @skip
.endp

; A = the next byte, and the stream moves on. At the page's last byte the next
; page goes in, so that an image running from one unit into the next is read
; without anybody asking.
.proc cartReadByte
        lda $FCB2                       ; Suzy's port, through the CART0 strobe
        inc cartPos
        bne @within
        inc cartPos+1
@within
        ldx cartPos+1
        cpx #>LYNX_PAGE_SIZE
        bne @out
        ldx cartPos
        cpx #<LYNX_PAGE_SIZE
        bne @out
        pha
        lda cartPage
        inc
        jsr cartShift
        pla
@out
        rts
.endp

; A = the page, which goes into the address register most significant bit first;
; the routine that does that is the machine's own, at the ROM's first byte, and
; what it costs us is three bytes — see 0219. The counter is released at the
; page's first byte, which is where the stream then stands.
.proc cartShift
        sta cartPage
        jsr romSetPage
        stz cartPos
        stz cartPos+1
        rts
.endp

; The stored form of a Section that nothing compressed: its size in two bytes and
; then the bytes. One at a time through the stream, because what this costs is the
; reads underneath it — some fifteen ticks each, and a page of them either way.
cartDst  .ztemp 2
cartLeft .ztemp 2

.proc cartCopy
        stx cartDst
        sty cartDst+1
        jsr cartReadByte
        sta cartLeft
        jsr cartReadByte
        sta cartLeft+1
@byte
        lda cartLeft
        ora cartLeft+1
        beq @done
        jsr cartReadByte
        ldy #0
        sta (cartDst),y
        inc cartDst
        bne @counted
        inc cartDst+1
@counted
        lda cartLeft
        bne @low
        dec cartLeft+1
@low
        dec cartLeft
        jmp @byte
@done
        rts
.endp
