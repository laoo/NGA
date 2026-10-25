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
error[NGA4606]: the address of `handler` is taken and nothing says who follows it: `.own` if this section does, `.own NAME` if another does, `.root HOLDER` if the hardware does
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

An address of data needs none of this. A table taken to read or write through,
`.word colors`, is fine with nothing said, even where it sits in a `.section`
next to the code that uses it. What counts is the Label: one that stands at a
`.byte`, a `.word` or a `.res` names data.

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

phase (implicit) (0)
  zero page: 1 of 128 bytes
  memory:    68 of 40960 bytes
  $0080-$0080  pick.entry.band temporary          (implicit)  shares with entry.best
  $0080-$0080  pick.entry.best temporary          (implicit)  shares with entry.band
  $2000-$2004  pick.input      section            (implicit)
  $2005-$2005  pick.out        section            (implicit)
  $2006-$2043  pick.entry      proc               (implicit)
```
<!-- end -->

<!-- map 13-roots/by-hand -->
```
NGA memory map

phase (implicit) (0)
  zero page: 2 of 128 bytes
  memory:    68 of 40960 bytes
  $0080-$0080  pick.entry.best temporary          (implicit)
  $0081-$0081  pick.entry.band temporary          (implicit)
  $2000-$2004  pick.input      section            (implicit)
  $2005-$2005  pick.out        section            (implicit)
  $2006-$2043  pick.entry      proc               (implicit)
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

<!-- include 13-roots/dropped/os.asm -->
```asm
; The machine's OS keeps its variables in page two, and reads and writes them
; on every interrupt. This program never touches them, so no instruction in it
; names them.
.section absolute at $0200
osVars
        .res $100
.ends
```
<!-- end -->

<!-- include 13-roots/dropped/program.asm -->
```asm
; A buffer of the program's own, placed wherever the solver finds room, and the
; code that uses it.
.section
buffer
        .res $100
.ends

.proc entry
        lda #0
        sta buffer
        rts
.endp
```
<!-- end -->

<!-- diagnostics 13-roots/dropped -->
```
warning[NGA4505]: `osVars` is pinned at $0200 and nothing reaches it, so it is dropped; mark it `root` if the hardware reads it
 --> os.asm:4:1
  |
4 | .section absolute at $0200
  | ^^^^^^^^
```
<!-- end -->

<!-- map 13-roots/dropped -->
```
NGA memory map

phase (implicit) (0)
  zero page: 0 of 128 bytes
  memory:    262 of 48640 bytes
  $0200-$02FF  program.buffer section            (implicit)
  $0300-$0305  program.entry  proc               (implicit)
```
<!-- end -->

The OS's variables are gone. Prune keeps what is reached from a Root and drops
the rest, and nothing in the program reaches `osVars`. The solver then sees
page two as free memory and puts `buffer` there, on top of the variables the OS
keeps writing. The warning is there because of the pin: a Section pinned to an
address is usually there because something expects it there.

Which memory something outside the program uses is a fact about the machine.
Nothing in the code says it, so it is declared:

<!-- include 13-roots/kept/os.asm -->
```asm
; The machine's OS keeps its variables in page two, and reads and writes them
; on every interrupt. Nothing in this program reaches them, so `root` says that
; something outside the program does.
.section absolute at $0200, root
osVars
        .res $100
.ends
```
<!-- end -->

<!-- map 13-roots/kept -->
```
NGA memory map

phase (implicit) (0)
  zero page: 0 of 128 bytes
  memory:    518 of 48640 bytes
  $0200-$02FF  os.osVars      section root       (implicit)
  $0300-$03FF  program.buffer section            (implicit)
  $0400-$0405  program.entry  proc               (implicit)
```
<!-- end -->

`osVars` stays in page two and `buffer` moves to page three. The map marks the
Section `root`, since no Chunk reaches it.

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
; address is taken twice and each taking says who follows it. The deferred
; vertical blank does, and `.root vbd` is how that is written.
.proc entry
        lda #0
        sta ticks

        lda #7                          ; the deferred vertical blank
        .root vbd
        ldx #>onVerticalBlank
        .root vbd
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

`.root vbd` declares that the address goes to the hardware, and names which
hardware takes it. `vbd` is the deferred vertical blank, one of the **Holders**
the machine declares with `holder` in its `target` block. A Holder is hardware
that holds one Section at a time, and `atari/130xe.ngp` declares nine: `dlist`,
`dli`, `vbi`, `vbd`, `nmi`, `irq`, `chbase`, `pmbase` and `dosini`. A `.root`
always names one, and a name that is not one of them is an error.

What a Holder is handed becomes a Root from there: kept by Prune, active in
every state for Trace, and never the target of one of the program's own jumps.
That is the difference from `.own`, and why a statement may not carry both.

<!-- map 13-roots/vbi phase=(implicit) rows=tick -->
```
phase (implicit) (0)
  zero page: 129 of 256 bytes
  memory:    15719 of 40192 bytes
  ...
  $0080-$0080  tick.ticks            section            (implicit)
  ...
  $201F-$2023  tick.onVerticalBlank  proc root          (implicit)
  $2024-$202A  tick.word             section            (implicit)
  $202B-$204A  tick.entry            proc               (implicit)
  ...
```
<!-- end -->

`onVerticalBlank` is a Root, and no attribute on it says so; the two `.root vbd`
lines in the other Proc are what put it there. On the machine the program
waits for the handler to have run fifty times and then writes to the screen,
so the words appearing at all is the handler having been kept and having run.

This is not new machinery arriving at chapter thirteen. The rows that map
leaves out are the OS's, and every one of them is a Root — `atari/osram.asm`
and `atari/osrom.asm` have marked them so since chapter three, because the hardware and the OS reach that
memory with no Reference anywhere in any program, and without the word not one
of them would survive Prune.

## How long the hardware holds it

A Holder lets go of a Section when it is given another one. ANTIC reads one
display list, and an interrupt vector points at one handler. `.root dlist`
hands ANTIC a display list:

```asm
        .root dlist
        lda #<titleList
        sta SDLSTL
        .root dlist
        lda #>titleList
        sta SDLSTL+1
```

From here ANTIC holds `titleList`, and it keeps holding it until the program
hands `dlist` another display list or writes `.release dlist`. The tool follows
this through the code, across `.transition`, and keeps `titleList` and whatever
it names, such as the screen memory it points at, in place in every Phase where
ANTIC may still read it. A Phase that needs nothing of the title screen still
keeps its display list if it was entered while ANTIC was showing it.

The tool follows each path separately. If one path hands over one list before
one `.transition` and another path hands over a second list before a different
one, each Phase entered keeps only its own. A `.release` frees the Section only
on the paths that pass through it.

What a Holder holds is a whole Section. A display list that shares a Section
with another one also holds whatever the other one names, so give a list you
hand over a Section of its own.
