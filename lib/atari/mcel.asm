; The driver for a MariaCEL cartridge: the two RAMMAP windows, and the SDRAM
; behind the one at $8000 as storage. Each role is a macro declared with
; `.driver`, which the routine, the decoders and the tool's own Procs expand
; where they use it as `nga.init`, `nga.open`, `nga.read`, `nga.show` and
; `nga.showAt`.
;
; **The device is not on the bus until it is selected**, and that is what
; `init` is for: the cartridge sits on the parallel bus and answers at none of
; its addresses until `PDVREG` carries its identifier, so until then a write to
; RAMMAP is a write to nothing at all — no fault, and a picture that is simply
; black. The Container calls it once before it fills a unit, since filling one
; calls `showAt` before a byte of the program's own code has run — see
; 0227 and xex.md.
;
; A state of `cel8000` is a **Bank**, and the block the hardware wants is one
; more than it: block zero is what `RAMMAP = 0` means, the Atari's own RAM at
; those addresses, which this board does not reach, so the model's unit *i* is
; block *i + 1* — as the Lynx's unit *i* is page *i + 1* — and the 2047 Banks
; are blocks one to 2047.
;
; **A state of `cel8000` is two bytes**, the low one in X and the high one in
; Y, since the board's eleven bits of block are more than a byte counts:
; `showAt` says so with `xy`, and every caller passes both, whatever the
; variant's count. A unit is two bytes only where the variant has more than
; 256 Banks, because that is the tool's rule for a unit, so the unit's cell is
; as wide as `celUnitBytes` and what reads it says so under `.if` — see
; docs/decisions/0228-a-driver-names-the-cells-its-stream-stands-in.md.
;
; The states of `cel4000` are the blitter's five BRAM blocks and `main`, and
; they are a table rather than arithmetic: bit 7 of the high byte is what takes
; the window off SDRAM and onto the blitter, so the two bytes are not one
; number.
;
; Neither register is read back — the byte written to RAMMAP is a latch, and
; what `CELCTL` gives back on a read is `STATUS` and not what was written — so
; what the windows show is this driver's own business and nowhere else.
;
; Resident, and outside the window: the variant lists this Module in
; `resident`, and the tool holds it outside `cel8000`, since code that switched
; the window from inside it would switch itself away.

.export CELENABLE, CELNODETECT, CELLEVELS, CELSNAPCLR, CELOVRCLR, CELRESET
.export CELSNAP, CELOVERRUN
.export CELPALBANKR, CELPALBANKG, CELPALBANKB

; What `CELCTL` takes on a write. The **level** bits are CELENABLE and
; CELNODETECT, and CELLEVELS is the mask that keeps them: `STATUS` gives them
; back at the positions `CTL` takes them at, so a copy in RAM is unnecessary
; and the mask is not. Leaving it out is what makes `lda CELCTL : ora ... : sta
; CELCTL` reset the blitter in every frame where SNAP is set, which is nearly
; all of them, while looking like correct code.
;
; No read-modify-write instruction may be used on CELCTL — `inc`, `dec`, `asl`,
; `lsr`, `rol`, `ror` would read STATUS and write it back as CTL.
CELENABLE       = $01                   ; level: the blitter does nothing until this is set
CELNODETECT     = $02                   ; level: diagnostic
CELLEVELS       = $03                   ; the mask of the level bits, and it is obligatory
CELSNAPCLR      = $04                   ; strobe: hand the sprite list to the blitter
CELOVRCLR       = $08                   ; strobe: clear the sticky OVERRUN
CELRESET        = $80                   ; strobe

; What `CELCTL` gives back on a read, at the positions `bit` leaves in the
; flags: SNAP in N, OVERRUN in V.
CELSNAP         = $80                   ; the sprite list belongs to the 6502
CELOVERRUN      = $40                   ; a strip did not finish in its budget; sticky

; The banks of the page `PBIRAMBANK` brings in at $DF00, which carry the CEL
; palette one component each.
CELPALBANKR     = $19
CELPALBANKG     = $1A
CELPALBANKB     = $1B

