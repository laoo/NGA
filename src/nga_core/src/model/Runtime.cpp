#include "nga/model/Runtime.hpp"

#include <algorithm>
#include <filesystem>
#include <string>

namespace nga::model
{

namespace
{

/// The runtime's text, as the assembler reads it. What each Proc computes,
/// division by zero included, is docs/spec/c-subset.md's.
constexpr std::string_view C_RUNTIME_SOURCE =
    R"asm(; The C runtime: what `*`, `/` and `%` call where no constant makes them
; shifts, adds and masks. Each Proc is called as any Proc of the assembler is:
; its `.declare arg`s written, `jsr`, its `.declare ret` read. A product is the
; low half, one for either sign; a quotient and a remainder truncate towards
; zero, as C does. See docs/decisions/0095-literals-and-the-runtime.md.

.export __mul8, __udiv8, __umod8, __sdiv8, __smod8
.export __mul16, __udiv16, __umod16, __sdiv16, __smod16
.export __mul8to16

; ---------------------------------------------------------------------------
; 8 bits

; left * right: the bits of `left` from the top, the product doubled before
; each and `right` added where the bit is set. The product is left in `A`,
; where it was made — see docs/decisions/0146-the-runtime-in-registers.md.
.proc __mul8, absolute within 256
        .declare arg u8
left    .ztemp 1
        .declare arg u8
right   .ztemp 1
        .declare ret a

        lda #0
        ldx #8
loop    asl                     ; product * 2
        asl left                ; the next bit of `left` into the carry
        bcc skip
        clc
        adc right
skip    dex
        bne loop
        rts
.endp

; left * right, both bytes, the whole product in two: the bits of `left` from
; the bottom, `right` doubled after each and added where the bit is set, the
; product kept in `Y` below and `X` above. Done once no bit of `left` is left,
; so a `left` below 16 takes at most four rounds where every one took eight:
; the compiler hands the operand the ranges hold smaller as `left`. `right` is
; doubled at most seven times, so its pair never carries out of the top — see
; docs/decisions/0144-a-multiply-stops-when-its-multiplier-does.md, and
; docs/decisions/0122-a-multiply-of-two-bytes.md for why there is a Proc of
; two bytes at all. `left` comes in `A`, which is where it is walked — see
; docs/decisions/0146-the-runtime-in-registers.md.
;
; Shaped as Oscar64's `mul16by8` is, which the benchmarks measured it against
; — see docs/decisions/0154-a-multiply-shaped-by-its-bits.md. `A` always holds
; a set bit at `next`, so a clear bit needs no test for zero; the first bit,
; where set, makes the product `right` rather than adding it to zero; and the
; last set bit adds straight into `product`.
.proc __mul8to16, absolute within 256
        .declare arg a
        .declare arg u8
right   .ztemp 1
        .declare ret u16
product .ztemp 2
high    .ztemp 1                ; `right`'s high byte as it is doubled
left    .ztemp 1                ; what is left of `left`, while `A` adds

        ldx #0
        lsr                     ; bit 0 of `left` into the carry
        beq last                ; no bit above it: the product is `right` or 0
        stx high
        ldy #0
        bcc next
        ldy right               ; bit 0 set: the product so far is `right`
        bcs next                ; always
add     sta left                ; what is left of it, while `A` adds
        clc
        tya
        adc right
        tay
        txa
        adc high
        tax
        lda left
next    asl right
        rol high
        lsr                     ; the next bit into the carry
        bcc next                ; clear: `A` still holds a set bit
        bne add                 ; set, and more above it
        clc                     ; set, and the last: added into the product
        tya
        adc right
        sta product
        txa
        adc high
        sta product+1
        rts
last    bcc zero                ; `A` and `X` are zero
        lda right
zero    sta product
        stx product+1
        rts
.endp

; dividend / divisor: the dividend shifted into the remainder in `A` a bit at a
; time, and the divisor subtracted where it fits, which sets the bit of the
; quotient the shift left clear in the dividend's byte. A carry out of the
; shift is a remainder of nine bits, which always fits. A divisor of zero
; always fits: the quotient is all ones.
;
; The quotient is built where the dividend was, a bit a turn, so that byte is
; both the argument and the result and its name says so — see
; docs/decisions/0119-one-temporary-carries-two-roles.md.
.proc __udiv8, absolute within 256
        .declare arg u8
        .declare ret u8
dividendAndQuotient .ztemp 1
        .declare arg u8
divisor .ztemp 1

