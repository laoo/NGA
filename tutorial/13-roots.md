# Roots

Chapter twelve said of `.dispatch` that a table reaching other Sections is a
different thing, costs a Section its known flow, and has a statement of its
own. This is that statement, and the reason there is one.

Everything the tool has done in twelve chapters rests on knowing where control
goes. Liveness in chapter two, Prune, the co-visibility rule, the Trace a
Transition is built from — all of it reads Chunks that name other Chunks. A
program that takes an address is where that stops. `#<name`, `.word handler`,
a byte pushed for an `rts`: the value goes into a pointer, onto the stack, into
an operand patched at run time, and nothing left in the text says where it is
followed.

## An address with nobody behind it

<!-- include 13-roots/nobody/call.asm tag=nobody -->
```asm
; The address of `handler` goes into a pointer and the pointer is jumped
; through. Neither line says the two are the same jump.
.section
address
        .word handler
.ends

.proc entry
        lda address
        sta vector
        lda address + 1
        sta vector + 1
        jmp (vector)
.endp
```
<!-- end -->

<!-- diagnostics 13-roots/nobody -->
```
error[NGA4606]: the address of `handler` is taken and nothing says who follows it: `.own` if this section does, `.own NAME` if another does, `.root` if the hardware does
  --> call.asm:15:9
   |
15 |         .word handler
   |         ^^^^^^^^^^^^^

error[NGA4607]: `entry` jumps through a pointer and owns no address of code, so nothing says where the jump goes
  --> call.asm:23:9
   |
23 |         jmp (vector)
   |         ^^^^^^^^^^^^
```
<!-- end -->

Two findings, and they are the two ends of the same missing fact. The taking
does not say who follows the address; the jump does not say where it goes.
Either one alone would be a guess, and the tool makes neither: an edge of the
graph belongs to the taking, so a Section that jumps through a pointer and
owns no address of code would jump nowhere, which is not something the model
should be made to believe.

## Naming the follower

<!-- include 13-roots/a-pointer/call.asm tag=nobody -->
```asm
; The address of `handler` goes into a pointer and the pointer is jumped
; through. Neither line says the two are the same jump.
.section
address
        .own entry
        .word handler
.ends

.proc entry
        lda address
        sta vector
        lda address + 1
        sta vector + 1
        jmp (vector)
.endp
```
<!-- end -->

`.own` on its own line covers the statement below it and declares that **this
Section** follows the address. Where the Section that takes it is not the one
that follows it — as here, where one holds the word and another jumps through
the pointer — `.own NAME` names the follower: a Section, a Proc, or a Label,
whose Section it is, and several separated by commas.

It is declared and not checked, the way `temporary` is. The tool takes the
word for it: the address of code is now a jump from that Section to that code
and from nowhere else, whatever instruction actually follows the taking. One
thing is checked, because it is about Phases rather than about intent — the
follower must be present wherever the code it follows is, since in a Phase
that held the follower and not the code its jump would land in nothing.

## What it costs

A routine that finds the largest of five numbers and puts it into one of three
bands, with two Temporaries used in stretches that do not overlap. The choice
is written twice. First with chapter twelve's statement:

<!-- include 13-roots/dispatch/pick.asm tag=choose -->
```asm
; Where the three bands are chosen. The statement names the positions it goes
; to, so they are Successors of it and the Proc's flow is known.
        lda best
        cmp #3
        jcc @ok
        lda #2
@ok     .dispatch @low, @mid, @high
```
<!-- end -->

and then by hand, which on a 6502 means writing what the tool would have
written anyway:

<!-- include 13-roots/by-hand/pick.asm tag=choose -->
```asm
; The same choice as the instruction the tool would have written: the `rts`
; trick over two half tables of each target less one. The tables hold this
; Proc's own positions, so `.own` with no name is what declares them.
        lda best
        cmp #3
        jcc @ok
        lda #2
@ok     tax
        lda @hi,x
        pha
        lda @lo,x
        pha
        rts
        .own
@hi     .byte >(@low - 1), >(@mid - 1), >(@high - 1)
        .own
@lo     .byte <(@low - 1), <(@mid - 1), <(@high - 1)
```
<!-- end -->

Everything else in the two programs is the same text. They build to the same
number of bytes, and they are not the same program:

<!-- map 13-roots/dispatch -->
```
NGA memory map

phase phase0 (0)
  zero page: 1 of 128 bytes
  memory:    68 of 40960 bytes
  $0080-$0080  pick.entry.band temporary          phase0  shares with entry.best
  $0080-$0080  pick.entry.best temporary          phase0  shares with entry.band
  $2000-$2004  pick.input      section            phase0
  $2005-$2005  pick.out        section            phase0
  $2006-$2043  pick.entry      proc               phase0
```
<!-- end -->

<!-- map 13-roots/by-hand -->
```
NGA memory map

phase phase0 (0)
  zero page: 2 of 128 bytes
  memory:    68 of 40960 bytes
  $0080-$0080  pick.entry.best temporary          phase0
  $0081-$0081  pick.entry.band temporary          phase0
  $2000-$2004  pick.input      section            phase0
  $2005-$2005  pick.out        section            phase0
  $2006-$2043  pick.entry      proc               phase0
```
<!-- end -->

