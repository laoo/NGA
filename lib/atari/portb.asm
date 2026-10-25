; The storage driver for extended memory behind PORTB: the units are the
; Banks of the variant's `extension`, which a value written to PORTB brings
; into the $4000-$7FFF window — `ext` in the variant — and the stream is a
; pointer into that window that walks on to the next unit at its end. Each
; role is a macro declared with `.driver`, which the routine, the loader's
; glue, the decoders and the tool's own Procs expand where they use it as
; `nga.open`, `nga.read`, `nga.show` and `nga.showAt`. What the hardware
; calls each state of a Window is this Module's alone: `portbValues` holds
; a PORTB value per state of `ext`, one per Bank, as many as the variant's
; `extension` counts — the last of the 130XE's five being the machine's own
; memory, which no Window keeps as a base — see 0229. `PORTB` is the variant's
; register.
;
; **The same register holds the OS ROM and BASIC**, bits 0 and 1, so this
; driver is also what switches them: `.driver rom` names the Modules that are
; those ROMs, and `select` is handed the set a Phase needs and writes the two
; bits, which every switch of a Bank keeps as they are — see 0229.
;
; Resident, and outside the window — the variant lists this Module in
; `resident`, and the tool holds it outside the window the stream reads
; through.

portbWindow    = $4000
portbWindowEnd = portbWindow + $4000    ; one past the window: the high byte of the first address outside it
portbUnitPages = $40                    ; a unit, in pages: what an offset carries by

.driver open    portbOpen
.driver read    portbRead
.driver unit    portbUnit
.driver offset  portbPtr
.driver stream  ext
.driver show    ext portbShow
.driver showAt  ext portbShowAt
.driver init    portbInit
.driver finalize portbFinalize
.driver rom     osrom                   ; bit 0 of the set
.driver rom     mathpack osrom          ; bit 1, which one bit of PORTB shows with the OS
.driver rom     basic                   ; bit 2
.driver select  portbRoms
.driver mask    portbMask
.driver unmask  portbUnmask

.transform copy portbCopy

; PORTB as the program found it, kept by `init` and put back by `finalize`: the
; bits of BASIC and of the self-test are the user's, whatever the program does
; with them meanwhile, and a DOS returned to expects its machine as it left it
; — see docs/decisions/0229-no-window-has-a-base.md.
.macro portbInit
        lda PORTB
        sta portbFound
.endm

.macro portbFinalize
        lda portbFound
        sta PORTB
.endm

.section
portbFound      .res 1
.ends

; What the load wraps a switch of every ROM out in, while the DOS's interrupts
; are still on: with the OS out, $FFFA is RAM the load is writing. IRQ masked,
; and NMI, which `sei` does not reach, off at ANTIC; then back as the OS keeps
; it, the vertical blank on — see
; docs/decisions/0241-bytes-kept-for-a-later-phase-may-lie-hidden-under-a-rom.md.
NMIEN = $D40E

.macro portbMask
        sei
        lda #0
        sta NMIEN
.endm

.macro portbUnmask
        lda #$40
        sta NMIEN
        cli
.endm

.macro portbOpen
        jsr portbOpenStream
.endm

.macro portbRead
        jsr portbReadByte
.endm

; The state to show, as the tool numbers ext's states: Bank n is n.
.macro portbShow state
        ldx #state
        jsr portbBank
.endm

; X = the state to show.
.macro portbShowAt
        jsr portbBank
.endm

; A = the set of ROMs the Phase needs.
.macro portbRoms
        jsr portbSelectRoms
.endm

; PORTB per state of ext, one value per Bank. The list runs to the 130XE's
; five — four of extended memory, and then $FF, the machine's own with the
; self-test out — and the variant's count trims it, so that a variant with
; fewer Banks takes a prefix; one with more lists its values here. Every value
; has bits 0 and 1 set, which is what lets portbBank keep the ROMs' bits as
; they are with one `and` and no scratch byte.
.macro portbBankValues n, values...
.if n > 0
.match values
.case first, rest...
        .byte first
        portbBankValues n - 1, rest...
.endmatch
.endif
.endm

; The table portbBank indexes, in a Section of its own: what has an
; address stands in one, and a Proc holds code alone.
.section
portbValues
        portbBankValues extension, $E3, $E7, $EB, $EF, $FF
.ends

; The stream: where the next byte is, and which unit is in. A plain zero-page
; Section rather than Temporaries, because the value has to survive between
; one call and the next while nothing here is running. They are also the
; cells the routine writes a position into before it opens the stream,
; `portbPtr` the offset and `portbUnit` the unit.
.section zeropage
portbPtr        .res 2
portbUnit       .res 1
.ends

; X = the state of ext to show: its PORTB value, with the bits of the OS ROM
; and of BASIC left as they are, since those are what `select` said. Writes no
; memory, so that a loader running it leaves nothing behind but the register.
.proc portbBank
        lda PORTB
        ora #$FC
        and portbValues,x
        sta PORTB
        rts
.endp

; A = the set of ROMs the Phase entered needs: bit 0 `osrom`, bit 1 `mathpack`,
; bit 2 `basic`. Bit 0 of PORTB is set for the OS ROM and the floating-point
; pack, which one bit shows together, and bit 1 is clear for BASIC; the bank
; bits are left as they are. Writes no memory, the set waiting on the stack
; while the register is worked out.
.proc portbSelectRoms
        pha
        lda PORTB
        ora #$03                        ; the OS in and BASIC out, to begin with
        tax
        pla
        pha
        and #$03                        ; neither osrom nor mathpack: the OS out
        bne @os
        txa
        and #$FE
        tax
@os
        pla
        and #$04                        ; basic: BASIC in
        beq @basic
        txa
        and #$FD
        tax
@basic
        stx PORTB
        rts
.endp

; The stream to the position in portbUnit and portbPtr. An offset past the
; unit's end carries into the units after, since the routine counts on in a
; Frame without knowing where a unit ends: a unit is $4000 bytes, so the
; offset's top two bits are units.
.proc portbOpenStream
        lda portbPtr+1
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
        jsr portbBank
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
        jsr portbBank
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
