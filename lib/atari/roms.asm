; The driver of an XL's ROMs and of nothing else: PORTB bits 0 and 1 take the OS
; ROM — with the floating-point pack — and BASIC in and out, which is all this
; Module switches. For a program without storage on a machine with no banking,
; `atari/800xl.ngp`, so that a Phase may do without the OS and keep what it
; needs in the RAM under it — see 0229 and 0241. It has no stream: nothing is
; read through it, and `nga.open` and `nga.read` are refused where it is the
; driver — see 0242.
;
; `select` is handed the set a Phase needs and writes the two bits, keeping the
; rest of PORTB as it is. `init` keeps the PORTB the program found and
; `finalize` puts it back, so a program that returns leaves BASIC as the user
; had it. `mask` and `unmask` keep the DOS's interrupts away while the load has
; every ROM out. `PORTB` is the variant's register.
;
; Resident: the variant's file that includes it lists it so.

.driver rom      osrom                  ; bit 0 of the set
.driver rom      mathpack osrom         ; bit 1, which one bit of PORTB shows with the OS
.driver rom      basic                  ; bit 2
.driver select   romsSelect
.driver init     romsInit
.driver finalize romsFinalize
.driver mask     romsMask
.driver unmask   romsUnmask

NMIEN = $D40E

.macro romsSelect
        jsr romsSelectSet
.endm

.macro romsInit
        lda PORTB
        sta romsFound
.endm

.macro romsFinalize
        lda romsFound
        sta PORTB
.endm

.macro romsMask
        sei
        lda #0
        sta NMIEN
.endm

.macro romsUnmask
        lda #$40
        sta NMIEN
        cli
.endm

.section
romsFound       .res 1
.ends

; A = the set: bit 0 or bit 1 the OS in, bit 2 BASIC in. PORTB bit 0 clear
; takes the OS out, bit 1 clear puts BASIC in.
.proc romsSelectSet
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