        lda #0
        ldx #8
loop    asl dividendAndQuotient
        rol                     ; the remainder takes the dividend's top bit
        bcs subtract
        cmp divisor
        bcc skip
subtract
        sbc divisor             ; the carry is set on both ways here
        inc dividendAndQuotient ; the quotient's bit
skip    dex
        bne loop
        rts
.endp

; dividend % divisor: as `__udiv8`, the remainder kept. A divisor of zero leaves
; the dividend.
;
; The remainder is left in `A`, where it was made — see
; docs/decisions/0146-the-runtime-in-registers.md.
.proc __umod8, absolute within 256
        .declare arg u8
dividend .ztemp 1
        .declare arg u8
divisor .ztemp 1
        .declare ret a

        lda #0
        ldx #8
loop    asl dividend
        rol
        bcs subtract
        cmp divisor
        bcc skip
subtract
        sbc divisor
        inc dividend
skip    dex
        bne loop
        rts
.endp

; dividend / divisor, signed: the unsigned quotient of the magnitudes, negated
; where the signs differ.
;
; The dividend comes in `A` and the quotient goes back in it — see
; docs/decisions/0146-the-runtime-in-registers.md.
.proc __sdiv8, absolute within 256
        .declare ret a
        .declare arg a
        .declare arg i8
divisor .ztemp 1
sign    .ztemp 1

        tax                     ; the dividend, while the signs are compared
        eor divisor
        sta sign                ; bit 7: the signs differ

        txa
        bpl dividendKept
        eor #$FF
        clc
        adc #1
dividendKept
        sta __udiv8.dividendAndQuotient

        lda divisor
        bpl divisorKept
        eor #$FF
        clc
        adc #1
divisorKept
        sta __udiv8.divisor

        jsr __udiv8
        lda __udiv8.dividendAndQuotient
        bit sign
        bpl done
        eor #$FF
        clc
        adc #1
done    rts
.endp

; dividend % divisor, signed: the unsigned remainder of the magnitudes, negated
; where the dividend is negative.
;
; The dividend comes in `A` and the remainder goes back in it, as it comes
; back from `__umod8` — see docs/decisions/0146-the-runtime-in-registers.md.
.proc __smod8, absolute within 256
        .declare ret a
        .declare arg a
        .declare arg i8
divisor .ztemp 1
sign    .ztemp 1

        sta sign                ; bit 7: the dividend is negative
        tax                     ; `N` from the dividend: the caller promises none
        bpl dividendKept
        eor #$FF
        clc
        adc #1
dividendKept
        sta __umod8.dividend

        lda divisor
        bpl divisorKept
        eor #$FF
        clc
        adc #1
divisorKept
        sta __umod8.divisor

