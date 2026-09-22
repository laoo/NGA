; The storage driver for extended memory behind PORTB: the units are the
; Banks of the variant's `extension`, which a value written to PORTB brings
; into the $4000-$7FFF window — `ext` in the variant — and the stream is a
; pointer into that window that walks on to the next unit at its end. Each
; role is a macro declared with `.driver`, which the routine, the loader's
; glue, the decoders and the tool's own Procs expand where they use it as
; `nga.open`, `nga.read`, `nga.show` and `nga.showAt`. What the hardware
; calls each state of a Window is this Module's alone: `portbValues` holds
; a PORTB value per state of `ext` — base RAM first, then one per Bank, as
; many as the variant's `extension` counts — and the OS Window is bit 0.
; `PORTB` is the variant's register. See docs/spec/transition.md and
; docs/decisions/0053-a-window-names-its-units.md.
;
; Resident, and outside the window — the variant lists this Module in
; `resident`, and the tool holds it outside the window the stream reads
; through.

portbWindow    = $4000
portbWindowEnd = portbWindow + $4000    ; one past the window: the high byte of the first address outside it
portbUnitPages = $40                    ; a unit, in pages: what an offset carries by
portbBase      = $FF                    ; base RAM in, the OS ROM in, BASIC and the self-test out

.driver open    portbOpen
.driver read    portbRead
.driver stream  ext
.driver show    ext portbShow
.driver showAt  ext portbShowAt
.driver show    os  portbShowOs
.driver showAt  os  portbShowOsAt

.transform copy portbCopy

.macro portbOpen
        jsr portbOpenStream
.endm

.macro portbRead
        jsr portbReadByte
.endm

; The state to show, as the tool numbers ext's states: 0 is base RAM, and
; Bank n is n + 1.
.macro portbShow state
        ldx #state
        jsr portbSelect
.endm

; X = the state to show.
.macro portbShowAt
        jsr portbSelect
.endm

; The OS Window is bit 0 of PORTB: set for the ROM, state 0, and clear for
; the RAM beneath it, state 1. The bank bits are left as they are.
.macro portbShowOs state
        lda PORTB
        and #$FE
        ora #1 - state
        sta PORTB
.endm

; X = the state to show, 0 or 1.
.macro portbShowOsAt
        lda PORTB
        ora #1
        cpx #0
        beq @store
        and #$FE
@store
        sta PORTB
.endm

; PORTB per state of ext: base RAM, then one value per Bank. The list runs
; to the 130XE's four and the variant's count trims it, so that a variant
; with fewer Banks takes a prefix; one with more lists its values here.
; Every value has bit 0 set — the OS ROM in — which is what lets
; portbSelect keep the OS Window's bit with one `and` and no scratch byte.
.macro portbBankValues n, values...
.if n > 0
.match values
.case first, rest...
        .byte first
        portbBankValues n - 1, rest...
.endmatch
.endif
.endm

; The table portbSelect indexes, in a Section of its own: what has an
; address stands in one, and a Proc holds code alone.
.section
portbValues
        .byte portbBase
        portbBankValues extension, $E3, $E7, $EB, $EF
.ends

; The stream: where the next byte is, and which unit is in. A plain zero-page
; Section rather than Temporaries, because the value has to survive between
; one call and the next while nothing here is running.
.section zeropage
portbPtr        .res 2
portbUnit       .res 1
.ends

; X = the state of ext to show: its PORTB value, with the OS bit left as it
; is, since the OS Window is the same byte. Writes no memory, so that a
; loader running it leaves nothing behind but the register.
.proc portbSelect
        lda PORTB
        ora #$FE
        and portbValues,x
        sta PORTB
        rts
.endp

; A = the unit, X/Y = the offset in it: the stream stands there. An offset
; past the unit's end carries into the units after, since the routine
; counts on in a Frame without knowing where a unit ends: a unit is $4000
; bytes, so the offset's top two bits are units.
.proc portbOpenStream
        sta portbUnit
        stx portbPtr
        tya
@carry
        cmp #portbUnitPages
        bcc @within
        sbc #portbUnitPages
        inc portbUnit
        jmp @carry
@within
        clc
        adc #>portbWindow
        sta portbPtr+1
        ldx portbUnit
        inx                             ; Bank n is state n + 1
        jsr portbSelect
        rts
.endp

; A = the next byte of the stream. X and Y are not preserved.
.proc portbReadByte
        ldy #0
        lda (portbPtr),y
        inc portbPtr
        bne @done
        inc portbPtr+1
        ldy portbPtr+1
        cpy #>portbWindowEnd
        bne @done
        pha
        jsr portbNextUnit
        pla
@done
        rts
.endp

; The window's end: the next unit in, and the pointer back at its start.
.proc portbNextUnit
        inc portbUnit
        ldx portbUnit
        inx
        jsr portbSelect
        lda #0
        sta portbPtr
        lda #>portbWindow
        sta portbPtr+1
        rts
.endp

;  The decoder of `copy`: X/Y = the destination, the stream at the stored
; size and then the bytes. Reads the window through the pointer rather than
; through portbReadByte, which is what a driver's own decoder is for: a run at a
; time, where a run ends at the source's page end or at the size, so that
; the inner loop is `(zp),y` down to zero — the window's end is a page end,
; so a unit is never crossed inside a run.
portbDst  .ztemp 2
portbSize .ztemp 2
portbRun  .ztemp 1                      ; bytes in the run, zero for 256

.proc portbCopy
        stx portbDst
        sty portbDst+1
        jsr portbReadByte
        sta portbSize
        jsr portbReadByte
        sta portbSize+1
@run
        lda portbSize
        ora portbSize+1
        beq @done
        lda portbPtr                    ; to the end of the source's page
        eor #$FF
        clc
        adc #1
        sta portbRun
        lda portbSize+1
        bne @copy                       ; at least a page left: the run stands
        lda portbRun
        beq @cap                        ; a whole page, and less than one left
        cmp portbSize
        bcc @copy
@cap
        lda portbSize
        sta portbRun
@copy
        ldy portbRun
@byte
        dey
        lda (portbPtr),y
        sta (portbDst),y
        tya
        bne @byte
        ldx portbRun                    ; the run, as 256 where it is zero
        bne @counted
        inc portbPtr+1
        inc portbDst+1
        dec portbSize+1
        jmp @crossed
@counted
        txa
        clc
        adc portbPtr
        sta portbPtr
        bcc @source
        inc portbPtr+1
@source
        txa
        clc
        adc portbDst
        sta portbDst
        bcc @destination
        inc portbDst+1
@destination
        sec
        lda portbSize
        stx portbRun
        sbc portbRun
        sta portbSize
        bcs @crossed
        dec portbSize+1
@crossed
        lda portbPtr+1
        cmp #>portbWindowEnd
        bne @run
        jsr portbNextUnit               ; the window's end: the next unit in
        jmp @run
@done
        rts
.endp
