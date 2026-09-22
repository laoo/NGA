; The decoder of `rle` over any driver's stream: run-length packets, a
; control byte whose top bit says run or literal and whose low seven bits
; count, driven by the stored size — it stops when that many bytes have
; been read, and what it produces is as long as it turns out to be. Entered
; with X/Y = the destination and the stream at the stored size. Resident,
; and outside the driver's window. See docs/spec/transition.md.

.transform rle rleDecode

rleDst   .ztemp 2
rleSize  .ztemp 2
rleCount .ztemp 1
rleValue .ztemp 1

; The next byte of the stream, with the size coming down.
.proc rleFetch
        nga.read
        pha
        lda rleSize
        bne @low
        dec rleSize+1
@low
        dec rleSize
        pla
        rts
.endp

.proc rlePut
        ldy #0
        sta (rleDst),y
        inc rleDst
        bne @done
        inc rleDst+1
@done
        rts
.endp

.proc rleDecode
        stx rleDst
        sty rleDst+1
        nga.read
        sta rleSize
        nga.read
        sta rleSize+1
@packet
        lda rleSize
        ora rleSize+1
        beq @done
        jsr rleFetch
        tax                     ; the control byte, kept for its top bit
        and #$7F
        beq @packet             ; a count of nothing, which the encoder never writes
        sta rleCount
        txa
        bmi @run
@literal
        jsr rleFetch
        jsr rlePut
        dec rleCount
        bne @literal
        jmp @packet
@run
        jsr rleFetch
        sta rleValue
@repeat
        lda rleValue
        jsr rlePut
        dec rleCount
        bne @repeat
        jmp @packet
@done
        rts
.endp
