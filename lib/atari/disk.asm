; The storage driver for a diskette: a unit of storage is 256 sectors of 256
; bytes, and unit zero begins at sector 4, the first after the boot record.
; That size is what makes the three bytes the model addresses storage with —
; a unit and a two-byte offset — the address the hardware takes, so this driver
; does no arithmetic: the unit is the sector number's high byte, the offset's
; high byte is its low one, and the offset's low byte is the position in the
; sector. Reading a sector is the OS's own disk handler, called through
; `DSKINV` with the request in the device control block, which is why this
; driver needs no register of its own and knows nothing about a drive.
;
; The medium maps nothing, so this driver names no Window: `stream`, `show`
; and `showAt` are not declared, and the two Procs that put base memory back
; after a Transition are a `rts`.
;
; Resident, since every Transition calls it; the variant lists it so.

DSKINV = $E453                  ; the OS's disk handler: one sector per call
DUNIT  = $0301                  ; the drive, 1 for D1:
DCOMND = $0302                  ; 'R' reads
DBUFLO = $0304
DBUFHI = $0305
DAUX1  = $030A                  ; the sector, two bytes
DAUX2  = $030B

diskFirst = 4                   ; the sector unit zero begins at

.driver open diskOpen
.driver read diskRead

.transform copy diskCopy

.macro diskOpen
        jsr diskOpenStream
.endm

.macro diskRead
        jsr diskReadByte
.endm

; The stream: which sector the buffer holds, where in it the next byte is, and
; whether the position has run off its end. A plain Section rather than
; Temporaries, because the value has to survive between one call and the next
; while nothing here is running.
.section zeropage
diskAt          .res 2
diskPos         .res 1
diskNeed        .res 1
.ends

; The sector the stream reads through. A Section of reservations alone, so it
; is in no Container and no load writes it. `within 256` keeps it off a page
; boundary: every byte of it is read as `diskBuffer,y`, and an index that
; carries into the next page costs a cycle.
.section absolute within 256
diskBuffer
        .res 256
.ends

; A = the unit, X/Y = the offset in it: the stream stands there. An offset past
; the unit's end carries into the units after, since the routine counts on in a
; Frame without knowing where a unit ends — and here it carries by itself, the
; three bytes being one position that the sector number takes the top two of.
.proc diskOpenStream
        stx diskPos
        sta diskAt+1            ; the unit is the sector number's high byte
        tya                     ; and the offset's high byte is its low one
        clc
        adc #<diskFirst
        sta diskAt
        lda diskAt+1
        adc #>diskFirst
        sta diskAt+1
        jmp diskFill
.endp

; A = the next byte of the stream. X and Y are not preserved.
.proc diskReadByte
        lda diskNeed
        beq @have
        inc diskAt
        bne @next
        inc diskAt+1
@next
        jsr diskFill
@have
        ldy diskPos
        inc diskPos
        bne @within
        sty diskNeed            ; Y is $FF here, and any non-zero will do
@within
        lda diskBuffer,y
        rts
.endp

; The sector in diskAt into the buffer, asked for again until the handler says
; it has it: a Transition that cannot read cannot go on, and the drive answered
; when the OS read the boot record off this diskette.
.proc diskFill
        lda #0
        sta diskNeed
        lda diskAt
        sta DAUX1
        lda diskAt+1
        sta DAUX2
@again
        lda #1
        sta DUNIT
        lda #$52
        sta DCOMND
        lda #<diskBuffer
        sta DBUFLO
        lda #>diskBuffer
        sta DBUFHI
        jsr DSKINV
        bmi @again
        rts
.endp

; The decoder of `copy`: X/Y = the destination, the stream at the stored size
; and then the bytes. A byte at a time through the stream, because what this
; costs is the sector reads underneath it — a run copied out of the buffer
; would save a few hundred cycles against the tens of thousands a sector takes
; to arrive.
diskDst  .ztemp 2
diskLeft .ztemp 2

.proc diskCopy
        stx diskDst
        sty diskDst+1
        jsr diskReadByte
        sta diskLeft
        jsr diskReadByte
        sta diskLeft+1
@byte
        lda diskLeft
        ora diskLeft+1
        beq @done
        jsr diskReadByte
        ldy #0
        sta (diskDst),y
        inc diskDst
        bne @counted
        inc diskDst+1
@counted
        lda diskLeft
        bne @low
        dec diskLeft+1
@low
        dec diskLeft
        jmp @byte
@done
        rts
.endp