; What this device answers to. A **bit and not a number**: the kernel's own
; scan walks $D1FF with `lda #$01` and `asl`, one bit a slot, so this is the
; fourth of them — and a write here therefore deselects every other device on
; the bus, which a program sharing it with one has to know.
celPbiId        = $08
celWindow       = $8000                 ; the window the stream reads through
celWindowEnd    = celWindow + $4000     ; one past it: the high byte of the first address outside
celUnitPages    = $40                   ; a unit, in pages: what an offset carries by
celUnitBytes    = 1 + ( sdram > 256 )   ; how wide a unit's number is, which the variant decides

.driver init    celInit
.driver finalize celFinalize
.driver rom     osrom                   ; bit 0 of the set: bit 0 of PORTB
.driver select  celRoms
.driver open    celOpen
.driver read    celRead
.driver unit    celUnit
.driver offset  celPtr
.driver stream  cel8000
.driver show    cel4000 celShow4000
.driver showAt  cel4000 celShowAt4000
.driver show    cel8000 celShow8000
.driver showAt  cel8000 celShowAt8000 xy

.transform copy celCopy

.macro celInit
        jsr celSelect
.endm

; Both windows back on the machine's own memory, which is what a DOS returned to
; found there — see docs/decisions/0229-no-window-has-a-base.md.
.macro celFinalize
        lda #0
        sta RAMMAP4000L
        sta RAMMAP4000H
        sta RAMMAP8000L
        sta RAMMAP8000H
.endm

.macro celOpen
        jsr celOpenStream
.endm

; A = the set of ROMs the Phase entered needs, of which this board's machine
; switches one: the OS, bit 0 of PORTB, set for the ROM. The floating-point pack
; is not this driver's to switch, since the board maps the parallel bus's ROM
; over it — see 0229.
.macro celRoms
        jsr celSelectRoms
.endm

.macro celRead
        jsr celReadByte
.endm

; The state to show, which is known where the use is written.
.macro celShow4000 state
        ldx #state
        jsr celMap4000
.endm

; X = the state to show.
.macro celShowAt4000
        jsr celMap4000
.endm

.macro celShow8000 state
        ldx #<state
        ldy #>state
        jsr celMap8000
.endm

; X/Y = the state to show, the low byte in X.
.macro celShowAt8000
        jsr celMap8000
.endm

; The device onto the bus. Written once by the Container and never again —
; nothing here puts another device on the bus, and a program that does is
; responsible for putting this one back.
;
; **One write and not two.** The device's own loader writes the OS's shadow of
; this register beside it, and that is unnecessary here: the shadow is what
; lets a device's ROM, called *by the OS*, read back which device it is, and
; this board has no such ROM — it maps one page at $DF00 and nothing at the
; $D803 and $D80B the kernel's scan tests, so the scan never marks it and the
; OS never calls into it. The scan also leaves every slot deselected when it
; ends, which is why this has to be written at all.
.proc celSelect
        lda #celPbiId
        sta PDVREG
        rts
.endp

; A = the set of ROMs: bit 0 the OS. The other bits of PORTB are left as they
; are.
.proc celSelectRoms
        and #$01
        bne @in
        lda PORTB
        and #$FE
        sta PORTB
        rts
@in
        lda PORTB
        ora #$01
        sta PORTB
        rts
.endp

; X = the state of `cel4000`: `main`, or one of the blitter's five blocks.
; Nothing of storage stands behind this window.
.proc celMap4000
        lda celBlockLow,x
        sta RAMMAP4000L
        lda celBlockHigh,x
        sta RAMMAP4000H
        rts
.endp

; X/Y = the state of `cel8000`, the low byte in X: a Bank of `sdram`. Both
; are given back.
.proc celMap8000
        txa                             ; the model's unit i is block i + 1
        clc
        adc #1
        sta RAMMAP8000L
        tya
        adc #0
        and #$07                        ; eleven bits
        sta RAMMAP8000H
        rts
.endp