        jsr __umod8             ; the remainder in `A`
        bit sign
        bpl done
        eor #$FF
        clc
        adc #1
done    rts
.endp

; ---------------------------------------------------------------------------
; 16 bits

; left * right, the low half: the bits of `left` from the bottom, `right`
; doubled after each and added where the bit is set, the product kept in `Y`
; below and `high` above. Done once no bit of `left` is left, which is
; docs/decisions/0144-a-multiply-stops-when-its-multiplier-does.md carried from
; `__mul8to16` to the wider Proc: a multiplier whose high byte is zero takes at
; most eight rounds where every one took sixteen, and one of four bits takes
; four. The round is cheaper as well, `right` being doubled where
; the product was.
;
; `A` walks what is left of the multiplier. Over the low byte a set bit is
; rolled in above it first, so that `A` running out says the byte is done and
; not that the bits above are clear; over the high byte, which is walked at
; `narrow` and is where a multiplier of one byte starts, `A` running out ends
; the Proc. See docs/decisions/0170-a-wide-multiply-stops-when-its-multiplier-does.md.
.proc __mul16, absolute within 256
        .declare arg u16
left    .ztemp 2
        .declare arg u16
right   .ztemp 2
        .declare ret u16
product .ztemp 2
high    .ztemp 1                ; the product's high byte, while `Y` holds its low

.if __small
        ; Sixteen rounds, always, the multiplier walked from the bottom out of
        ; its own bytes and the product added up in memory: half the bytes of
        ; the shape above and none of what makes that one quick.
        lda #0
        sta product
        sta product+1
        ldx #16
@loop
        lsr left+1
        ror left
        bcc @+skip
        clc
        lda product
        adc right
        sta product
        lda product+1
        adc right+1
        sta product+1
@skip
        asl right
        rol right+1
        dex
        bne @-loop
        rts
.else
        ldy #0
        sty high
        lda left
        ldx left+1
        beq @+narrow            ; one byte of multiplier: `A` holds all of it
        sec
        ror                     ; bit 0 out, and a set bit in above the rest
        bcc @+lowNext
@lowAdd
        tax                     ; what is left of the byte, while `A` adds
        clc
        tya
        adc right
        tay
        lda high
        adc right+1
        sta high
        txa
@lowNext
        asl right
        rol right+1
        lsr                     ; the next bit into the carry
        bcc @-lowNext           ; clear: `A` still holds a set bit
        bne @-lowAdd            ; set, and more above it
        lda left+1              ; set, and the mark: the low byte is done
@narrow
        lsr
        bcc @+next
@add
        tax
        clc
        tya
        adc right
        tay
        lda high
        adc right+1
        sta high
        txa
@next
        asl right
        rol right+1
        lsr
        bcs @-add
        bne @-next
        sty product
        lda high
        sta product+1
        rts
.endif
.endp

; dividend / divisor, as `__udiv8` with the remainder in `rest`: the quotient
; is built where the dividend was, so that byte is both the argument and the
; result — see docs/decisions/0119-one-temporary-carries-two-roles.md.
;
; A divisor of one byte below $80 takes the loop at `narrow`, where the
; remainder lives in `A` alone: it stays below the divisor, so doubling it and
; taking a bit of the dividend keeps it inside a byte, and the compare is one
; instruction rather than four. The dividend's own pair carries the quotient
; in at the bottom as it carries the next bit out at the top, so the two
; rotations serve both and nothing counts the bit separately. Half the cycles
; of the wide loop, and it is the loop every call of `prime` takes — see
; docs/decisions/0171-a-divisor-of-one-byte.md. A divisor of zero
; goes wide, which is where the quotient of all ones and the remainder of the
; dividend that docs/spec/c-subset.md states come from.
.proc __udiv16, absolute within 256
        .declare arg u16
        .declare ret u16
dividendAndQuotient .ztemp 2
        .declare arg u16
divisor .ztemp 2
rest    .ztemp 2

.if !__small
        lda divisor+1
        bne wide
        lda divisor
        beq wide
        bmi wide                ; $80 and above: a remainder of nine bits
        ldx #16
        asl dividendAndQuotient
        rol dividendAndQuotient+1
        lda #0
@narrow
        rol                     ; the remainder takes the dividend's top bit
        cmp divisor
        bcc @+narrowSkip
        sbc divisor             ; the carry is set on both ways here
@narrowSkip
        rol dividendAndQuotient ; the quotient's bit in, the next dividend bit out
        rol dividendAndQuotient+1
        dex
        bne @-narrow
        rts
.endif

wide    lda #0
        sta rest
        sta rest+1
        ldx #16
loop    asl dividendAndQuotient
        rol dividendAndQuotient+1
        rol rest                ; the remainder takes the dividend's top bit
        rol rest+1
        bcs subtract
        lda rest                ; rest >= divisor?
        cmp divisor
        lda rest+1
        sbc divisor+1
        bcc skip
subtract
        lda rest
        sec
        sbc divisor
        sta rest
        lda rest+1
        sbc divisor+1
        sta rest+1
        inc dividendAndQuotient ; the quotient's bit
skip    dex
        bne loop
        rts
.endp

; dividend % divisor, as `__udiv16`, the remainder kept: it is built in the
; result's own bytes, and the dividend's, which hold the quotient nothing
; here wants, are the Proc's own. The narrow loop is `__udiv16`'s, and leaves
; in `A` the remainder that Proc throws away — see
; docs/decisions/0171-a-divisor-of-one-byte.md.
.proc __umod16, absolute within 256
        .declare arg u16
dividend .ztemp 2
        .declare arg u16
divisor .ztemp 2
        .declare ret u16
remainder .ztemp 2

.if !__small
        lda divisor+1
        bne wide
        lda divisor
        beq wide
        bmi wide                ; $80 and above: a remainder of nine bits
        lda #0
        sta remainder+1         ; a remainder below the divisor is one byte
        ldx #16
        asl dividend
        rol dividend+1
@narrow
        rol
        cmp divisor
        bcc @+narrowSkip
        sbc divisor
@narrowSkip
        rol dividend
        rol dividend+1
        dex
        bne @-narrow
        sta remainder
        rts
.endif

wide    lda #0
        sta remainder
        sta remainder+1
        ldx #16
loop    asl dividend
        rol dividend+1
        rol remainder
        rol remainder+1
        bcs subtract
        lda remainder
        cmp divisor
        lda remainder+1
        sbc divisor+1
        bcc skip
subtract
        lda remainder
        sec
        sbc divisor
        sta remainder
        lda remainder+1
        sbc divisor+1
        sta remainder+1
        inc dividend
skip    dex
        bne loop
        rts
.endp

; dividend / divisor, signed, as `__sdiv8`.
.proc __sdiv16, absolute within 256
        .declare arg i16
dividend .ztemp 2
        .declare arg i16
divisor .ztemp 2
        .declare ret i16
quotient .ztemp 2
sign    .ztemp 1

