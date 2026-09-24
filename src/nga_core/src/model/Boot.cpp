#include "nga/model/Atr.hpp"

namespace nga::model
{

namespace
{

/// The boot record, as the assembler reads it. What the OS does with the six
/// bytes in front of it, and why the loader may use the zero page it uses,
/// is docs/spec/atr.md.
constexpr std::string_view BOOT_RECORD_SOURCE =
    R"asm(; The boot record of an .atr: the first three sectors, which the OS reads to
; $0700 and calls at $0706. Those three hold 128 bytes, because the OS boots
; with DSCTLN at 128; every sector after them holds 256, so the first thing the
; loader does is say so. What it loads is the load image, the stream of segments
; a `.xex` is made of with $FFFF closing it, waiting in the sectors after
; storage. See docs/spec/atr.md.

DSKINV = $E453
DUNIT  = $0301
DCOMND = $0302
DBUFLO = $0304
DBUFHI = $0305
DAUX1  = $030A
DAUX2  = $030B
DSCTLN = $02D5                  ; what a sector holds, which DSKINV transfers
RUNAD  = $02E0
DOSVEC = $000A

bootBuf = $0400                 ; the cassette buffer and the spare page after
                                ; it, both free once the record is in: 256
                                ; bytes, which is what a sector holds here

; The zero page the OS leaves once it has booted: RAMLO and CASINI were the
; boot's own scratch and TSTDAT is spare. TRAMSZ, $06, is not among them —
; coldstart reads it after the boot has returned.
bootDst    = $0000              ; where the segment being read lands
bootLeft   = $0002              ; bytes of it still to come
bootSector = $0004              ; the sector the buffer holds
bootPos    = $0007              ; where in the buffer the stream stands, a byte
                                ; that wraps because a sector holds 256

.section absolute at $0700, root
ngaBootRecord
        .byte 0                 ; the OS ignores this byte
        .byte 3                 ; sectors it reads, from the first
        .word $0700             ; where it puts them
        .root                   ; the OS calls what stands here
        .word ngaBootIdle       ; DOSINI, called once the load returns and on reset
.ends

; The first sector of the load image, which the tool writes here once it knows
; how much storage the program came to use, and one byte of the loader's own.
; At the end of the record, out of the loader's way; the zero page the OS
; leaves is spoken for, and $0006 is TRAMSZ, which the cold start reads after
; the boot has returned.
.export ngaBootImage
.section absolute at $087D, root
ngaBootImage
        .word 0
bootNeed
        .res 1                  ; set once the position has wrapped: the next
.ends                           ; byte stands in the sector after

; The OS jsr's here once the three sectors stand at $0700, and a carry set on
; return is a boot it tries again.
.proc ngaBootLoad, absolute at $0706, root
        lda #0                  ; every sector from here on holds 256 bytes
        sta DSCTLN
        lda #1
        sta DSCTLN+1
        lda ngaBootImage
        sta bootSector
        lda ngaBootImage+1
        sta bootSector+1
        jsr fill
@segment
        jsr byte
        sta bootDst
        jsr byte
        sta bootDst+1
        and bootDst             ; $FFFF closes the stream, and no segment starts there
        cmp #$FF
        beq @run
        jsr byte                ; the end address, inclusive
        sta bootLeft
        jsr byte
        sta bootLeft+1
        sec                     ; the count is end - start + 1
        lda bootLeft
        sbc bootDst
        sta bootLeft
        lda bootLeft+1
        sbc bootDst+1
        sta bootLeft+1
        inc bootLeft
        bne @bytes
        inc bootLeft+1
@bytes
        lda bootLeft
        ora bootLeft+1
        beq @segment
        jsr byte
        ldy #0
        sta (bootDst),y
        inc bootDst
        bne @counted
        inc bootDst+1
@counted
        lda bootLeft
        bne @low
        dec bootLeft+1
@low
        dec bootLeft
        jmp @bytes
@run
        lda RUNAD               ; where the program starts, which the OS jumps to
        sta DOSVEC
        lda RUNAD+1
        sta DOSVEC+1
        clc                     ; a boot the OS keeps
        rts

; A = the next byte of the image, which runs from one sector into the next.
; The position wraps to zero at the sector's end, and bootNeed says the buffer
; no longer holds what stands there.
byte
        lda bootNeed
        beq @have
        inc bootSector
        bne @next
        inc bootSector+1
@next
        jsr fill
@have
        ldy bootPos
        inc bootPos
        bne @within
        sty bootNeed            ; Y is $FF here, and any non-zero will do
@within
        lda bootBuf,y
        rts

; The sector in bootSector into the buffer, tried again until it is read: the
; OS read this record off this diskette, so the drive is there.
fill
        lda #0
        sta bootPos
        sta bootNeed
        lda bootSector
        sta DAUX1
        lda bootSector+1
        sta DAUX2
@again
        lda #1
        sta DUNIT
        lda #$52
        sta DCOMND
        lda #<bootBuf
        sta DBUFLO
        lda #>bootBuf
        sta DBUFHI
        jsr DSKINV
        bmi @again
        rts
.endp

; DOSINI: the OS calls it once the load has returned and on every reset, so it
; is in memory by then and the solver places it as it places any Proc.
.proc ngaBootIdle, root
        rts
.endp
)asm";

} // namespace

std::string bootRecordSource()
{
  return std::string{ BOOT_RECORD_SOURCE };
}

void addBootRecord( Project& project, diag::SourceManager& sources )
{
  diag::FileId const file = sources.addFile( "<nga>/boot.asm", bootRecordSource() );
  ModuleIndex const index{ static_cast<std::uint32_t>( project.modules.size() ) };
  project.modules.push_back( ProjectModule{
      .name = std::string{ BOOT_MODULE }, .file = file, .residency = {}, .generated = Generated::NONE } );
  for ( Phase& phase : project.phases.phases )
  {
    phase.needs.push_back( index );
  }
  deriveResidency( project );
}

} // namespace nga::model
