; What the PET's ROM keeps in RAM, and the addresses a program calls it at.
; Sections that hold no bytes, pinned where the kernal's own state lives, so
; that the solver sees memory which is taken rather than a Region it may never
; allocate from — the arrangement atari/os.asm has, and 0038 and 0051 say why.
;
; This is a machine whose interrupt handler the program has taken over, so what
; is reserved here is **what the kernal's disk path touches** and not what the
; ROM is documented to own. Measured rather than read: one open, one CHKIN, 512
; bytes, CLRCHN and CLOSE touch twelve bytes of the zero page, of which five are
; parameters the caller writes itself. The other 243 bytes are the program's.
;
; Seven bytes are reserved that the disk path does **not** touch: $8F, $98, $99,
; $9B, $A6, $F9 and $FA, which the kernal's own interrupt handler writes. A
; program arrives here through `SYS` with interrupts still running and takes
; them over itself, however early; those seven are what the handler may write in
; the window before it does, and a Section standing there would be corrupted by
; a retrace nobody asked for.
;
; **Every address below is specific to `kernal-2.901465-03`**, the ROM a PET
; 3032 runs. The two bodies in particular are not portable: the jump table's
; OPEN and CLOSE begin with the BASIC parameter parser, which reads its
; arguments out of BASIC text and clears $D1 and $D3, so machine code sets the
; zero page by hand and enters past it — and the values published for other PET
; ROM sets land mid-instruction here and disassemble as a JAM. A 4032 is a
; Module and a Variant of its own, verified by disassembly, and not this one
; with numbers changed.

.export STATUS, FNLEN, LA, SA, FA, FNADR
.export KERNAL_OPEN, KERNAL_CLOSE, CHKIN, CHKOUT, CLRCHN, CHRIN, CHROUT

; The bodies of OPEN and CLOSE, entered past the parser. Success is not in the
; carry: the count of open files at $AE is one higher than it was, and that is
; the only thing that says so. CHKIN does not normalise the carry either — its
; common exit is PLA/TAY/PLA/TAX/PLA/RTS — so nothing after it may branch on
; one. And EOF is bit **6** of STATUS, not bit 7.
KERNAL_OPEN  = $F524
KERNAL_CLOSE = $F2AC
CHKIN        = $FFC6
CHKOUT       = $FFC9
CLRCHN       = $FFCC
CHRIN        = $FFCF
CHROUT       = $FFD2

; The byte the interrupt handler writes low in the jiffy clock, and the RAM
; vector the ROM's stub dispatches through at $E61B — which is the supported
; place to put a handler of one's own, and is why it is not the program's even
; on a machine that takes the interrupt over.
.section zeropage at $008F, root
kernalClock
        .res 1                          ; $8F: the jiffy clock's low byte
kernalIrqVector
        .res 2                          ; $90-$91: IRQ, and $92-$93 is BRK's
.ends

; The status word, which EOF and every bus error arrive in.
.section zeropage at $0096, root
STATUS  .res 1
.ends

; Shift, the key down and the STOP flag: the handler's, not the disk path's.
.section zeropage at $0098, root
kernalKeys
        .res 2                          ; $98-$99
.ends

.section zeropage at $009B, root
kernalStop
        .res 1
.ends

; The IEEE-488 output flag and the character waiting behind it.
.section zeropage at $00A0, root
kernalOutFlag
        .res 1
.ends

.section zeropage at $00A5, root
kernalOutChar
        .res 1                          ; $A5
kernalKeyImage
        .res 1                          ; $A6, the handler's
.ends

; How many files are open, and which devices input and output are on. The first
; is what an open is judged by, the other two are what CHKIN and CLRCHN move.
.section zeropage at $00AE, root
kernalOpenFiles
        .res 1                          ; $AE
kernalInputDevice
        .res 1                          ; $AF
kernalOutputDevice
        .res 1                          ; $B0
.ends

; The five a caller writes before entering a body: the length of the name, the
; logical file, the secondary address and the device, and a pointer to the name.
; Reserved here rather than left to the program because the ROM reads them under
; names of its own and a Section standing on them would be read as a filename.
.section zeropage at $00D1, root
FNLEN   .res 1                          ; $D1: zero for a command channel
LA      .res 1                          ; $D2: the logical file number
SA      .res 1                          ; $D3: the secondary address
FA      .res 1                          ; $D4: the device, 8 for a drive
.ends

.section zeropage at $00DA, root
FNADR   .res 2                          ; $DA-$DB: where the filename stands
.ends

; The two cassette switch flags the interrupt handler writes.
.section zeropage at $00F9, root
kernalCassette
        .res 2
.ends

; One byte per open file in each of three tables, ten entries each: the logical
; file, the device and the secondary address. The kernal indexes them by the
; order files were opened, so all thirty are taken as soon as any file is.
.section absolute at $0251, root
kernalFiles
        .res 30
.ends