Sixty-eight bytes either way, and one more byte of zero page. Under
`.dispatch` the two Temporaries share one, because the statement names where
control goes and Trace can see that `best` is dead by the time `band` is
alive. Under `.own` the flow through those tables is unknown, so everything is
live everywhere and the two cannot share. The whole difference between the
programs is two bytes, and both of them are the address `band` ended up at.

That is the trade in its smallest form. `.own` is for an address the tool
cannot be told about any other way, and where a table holds positions of the
Section that jumps through it, `.dispatch` says the same thing and keeps what
`.own` spends.

## What nothing reaches

<!-- include 13-roots/dropped/screen.asm tag=dropped -->
```asm
; A display list is what ANTIC reads to know how to draw the screen, and the
; screen it points at is forty bytes ANTIC reads as well. No instruction in
; this program names either of them, because nothing in this program looks at
; them: the hardware does.
.section
displayList
        .byte $70, $70, $70, $42
        .word row
        .byte $41
        .word displayList
.ends

.section
row
        .res 40
.ends

.proc entry
        rts
.endp
```
<!-- end -->

<!-- map 13-roots/dropped -->
```
NGA memory map

phase phase0 (0)
  zero page: 0 of 128 bytes
  memory:    1 of 40960 bytes
  $2000-$2000  screen.entry proc               phase0
```
<!-- end -->

The display list is gone, and so are the forty bytes it pointed at. One byte
of program is left, which is the `rts`. Prune keeps what is reached from a
Root and drops the rest, and here nothing in the program reaches either
Section — correctly, since nothing in the program is meant to.

Which memory the hardware reads is a fact about the machine and about what the
program is for. Nothing in the code says it, so it is declared:

<!-- include 13-roots/kept/screen.asm tag=kept -->
```asm
; A display list is what ANTIC reads to know how to draw the screen, and the
; screen it points at is forty bytes ANTIC reads as well. No instruction in
; this program names either of them, because nothing in this program looks at
; them: the hardware does. One word says so, and it stands on the display
; list alone — the forty bytes are reached from it, the way anything else is
; reached from what names it.
.section root
displayList
        .byte $70, $70, $70, $42
        .word row
        .byte $41
        .word displayList
.ends

.section
row
        .res 40
.ends

.proc entry
        rts
.endp
```
<!-- end -->

<!-- map 13-roots/kept -->
```
NGA memory map

phase phase0 (0)
  zero page: 0 of 128 bytes
  memory:    50 of 40960 bytes
  $2000-$2008  screen.displayList section root       phase0
  $2009-$2030  screen.row         section            phase0
  $2031-$2031  screen.entry       proc               phase0
```
<!-- end -->

The word goes on the display list alone. The forty bytes come back with it,
reached the way anything is reached from what names it, and the map says which
Section was kept for a reason no Chunk gives.

## What the hardware follows

A Section is marked `root` where it is declared, which works for memory that
simply sits there. An address handed to the OS is handed over somewhere, and
that is where the reader will look for it:

<!-- include 13-roots/vbi/tick.asm tag=handler -->
```asm
; The OS calls this sixty times a second once its address is in the vector,
; and no instruction in the program ever names it.
.proc onVerticalBlank
        inc ticks
        jmp XITVBV
.endp
```
<!-- end -->

<!-- include 13-roots/vbi/tick.asm tag=install -->
```asm
; `SETVBV` takes the stage in `A` and the routine in `X` and `Y`, so the
; address is taken twice and each taking says who follows it. The hardware
; does, and `.root` is how that is written.
.proc entry
        lda #0
        sta ticks

        lda #6                          ; the deferred vertical blank
        .root
        ldx #>onVerticalBlank
        .root
        ldy #<onVerticalBlank
        jsr SETVBV

@wait   lda ticks                       ; a second of them, and then say so
        cmp #50
        jcc @wait

        ldy #wordEnd - word - 1
@copy   lda word,y
        sta (SAVMSC),y
        dey
        bpl @copy
@stop   jmp @stop
.endp
```
<!-- end -->

`.root` declares that the address goes to the hardware. What it names becomes
a Root from there: kept by Prune, active in every state for Trace, and never
the target of one of the program's own jumps — which is the difference from
`.own`, and why a statement may not carry both.

<!-- map 13-roots/vbi phase=phase0 rows=tick -->
```
phase phase0 (0)
  zero page: 129 of 256 bytes
  memory:    15660 of 56576 bytes
  ...
  $0080-$0080  tick.ticks           section            phase0
  ...
  $2000-$2004  tick.onVerticalBlank proc root          phase0
  $2005-$200B  tick.word            section            phase0
  $200C-$202B  tick.entry           proc               phase0
  ...
```
<!-- end -->

`onVerticalBlank` is a Root, and no attribute on it says so; the two `.root`
lines in the other Proc are what put it there. On the machine the program
waits for the handler to have run fifty times and then writes to the screen,
so the words appearing at all is the handler having been kept and having run.

This is not new machinery arriving at chapter thirteen. The rows that map
leaves out are the OS's, and every one of them is a Root — `atari/os.asm` has
marked them so since chapter three, because the hardware and the OS reach that
memory with no Reference anywhere in any program, and without the word not one
of them would survive Prune.
