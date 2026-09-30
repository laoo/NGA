; The storage driver for a Commodore diskette, which the program reads over the
; IEEE-488 bus itself: a unit of storage is a **track** and the offset's high
; byte is the sector in it, so the three bytes the model addresses storage with
; are the two numbers `U1` takes and this driver does no arithmetic on them —
; the same identity atari/disk.asm has, reached from the other side. See
; docs/decisions/0225-a-unit-of-a-commodore-diskette-is-a-track.md.
;
; It says `.driver span none`, so no image and no Frame ever runs from one track
; into the next, and it therefore needs to know **nothing** of the surface: not
; which tracks are long and which are short, not where the directory is, and not
; which track follows which. A sector's successor is the next sector of the same
; track, always, and the tool is held to that.
;
; The medium maps nothing, so this names no Window: `stream`, `show` and
; `showAt` are not declared and the two Procs that put base memory back after a
; Transition are a `rts`.
;
; Resident, since every Transition calls it; the geometry document lists it so.
;
; Four things about this ROM that cost an hour each if taken on trust:
;
;   * `OPEN` and `CLOSE` in the jump table begin with the BASIC parameter
;     parser, which reads its arguments out of BASIC text and clears $D1 and
;     $D3. Machine code sets the zero page itself and enters past it, at the
;     addresses pet/kernal.asm names — and those are specific to this ROM.
;   * **Success is not in the carry.** `OPEN` reports it by leaving the count of
;     open files at $AE one higher than it was, and nothing else.
;   * `CHKIN` does not normalise the carry either: its common exit is
;     PLA/TAY/PLA/TAX/PLA/RTS. A real failure goes to the BASIC error handler
;     and never comes back, so there is nothing to branch on.
;   * **NRFD must be driven low on every byte.** NODISKEMU waits for it after it
;     asserts DAV, so a listener that leaves NRFD released hangs an SD2PET.

.driver open  petOpen
.driver read  petRead
.driver span  none

.transform copy petCopy

.macro petOpen
        jsr petOpenStream
.endm

.macro petRead
        jsr petReadByte
.endm

petDevice  = 8
petCommand = 15                         ; the channel a `U1` is sent on
petBuffer  = 5                          ; and the one the sector arrives on

; Where the stream stands. A plain Section and not Temporaries, because it has
; to survive between one call and the next while nothing here is running.
.section zeropage
petTrack        .res 1
petSector       .res 1
petPos          .res 1                  ; bytes taken from this sector
petNeed         .res 1                  ; non-zero when the sector is used up
petOpened       .res 1                  ; non-zero once the two channels are open
.ends

; A = the unit, which is the track; X the offset's low byte, which is the
; position in the sector; Y its high byte, which is the sector. Nothing crosses
; a unit, so none of the three needs adding to anything.
.proc petOpenStream
        sta petTrack
        sty petSector
        stx petPos
        jsr petFill
        ldx petPos
        beq @ready
@skip
        txa
        pha
        jsr petPull                     ; to an offset by reading up to it: the
        pla                             ; drive holds the sector, we take bytes
        tax
        dex
        bne @skip
@ready
        rts
.endp

; A = the next byte, and the stream moves on. X and Y are not preserved.
.proc petReadByte
        lda petNeed
        beq @have
        inc petSector                   ; the next sector of the same track,
        jsr petFill                     ; which is where a stream carries to
        lda #0
        sta petPos
@have
        jsr petPull
        inc petPos
        bne @within
        ; 256 taken: the drive has no more of this sector. The flag is set from
        ; a constant and not from A, which holds the byte just read — a byte
        ; that happened to be zero would say the sector was not used up, and the
        ; next read would wait for a drive that has nothing left to send.
        pha
        lda #$FF
        sta petNeed
        pla
@within
        rts
.endp

; The sector in petTrack/petSector into the drive's buffer, and the bus left
; talking to us. `U1:5 0 <track> <sector>`, which is the shortest the DOS will
; take — every character is some 375 cycles against 58 for a byte of data.
.proc petFill
        lda #0
        sta petNeed                     ; this sector is whole again
        lda petOpened
        bne @ask
        jsr petOpenChannels
@ask
        ldx #petCommand
        jsr CHKOUT
        ldx #0
@text
        lda petU1,x
        beq @track
        jsr CHROUT
        inx
        bne @text
