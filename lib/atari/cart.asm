; The storage driver for a XEGS cartridge: the units are the Banks of the
; variant's `banks`, which the **byte written to `$D5FF`** brings into the
; $8000-$9FFF window — `switched` in the variant — and the stream is a pointer
; into that window that walks on to the next unit at its end. Each role is a
; macro declared with `.driver`, which the routine, the decoders and the tool's
; own Procs expand where they use it as `nga.open`, `nga.read`, `nga.show` and
; `nga.showAt`.
;
; A state of `switched` **is** a Bank number, so this driver holds no table
; where `portb.asm` holds one: the window has no named state to number before
; the Banks, since a cartridge of this kind cannot be switched off, and the
; board wires the byte written straight to the address lines above the window.
; Which is also why `show` is two instructions and nothing else.
;
; The register is written and never read — the board decodes an address and
; latches the data, and gives nothing back — so the bank a Transition left in
; is in this driver's own byte and nowhere else.
;
; Resident, and outside the window: the variant lists this Module in `resident`,
; and the tool holds it outside the window the stream reads through, since code
; that switched the window from inside it would switch itself away.

cartSelect     = $D5FF                  ; any address of $D500-$D5FF; the data is the bank
cartWindow     = $8000
cartWindowEnd  = cartWindow + $2000     ; one past the window: the high byte of the first address outside it
cartUnitPages  = $20                    ; a unit, in pages: what an offset carries by

.driver open    cartOpen
.driver read    cartRead
.driver stream  switched
.driver show    switched cartShow
.driver showAt  switched cartShowAt

.transform copy cartCopy

.macro cartOpen
        jsr cartOpenStream
.endm

.macro cartRead
        jsr cartReadByte
.endm

; The state to show, which is the Bank: the window shows nothing else.
.macro cartShow state
        lda #state
        sta cartSelect
.endm

; X = the state to show.
.macro cartShowAt
        stx cartSelect
.endm

; The stream: where the next byte is, and which unit is in. A plain zero-page
; Section rather than Temporaries, because the value has to survive between one
; call and the next while nothing here is running.
.section zeropage
cartPtr         .res 2
cartUnit        .res 1
.ends

; A = the unit, X/Y = the offset in it: the stream stands there. An offset past
; the unit's end carries into the units after, since the routine counts on in a
; Frame without knowing where a unit ends: a unit is $2000 bytes, so the
; offset's top three bits are units.
.proc cartOpenStream
        sta cartUnit
        stx cartPtr
        tya
@carry
        cmp #cartUnitPages
        bcc @within
        sbc #cartUnitPages
        inc cartUnit
        jmp @carry
@within
        clc
        adc #>cartWindow
        sta cartPtr+1
        lda cartUnit
        sta cartSelect
        rts
.endp

; A = the next byte of the stream. X and Y are not preserved.
.proc cartReadByte
        ldy #0
        lda (cartPtr),y
        inc cartPtr
        bne @done
        inc cartPtr+1
        ldy cartPtr+1
        cpy #>cartWindowEnd
        bne @done
        pha
        jsr cartNextUnit
        pla
@done
        rts
.endp

; The window's end: the next unit in, and the pointer back at its start.
.proc cartNextUnit
        inc cartUnit
        lda cartUnit
        sta cartSelect
        lda #0
        sta cartPtr
        lda #>cartWindow
        sta cartPtr+1
        rts
.endp

; The decoder of `copy`: X/Y = the destination, the stream at the stored size
; and then the bytes. Reads the window through the pointer rather than through
; cartReadByte, which is what a driver's own decoder is for: a run at a time,
; where a run ends at the source's page end or at the size, so that the inner
; loop is `(zp),y` down to zero — the window's end is a page end, so a unit is
; never crossed inside a run.
cartDst  .ztemp 2
cartSize .ztemp 2
cartRun  .ztemp 1                       ; bytes in the run, zero for 256

.proc cartCopy
        stx cartDst
        sty cartDst+1
        jsr cartReadByte
        sta cartSize
        jsr cartReadByte
        sta cartSize+1
@run
        lda cartSize
        ora cartSize+1
        beq @done
        lda cartPtr                     ; to the end of the source's page
        eor #$FF
        clc
        adc #1
        sta cartRun
        lda cartSize+1
        bne @copy                       ; at least a page left: the run stands
        lda cartRun
        beq @cap                        ; a whole page, and less than one left
        cmp cartSize
        bcc @copy
@cap
        lda cartSize
        sta cartRun
@copy
        ldy cartRun
@byte
        dey
        lda (cartPtr),y
        sta (cartDst),y
        tya
        bne @byte
        ldx cartRun                     ; the run, as 256 where it is zero
        bne @counted
        inc cartPtr+1
        inc cartDst+1
        dec cartSize+1
        jmp @crossed
@counted
        txa
        clc
        adc cartPtr
        sta cartPtr
        bcc @source
        inc cartPtr+1
@source
        txa
        clc
        adc cartDst
        sta cartDst
        bcc @destination
        inc cartDst+1
@destination
        sec
        lda cartSize
        stx cartRun
        sbc cartRun
        sta cartSize
        bcs @crossed
        dec cartSize+1
@crossed
        lda cartPtr+1
        cmp #>cartWindowEnd
        bne @run
        jsr cartNextUnit                ; the window's end: the next unit in
        jmp @run
@done
        rts
.endp