        lda dividend+1
        eor divisor+1
        sta sign                ; bit 7: the signs differ

        lda dividend+1
        bmi dividendNegative
        lda dividend
        sta __udiv16.dividendAndQuotient
        lda dividend+1
        sta __udiv16.dividendAndQuotient+1
        jmp dividendDone
dividendNegative
        lda dividend
        eor #$FF
        clc
        adc #1
        sta __udiv16.dividendAndQuotient
        lda dividend+1
        eor #$FF
        adc #0
        sta __udiv16.dividendAndQuotient+1
dividendDone

        lda divisor+1
        bmi divisorNegative
        lda divisor
        sta __udiv16.divisor
        lda divisor+1
        sta __udiv16.divisor+1
        jmp divisorDone
divisorNegative
        lda divisor
        eor #$FF
        clc
        adc #1
        sta __udiv16.divisor
        lda divisor+1
        eor #$FF
        adc #0
        sta __udiv16.divisor+1
divisorDone

        jsr __udiv16
        bit sign
        bmi negative
        lda __udiv16.dividendAndQuotient
        sta quotient
        lda __udiv16.dividendAndQuotient+1
        sta quotient+1
        rts
negative
        lda __udiv16.dividendAndQuotient
        eor #$FF
        clc
        adc #1
        sta quotient
        lda __udiv16.dividendAndQuotient+1
        eor #$FF
        adc #0
        sta quotient+1
        rts
.endp

; dividend % divisor, signed, as `__smod8`.
.proc __smod16, absolute within 256
        .declare arg i16
dividend .ztemp 2
        .declare arg i16
divisor .ztemp 2
        .declare ret i16
remainder .ztemp 2
sign    .ztemp 1

        lda dividend+1
        sta sign                ; bit 7: the dividend is negative

        lda dividend+1
        bmi dividendNegative
        lda dividend
        sta __umod16.dividend
        lda dividend+1
        sta __umod16.dividend+1
        jmp dividendDone
dividendNegative
        lda dividend
        eor #$FF
        clc
        adc #1
        sta __umod16.dividend
        lda dividend+1
        eor #$FF
        adc #0
        sta __umod16.dividend+1
dividendDone

        lda divisor+1
        bmi divisorNegative
        lda divisor
        sta __umod16.divisor
        lda divisor+1
        sta __umod16.divisor+1
        jmp divisorDone
divisorNegative
        lda divisor
        eor #$FF
        clc
        adc #1
        sta __umod16.divisor
        lda divisor+1
        eor #$FF
        adc #0
        sta __umod16.divisor+1
divisorDone

        jsr __umod16
        bit sign
        bmi negative
        lda __umod16.remainder
        sta remainder
        lda __umod16.remainder+1
        sta remainder+1
        rts
negative
        lda __umod16.remainder
        eor #$FF
        clc
        adc #1
        sta remainder
        lda __umod16.remainder+1
        eor #$FF
        adc #0
        sta remainder+1
        rts
.endp
)asm";

} // namespace

std::string cRuntimeSource( Intent intent )
{
  // The one thing the text does not say for itself, written above it: `.if`
  // decides on a declared value, so the Intent reaches the assembler as the
  // Constant its conditionals read. Private to this Module, which is why the
  // name is the tool's — see
  // docs/decisions/0180-the-runtime-has-a-small-shape.md.
  std::string source = intent == Intent::SIZE ? "__small = 1\n" : "__small = 0\n";
  source += C_RUNTIME_SOURCE;
  return source;
}

bool holdsC( diag::SourceManager const& sources, Project const& project )
{
  return std::ranges::any_of( project.modules,
                              [&sources]( ProjectModule const& entry )
                              {
                                return entry.generated == Generated::NONE &&
                                       std::filesystem::path{ sources.pathOf( entry.file ) }.extension() == ".ngc";
                              } );
}

void addCRuntime( Project& project, diag::SourceManager& sources )
{
  diag::FileId const file = sources.addFile( "<nga>/runtime.asm", cRuntimeSource( project.intent ) );
  ModuleIndex const index{ static_cast<std::uint32_t>( project.modules.size() ) };
  project.modules.push_back( ProjectModule{
      .name = std::string{ C_RUNTIME_MODULE }, .file = file, .residency = {}, .generated = Generated::NONE } );
  for ( Phase& phase : project.phases.phases )
  {
    phase.needs.push_back( index );
  }
  deriveResidency( project );
}

} // namespace nga::model