@track
        lda petTrack
        jsr petDecimal
        lda #$20
        jsr CHROUT
        lda petSector
        jsr petDecimal
        lda #13
        jsr CHROUT
        jsr CLRCHN                      ; unlisten: the drive now executes it
        ldx #petBuffer
        jsr CHKIN
        rts
.endp

; A, as decimal digits, on the channel that is open for output. No leading
; zero, since a shorter command is a cheaper one — every character of it is
; some 375 cycles against 58 for a byte of data.
petDigits  .ztemp 1                     ; what is left of the value
petHundred .ztemp 1
petTen     .ztemp 1

.proc petDecimal
        sta petDigits
        ldx #0
@hundreds
        lda petDigits
        cmp #100
        bcc @hundredsDone
        sbc #100
        sta petDigits
        inx
        bne @hundreds
@hundredsDone
        stx petHundred
        ldx #0
@tens
        lda petDigits
        cmp #10
        bcc @tensDone
        sbc #10
        sta petDigits
        inx
        bne @tens
@tensDone
        stx petTen

        lda petHundred
        beq @noHundreds
        ora #$30
        jsr CHROUT
        lda petTen                      ; a hundreds digit forces a tens digit
        ora #$30
        jsr CHROUT
        jmp @units
@noHundreds
        lda petTen
        beq @units
        ora #$30
        jsr CHROUT
@units
        lda petDigits
        ora #$30
        jsr CHROUT
        rts
.endp

; One byte off the bus, by the handshake the ROM does at $F18C with everything
; optional taken out: no timer armed as a timeout, no timeout test inside the
; wait, no EOI test and no channel dispatch. It rests on CHKIN having put the
; bus in TALK for our channel, which petFill did.
;
; VIA port B: bit 7 is DAV in, low while a byte is valid, and bit 1 is NRFD out.
; PIA 2's port A is the data, **inverted**, and bits 5 to 3 of its control
; register drive CA2, which is NDAC: $34 holds it low and $3C releases it.
.proc petPull
        lda #$34
        sta PIA2CRA                     ; NDAC low: ready to accept
        lda VIAPB
        ora #$02
        sta VIAPB                       ; release NRFD: the talker may send
@wait
        bit VIAPB
        bmi @wait                       ; until DAV goes low
        lda VIAPB
        and #$FD
        sta VIAPB                       ; NRFD low: hold the talker off
        lda PIA2PA
        eor #$FF                        ; the bus is active low
        pha
        lda #$3C
        sta PIA2CRA                     ; NDAC high: the byte is accepted
@released
        bit VIAPB
        bpl @released                   ; until DAV is released
        lda #$34
        sta PIA2CRA                     ; NDAC low: ready for the next
        pla
        rts
.endp

; The command channel and a direct-access buffer, opened once and never closed.
; **One data channel at a time** is the rule this keeps: only the buffer is ever
; read from, and the command channel is written to and left.
.proc petOpenChannels
        lda #0
        sta FNLEN                       ; a command channel has no name
        lda #petCommand
        sta LA
        sta SA
        lda #petDevice
        sta FA
        jsr KERNAL_OPEN

        lda #1
        sta FNLEN
        lda #<petHash
        sta FNADR
        lda #>petHash
        sta FNADR+1
        lda #petBuffer
        sta LA
        sta SA
        lda #petDevice
        sta FA
        jsr KERNAL_OPEN

        lda #1
        sta petOpened
        rts
.endp

.section absolute
petHash
        .byte petscii"#"
petU1
        .byte petscii"U1:5 0 ", 0
.ends

; The decoder of `copy`: X/Y = the destination, the stream at the stored size
; and then the bytes. One at a time through the stream, because what this costs
; is the handshake under it — the bus is interlocked and the PET is the slower
; of the two, so there is no run to be had by copying differently.
petDst  .ztemp 2
petLeft .ztemp 2

.proc petCopy
        stx petDst
        sty petDst+1
        jsr petReadByte
        sta petLeft
        jsr petReadByte
        sta petLeft+1
@byte
        lda petLeft
        ora petLeft+1
        beq @done
        jsr petReadByte
        ldy #0
        sta (petDst),y
        inc petDst
        bne @counted
        inc petDst+1
@counted
        lda petLeft
        bne @low
        dec petLeft+1
@low
        dec petLeft
        jmp @byte
@done
        rts
.endp
