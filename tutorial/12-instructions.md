# Instructions

The program in chapter one had a line in it that nothing since has explained:

```asm
        jcc @l2
```

`jcc` is not a 6502 mnemonic. The processor has `bcc`, which branches when the
carry is clear and reaches 127 bytes forward and 128 back. `jcc` branches on
the same flag and goes to the same place, and how many bytes it takes is not
decided in the source. It cannot be: the distance depends on where the
Sections went, and that is settled by the solver, after everything in the
program has been read.

Everything in this chapter but the last section is that shape: the text says
what is to happen and the tool chooses the instruction that does it. The last
section is a rule about what was written, and it is here because it is the one
place where the answer is not the obvious one.

## Two bytes or five

A Jcc is written as the Bcc of the same letters where the distance fits one,
and otherwise as the opposite Bcc over three bytes followed by a `jmp`:

| Written | Where it fits a branch | Where it does not |
|---|---|---|
| `jne target` | `bne target` — 2 bytes | `beq` over 3 bytes, `jmp target` — 5 bytes |

Here are both, in one Proc, spelled the same way:

<!-- include 12-instructions/two-or-five/loop.asm tag=loop -->
```asm
; The same mnemonic twice, and two different instructions. The first reaches
; forward over one instruction; the second reaches back over the whole
; unrolled body. Each is held to its size at the line below, and the source
; says nothing about either.
.proc entry
        ldx #0
@loop
        lda counter
@a      jcc @skip
@b      inc counter
@skip
        unrolled 16
        inx
        cpx #5
@c      jcc @loop
@d
        rts

        .assert @b - @a == 2
        .assert @d - @c == 5
.endp
```
<!-- end -->

The first `jcc` reaches forward over one instruction and is two bytes. The
second reaches back over everything the loop does and is five. Nothing in the
text distinguishes them, and `.assert` — which chapter eleven used on a
character set, and which is checked after Place — is what holds each to its
size here.

What stands between the second branch and its target is not in the source as
lines at all:

<!-- include 12-instructions/two-or-five/loop.asm tag=unrolled -->
```asm
; What stands between a branch and its target, written as a number rather than
; as lines. `n` is how many copies there are, and nothing in the source counts
; the bytes they come to.
.macro unrolled n
.if n > 0
        lda counter
        clc
        adc #7
        sta counter
        unrolled n - 1
.endif
.endm
```
<!-- end -->

`unrolled 16` is one statement. The 144 bytes it comes to are the macro's, and
a reader counting lines in that Proc to see whether the branch reaches would be
counting the wrong things. The branch reaches at `unrolled 12` and does not at
`unrolled 13`; nobody is going to know that, and a program should not break
because somebody wrote a larger number.

Every Jcc starts at two bytes, and one whose distance does not fit becomes
five, until no more change. A form only ever lengthens, so what comes out is
the shortest that works, and it depends on no address — the same in every
Phase and every build. Nothing anywhere says which form a Jcc took.

The long form is not free. Where the condition holds, the `jmp` runs: five
cycles against a branch's three. Where it does not, the opposite branch is
taken over the `jmp`: three cycles against a branch's two. A loop that is
counted in cycles should be written with a Bcc, which is never lengthened —
and which says so when it cannot reach.

## What a branch says instead

<!-- include 12-instructions/does-not-reach/loop.asm tag=branch -->
```asm
; The same loop with the processor's own branch, which has one form and
; reaches what that form reaches.
.proc entry
        ldx #0
@loop
        unrolled 16
        inx
        cpx #5
        bcc @loop
        rts
.endp
```
<!-- end -->

<!-- diagnostics 12-instructions/does-not-reach -->
```
error[NGA6102]: `bcc` reaches -128 to 127 and this is -149 bytes away; write `jcc`, which is five bytes where two do not reach
  --> loop.asm:25:9
   |
25 |         bcc @loop
   |         ^^^^^^^^^
```
<!-- end -->

The distance is in the message because there is no other place to read it
from. This is the same loop as above with the processor's own instruction, and
the refusal names the one that has the reach.

## One of its own positions

`.dispatch` goes to one of the positions it names, chosen by the value in `A`,
and never to the statement after it.

