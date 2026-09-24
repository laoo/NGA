; The decoder of `zx0` over any driver's stream: Einar Saukas's ZX0, version
; 2's standard forward stream, decoded as the reference dzx0.c does, driven
; by the stream's own end marker. Written here from the format, not ported:
; nothing of the reference's text is in it, and it is licensed as everything
; under lib/ is, see lib/LICENSE. Entered with X/Y = the destination and the
; stream at the stored size, which it reads past. A match is copied a byte
; at a time, forward, from what was written, which is what makes an offset
; shorter than its length — the run — come out right. Resident, and outside
; the driver's window.
;
;   zx0Bits      the bit buffer, a sentinel one above the bits still unread
;   zx0Offset    the last offset
;   zx0Length    the length in hand, or an offset's MSB while one is read
;   zx0Invert    one while an offset's MSB is read, whose data bits the
;                stream carries complemented; zero otherwise

.transform zx0 zx0Decode

zx0Dst    .ztemp 2
zx0Src    .ztemp 2
zx0Bits   .ztemp 1
zx0Offset .ztemp 2
zx0Length .ztemp 2
zx0Invert .ztemp 1

; The next bit of the stream, in A as zero or one and in the Z flag. The
; buffer holds a sentinel above the unread bits, so shifting it to nothing is
; the signal to fetch the next byte and put the sentinel back below it.
.proc zx0Bit
        asl zx0Bits
        bne @have
        nga.read
        rol                     ; the carry the asl left is the sentinel
        sta zx0Bits
@have
        lda #0
        rol
        rts
.endp

; An interlaced Elias gamma value into zx0Length: a control bit says whether
; a data bit follows, and the value begins at one. Three Procs chained by
; `then` because the value after an offset is entered with its first control
; bit already read, at zx0EliasData: the first falls through into the
; second, and the second branches into the third, which the chain is what
; allows.
.proc zx0Elias
        lda #1
        sta zx0Length
        lda #0
        sta zx0Length+1
.endp then zx0EliasMore
.proc zx0EliasMore
        jsr zx0Bit
        beq zx0EliasData        ; zero: a data bit follows
        rts
.endp then zx0EliasData
.proc zx0EliasData
        jsr zx0Bit
        eor zx0Invert
        lsr
        rol zx0Length
        rol zx0Length+1
        jmp zx0EliasMore
.endp

; One byte written: the destination moves on and the length in hand comes
; down, leaving Z set when it reaches zero.
.proc zx0Step
        inc zx0Dst
        bne @moved
        inc zx0Dst+1
@moved
        lda zx0Length
        bne @low
        dec zx0Length+1
@low
        dec zx0Length
        lda zx0Length
        ora zx0Length+1
        rts
.endp

; The length in hand copied from zx0Dst less the last offset to zx0Dst.
.proc zx0Copy
        sec
        lda zx0Dst
        sbc zx0Offset
        sta zx0Src
        lda zx0Dst+1
        sbc zx0Offset+1
        sta zx0Src+1
@byte
        ldy #0
        lda (zx0Src),y
        sta (zx0Dst),y
        inc zx0Src
        bne @from
        inc zx0Src+1
@from
        jsr zx0Step
        bne @byte
        rts
.endp

.proc zx0Decode
        stx zx0Dst
        sty zx0Dst+1
        nga.read             ; the stored size, which the end marker makes unnecessary
        nga.read
        lda #$80
        sta zx0Bits             ; an empty buffer: the sentinel alone
        lda #1
        sta zx0Offset
        lda #0
        sta zx0Offset+1         ; the last offset begins at one
        sta zx0Invert
@literals
        jsr zx0Elias
@literal
        nga.read
        ldy #0
        sta (zx0Dst),y
        jsr zx0Step
        bne @literal
        jsr zx0Bit
        bne @offset
        jsr zx0Elias            ; a match at the last offset
        jsr zx0Copy
        jsr zx0Bit
        beq @literals
@offset
        lda #1
        sta zx0Invert
        jsr zx0Elias            ; the new offset's MSB, complemented in the stream
        lda #0
        sta zx0Invert
        lda zx0Length
        beq @done               ; 256 is the end marker, and the one value with a low byte of zero
        lsr                     ; offset = MSB * 128 - LSB / 2
        sta zx0Offset+1
        lda #0
        ror
        sta zx0Offset
        nga.read
        lsr                     ; the LSB's low bit is the length's first control bit
        php
        eor #$FF
        sec
        adc zx0Offset
        sta zx0Offset
        bcs @subtracted
        dec zx0Offset+1
@subtracted
        plp
        lda #1
        sta zx0Length
        lda #0
        sta zx0Length+1
        bcs @counted            ; a control bit of one: the value is one
        jsr zx0EliasData
@counted
        inc zx0Length           ; a match at a new offset is one longer than written
        bne @copy
        inc zx0Length+1
@copy
        jsr zx0Copy
        jsr zx0Bit
        bne @offset
        jmp @literals
@done
        rts
.endp