; The two bytes each state of `cel4000` is, in the order the variant lists
; them: `main` first, which is the window unmapped, then the five blocks, which
; bit 7 of the high byte selects.
.section absolute, readonly
celBlockLow
        .byte $00, $00, $01, $02, $03, $04
celBlockHigh
        .byte $00, $80, $80, $80, $80, $80
.ends

; The stream: where the next byte is, and which unit is in. A plain zero-page
; Section rather than Temporaries, because the value has to survive between one
; call and the next while nothing here is running. They are also the cells the
; routine writes a position into before it opens the stream, `celPtr` the
; offset and `celUnit` the unit, which `open` turns into a pointer where they
; lie.
.section zeropage
celPtr          .res 2
celUnit         .res celUnitBytes
.ends

; The stream to the position in celUnit and celPtr. An offset past the unit's
; end carries into the units after, since the routine counts on in a Frame
; without knowing where a unit ends: a unit is $4000 bytes, so the offset's
; top two bits are units. The low byte is already where the pointer wants it.
.proc celOpenStream
        lda celPtr+1
@carry
        cmp #celUnitPages
        bcc @within
        sbc #celUnitPages
        inc celUnit
.if celUnitBytes > 1
        bne @carry
        inc celUnit+1
.endif
        jmp @carry
@within
        clc
        adc #>celWindow
        sta celPtr+1
        ldx celUnit
.if celUnitBytes > 1
        ldy celUnit+1
.else
        ldy #0
.endif
        jsr celMap8000
        rts
.endp

; A = the next byte of the stream. X and Y are not preserved.
.proc celReadByte
        ldy #0
        lda (celPtr),y
        inc celPtr
        bne @done
        inc celPtr+1
        ldy celPtr+1
        cpy #>celWindowEnd
        bne @done
        pha
        jsr celNextUnit
        pla
@done
        rts
.endp

; The window's end: the next unit in, and the pointer back at its start.
.proc celNextUnit
        inc celUnit
.if celUnitBytes > 1
        bne @counted
        inc celUnit+1
@counted
        ldy celUnit+1
.else
        ldy #0
.endif
        ldx celUnit
        jsr celMap8000
        lda #0
        sta celPtr
        lda #>celWindow
        sta celPtr+1
        rts
.endp

; The decoder of `copy`: X/Y = the destination, the stream at the stored size
; and then the bytes. Reads the window through the pointer rather than through
; celReadByte, which is what a driver's own decoder is for: a run at a time,
; where a run ends at the source's page end or at the size, so that the inner
; loop is `(zp),y` down to zero — the window's end is a page end, so a unit is
; never crossed inside a run.
celDst  .ztemp 2
celSize .ztemp 2
celRun  .ztemp 1                        ; bytes in the run, zero for 256

.proc celCopy
        stx celDst
        sty celDst+1
        jsr celReadByte
        sta celSize
        jsr celReadByte
        sta celSize+1
@run
        lda celSize
        ora celSize+1
        beq @done
        lda celPtr                      ; to the end of the source's page
        eor #$FF
        clc
        adc #1
        sta celRun
        lda celSize+1
        bne @copy                       ; at least a page left: the run stands
        lda celRun
        beq @cap                        ; a whole page, and less than one left
        cmp celSize
        bcc @copy
@cap
        lda celSize
        sta celRun
@copy
        ldy celRun
@byte
        dey
        lda (celPtr),y
        sta (celDst),y
        tya
        bne @byte
        ldx celRun                      ; the run, as 256 where it is zero
        bne @counted
        inc celPtr+1
        inc celDst+1
        dec celSize+1
        jmp @crossed
@counted
        txa
        clc
        adc celPtr
        sta celPtr
        bcc @source
        inc celPtr+1
@source
        txa
        clc
        adc celDst
        sta celDst
        bcc @destination
        inc celDst+1
@destination
        sec
        lda celSize
        stx celRun
        sbc celRun
        sta celSize
        bcs @crossed
        dec celSize+1
@crossed
        lda celPtr+1
        cmp #>celWindowEnd
        bne @run
        jsr celNextUnit                 ; the window's end: the next unit in
        jmp @run
@done
        rts
.endp