<!-- include 12-instructions/dispatch/state.asm tag=step -->
```asm
; Four positions of this Proc, chosen by what is in `A`. The clamp is the
; writer's: the statement indexes a table and does not check, exactly as any
; other index does not.
.proc step
        lda state
        cmp #3
        jcc @ok
        lda #3
@ok     .dispatch @idle, @walk, @run, @stop
@idle   lda #0
        jmp @set
@walk   lda #1
        jmp @set
@run    lda #4
        jmp @set
@stop   lda #0
@set    sta speed
        rts
.endp
```
<!-- end -->

Every target is a position of the Section the statement stands in. That is
what makes this different from a table of addresses somewhere in memory: the
four positions are Successors of the statement, the way a branch's operand is,
so the Proc's flow is known and its Temporaries have the liveness of ordinary
code. A table that reaches other Sections is a different thing, costs a
Section its known flow, and has its own statement.

A target may stand twice, which is how a `default` and the holes of a jump
table are written. The value is the caller's to hold below the number of
targets — the `cmp` and the `lda #3` above are the clamp, and the statement
does not check, exactly as indexing anywhere else does not.

Two instructions can do this and the tool writes whichever the Target has. On
a 6502 it is `tax` and the `rts` trick over two half tables: ten bytes of code
and one byte per target in each half.

<!-- map 12-instructions/dispatch -->
```
NGA memory map

phase phase0 (0)
  zero page: 0 of 128 bytes
  memory:    50 of 40960 bytes
  $2000-$2000  state.state section            phase0
  $2001-$2001  state.speed section            phase0
  $2002-$2031  state.step  proc               phase0
```
<!-- end -->

## What the processor has

The other form needs `jmp (abs,x)`, which the 6502 does not have. Saying so is
one line of the Project file:

<!-- include 12-instructions/dispatch-65sc02/main.ngp tag=cpu -->
```ngp
target {
  cpu "65sc02"
  region $0080 .. $00FF ram
  region $2000 .. $BFFF ram
}
```
<!-- end -->

That project reads the same `state.asm`, changing nothing in it:

<!-- map 12-instructions/dispatch-65sc02 -->
```
NGA memory map

phase phase0 (0)
  zero page: 0 of 128 bytes
  memory:    45 of 40960 bytes
  $2000-$2000  state.state section            phase0
  $2001-$2001  state.speed section            phase0
  $2002-$202C  state.step  proc               phase0
```
<!-- end -->

Five bytes shorter, and ten cycles instead of twenty-two. The form indexes
pairs rather than bytes, so it reaches 128 targets where the other reaches
256.

`cpu` governs the instructions a Module may write as well as the ones the tool
writes for it. Every 6502 instruction is legal everywhere; the 65SC02's
additions — `stz`, the `(zp)` modes, `inc` and `dec` of `A`, `bra`,
`trb`/`tsb`, the extra pushes and pulls, `bit` immediate and indexed, and
`jmp (abs,x)` — are legal only where the Target says so.

<!-- include 12-instructions/processor/clear.asm tag=clear -->
```asm
; `stz` writes a zero without going through a register, and the 6502 has no
; such instruction.
.proc entry
        stz counter
        rts
.endp
```
<!-- end -->

<!-- diagnostics 12-instructions/processor -->
```
error[NGA5107]: `stz` is a `65sc02` instruction, and this target is a `6502`
  --> clear.asm:10:9
   |
10 |         stz counter
   |         ^^^^^^^^^^^
```
<!-- end -->

## A jump that is certain

`jmpcc`, `jmpcs`, `jmpeq` and `jmpne` go where the branch of the same letters
goes, and are taken because the writer knows the flag holds there. `jra` is
the same thing with no condition at all.

| Written | Where it fits a branch | Where it does not |
|---|---|---|
| `jmpne target` | `bne target` — 2 bytes | `jmp target` — 3 bytes |
| `jra target` | `bra target` — 2 bytes | `jmp target` — 3 bytes |

Three bytes and not five, because there is nothing to skip. Neither form ever
falls through, so a certain jump has **one Successor**, the way `jmp` has,
where a Jcc has two — and the model has been told that control does not
continue past it.

<!-- include 12-instructions/certain/jump.asm tag=certain -->
```asm
; Four jumps that are always taken, and none of them five bytes: there is no
; opposite branch and nothing to skip. The `clc` above each `jmpcc` is what
; makes the claim true; `jra` claims nothing and needs no such line.
.proc entry
        clc
@a      jmpcc @near
@b      inc flag
@near
        clc
@c      jmpcc @far
@d      unrolled 16
@far
@e      jra @beyond
@f      inc flag
@beyond
@g      jra @end
@h      unrolled 16
@end
        rts

        .assert @b - @a == 2
        .assert @d - @c == 3
        .assert @f - @e == 2
        .assert @h - @g == 3
.endp
```
<!-- end -->

Writing one is a claim, and the assembler takes it: a `jmpeq` assembled as
`beq` goes nowhere if `Z` happens to be clear, and nothing will say so. What
makes each of the two `jmpcc` above true is the `clc` on the line before it.
That pair is how a 6502 branches unconditionally and has been for as long as
there have been 6502s, and written this way it has a jump's reach as well.
The compiler writes these for itself wherever its own trackers make a flag
certain; hand-written code should write the Bcc unless it wants exactly this.

The file above is built for a 65SC02 so that all five mnemonics can stand in
it. On that processor nobody would write the `clc` and the `jmpcc`, because
there is `jra`:

`jra` asks for no flag because it is `bra`, which is the whole of the
difference. The same file, built without `cpu "65sc02"`:

<!-- diagnostics 12-instructions/certain-6502 -->
```
error[NGA5107]: `jra` is a `65sc02` instruction, and this target is a `6502`
  --> ../certain/jump.asm:29:9
   |
29 | @e      jra @beyond
   |         ^^^^^^^^^^^

error[NGA5107]: `jra` is a `65sc02` instruction, and this target is a `6502`
  --> ../certain/jump.asm:32:9
   |
32 | @g      jra @end
   |         ^^^^^^^^
```
<!-- end -->

Both of them, including the far one, whose long form is a `jmp` that every
processor has. Size starts every certain jump in its branch and only ever
lengthens it, so a `jra` written for a 6502 is caught wherever it stands.

## The parenthesis rule

One thing in this chapter is not the tool choosing bytes, and it is the place
where what was written and what was meant come apart most quietly.

`lda (pointer),y` is indirect. `lda (table)+1` is not — those parentheses
group an expression. Both begin with `(`, and one rule separates them:

> If an operand begins with `(`, it is indirect if and only if the matching
> `)` is followed by the end of the operand or by `, y`. Otherwise that
> parenthesis is grouping.

<!-- include 12-instructions/parentheses/read.asm tag=four -->
```asm
; Four operands that begin with a parenthesis, and four different
; instructions. What each one came to is its size, and each is held to it
; below: two bytes where the parenthesis indirected, three where it grouped.
.proc entry
        ldy #0
        ldx #0
@a      lda (pointer),y
@b      lda (pointer,x)
@c      lda (table)+1
@d      lda (table),x
@e
        rts

        .assert @b - @a == 2
        .assert @c - @b == 2
        .assert @d - @c == 3
        .assert @e - @d == 3
.endp
```
<!-- end -->

The sizes are the evidence. The first two went through a zero page pair and
are two bytes; the last two are absolute and are three, because their
parentheses grouped an expression and the operand that came out was an
ordinary address.

The rule is applied by scanning forward to the matching parenthesis before any
expression is parsed. The reason is not economy. It keeps the expression
parser — the same one the Project file uses — from ever learning that
addressing modes exist. Parsing the expression first would make `(pointer,x)`
fail on the comma, and the exception needed to rescue it would live in the one
component that is meant to be shared.

The fourth line is a character away from the second and does something else:

<!-- diagnostics 12-instructions/parentheses -->
```
warning[NGA0141]: these parentheses group rather than indirect; `(expression,x)` is the indirect form
  --> read.asm:21:13
   |
21 | @d      lda (table),x
   |             ^^^^^^^
```
<!-- end -->

It assembles, because `expression,x` is a real mode and the parentheses are
harmless around it. It is a warning and not an error for that reason, and a
warning rather than nothing because `(table),x` is what somebody writes when
they meant `(table,x)` and the machine will not say so afterwards.

`lda (pointer,y)` has no mode behind it at all — the processor indexes
indirect by `X` only — and is refused, with `(pointer),y` offered.
