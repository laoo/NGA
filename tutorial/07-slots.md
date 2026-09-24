# Slots

Chapter four refused a reference from one Phase to a Section of another, and
the answer was to stop writing it. Resident code has the same problem the other
way round, and there the answer will not do: a routine that is in memory in
every Phase has to reach what differs between them. That is what a **Slot** is.

## The shape of a screen, and what fills it

Every screen here is a title and then something under it. A Module of its own
knows that much and nothing else.

<!-- include 07-slots/per-phase/run.asm tag=screen -->
```asm
.proc showScreen
        ldy #0
@count  lda (title),y           ; the Phase's title, as long as it turns out
        beq @print
        iny
        bne @count
@print
        tya
        ldx title
        ldy title+1
        jsr printLine
        jsr draw
        rts
.endp
```
<!-- end -->

Neither the title nor what goes under it is in this Module, and both come from
whichever Phase is running, which it never learns. They are declared above it:

<!-- include 07-slots/per-phase/run.asm tag=slot -->
```asm
.export title, draw, showScreen

.slot title, pointer, zeropage
.slot draw, vector
```
<!-- end -->

`.slot NAME, binding` declares a Symbol with at most one live definition per
Phase. The Binding is required and is not guessed, because it decides what a
use compiles to and what the Cell behind it holds. `vector` is a `jmp` and an
address, so `jsr draw` is a call into the Cell. `pointer` is an address alone,
which is why `lda (title),y` reads through it and `ldx title` reads the low
byte of the address itself — and why a pointer Slot takes a placement, since
`(),y` wants the zero page.

Both names on the `.export` line are there because another Module needs them,
and for different reasons. `showScreen` is exported because the Phases call it.
`draw` is exported because the Phases **fill** it: a Slot is declared once and
implemented elsewhere, so every Module that says `.implements draw` has to see
the name, and without the export the tool says `draw` is not defined at the
`.implements` rather than at any use.

A Module says that it fills them wherever it is present. What fills a `vector`
is a Proc; what fills a `pointer` is anything with an address.

<!-- include 07-slots/per-phase/level.asm tag=fills -->
```asm
.implements title, levelTitle
.implements draw, levelDraw

.section
levelTitle
        .byte "LEVEL ONE", 0
.ends

.proc levelDraw
        .with level
        jsr drawMap
        rts
.endp
```
<!-- end -->

`intro.asm` does the same with a title of its own and a Proc that prints a
line. Neither Phase names the other, and `run.asm` names neither.

## What the tool made of it

<!-- map 07-slots/per-phase phase=level rows=nga.slots,run,level -->
```
phase level (1)
  zero page: 145 of 256 bytes
  memory:    16661 of 56576 bytes
  ...
  $0083-$0084  level.drawMap.at             temporary          level  shares with ngaDst ngaPtr portbDst
  ...
  $0085-$0085  level.drawMap.left           temporary          level  shares with ngaOffset ngaValue portbSize
  ...
  $008F-$0090  nga.slots.title              section            intro..level
  ...
  $20DF-$20F3  run.showScreen               proc               intro..level
  $20F4-$2118  level.drawMap                proc               level  waits in bank 0 at $0026 (copy, 39 bytes)
  $2119-$2122  level.levelTitle             section            level  waits in bank 0 at $004D (copy, 12 bytes)
  $2123-$2130  level.levelDraw              proc               level  waits in bank 0 at $0059 (copy, 16 bytes)
  $2131-$2136  level.levelStart             proc               level  waits in bank 0 at $0069 (copy, 8 bytes)
  ...
  $2226-$2228  nga.slots.draw               section            intro..level
  ...
  $4000-$4171  level.levelMap               section            level  in pane level (state 4)
  ...
```
<!-- end -->

Two Cells, and their Bindings are what they look like. `nga.slots.draw` is
three bytes of ordinary memory — `jmp` and an address, which `jsr draw` calls
into. `nga.slots.title` is two bytes of the zero page, an address and nothing
else, because that is what `lda (title),y` needs. Both stand in every Phase,
since resident code uses them; the tool added the Module the moment the program
declared a Slot.

<!-- map 07-slots/per-phase storage -->
```
storage
  bank 0: 113 of 16384 bytes
    $0000-$0025  frame intro -> level          live intro
    $0026-$004C  level.drawMap                copy (39 bytes)  live intro
    $004D-$0058  level.levelTitle             copy (12 bytes)  live intro
    $0059-$0068  level.levelDraw              copy (16 bytes)  live intro
    $0069-$0070  level.levelStart             copy (8 bytes)  live intro
  bank 1: 0 of 16384 bytes
  bank 2: 0 of 16384 bytes
  bank 3: 0 of 16384 bytes
```
<!-- end -->

The Frame is thirty-eight bytes where chapter six's was eighteen. Twelve of the
twenty are two more Sections to load, `levelTitle` and `levelDraw` being
Sections of `level` like any other. The remaining eight are the two **Cell
writes**, four bytes each: where the Cell is, and what this edge is to put in
it. The Transition routine performs them on its way, after the Sections and
before it jumps to the entered Phase's entry, and nothing else resident is
touched.

On the screen the two titles and the two bodies alternate, and the same two
instructions printed and called both times.

## What it refuses

At most one Implementation may be live in a Phase. A Module that is resident
implements the Slot in every Phase, so it may not implement what a Phase also
implements:

<!-- include 07-slots/two-draws/run.asm tag=second -->
```asm
; A default, for a Phase that has nothing of its own to draw. This Module is
; resident, so it is live in every Phase.
.implements draw, runNothing
```
<!-- end -->

<!-- diagnostics 07-slots/two-draws -->
```
error[NGA2252]: `draw` has two implementations live in phase `intro`: `introDraw` and `runNothing`
 --> intro.asm:3:1
  |
3 | .implements draw, introDraw
  | ^^^^^^^^^^^^^^^^^^^^^^^^^^^
note: `runNothing` implements it here
  --> run.asm:15:1
   |
15 | .implements draw, runNothing
   | ^^^^^^^^^^^^^^^^^^^^^^^^^^^^

error[NGA2252]: `draw` has two implementations live in phase `level`: `levelDraw` and `runNothing`
  --> level.asm:56:1
   |
56 | .implements draw, levelDraw
   | ^^^^^^^^^^^^^^^^^^^^^^^^^^^
note: `runNothing` implements it here
  --> run.asm:15:1
   |
15 | .implements draw, runNothing
   | ^^^^^^^^^^^^^^^^^^^^^^^^^^^^
```
<!-- end -->

There is no order of precedence and no default: which of two is meant is not
something a tool should decide, and a Phase that has nothing to draw says so
by drawing nothing.

## In C

The Slot is an `extern` that names nothing of the assembler's, and its Binding
is its type.

<!-- include 07-slots/in-c/run.ngc -->
```c
[[slot]] extern u8* const title;
[[slot]] extern void draw();

void showScreen()
{
  u8 left = 0;
  while ( title[left] != 0 )
    left += 1;
  printLine( (u16)title, left );
  draw();
}
```
<!-- end -->

Both Bindings come off the type, and neither is written down. `void draw()` is
a function, so the Slot is a `vector` and a call is the `jsr` into the Cell;
`u8* const title` is a pointer, so it is a `pointer` on the zero page and
`title[left]` is `lda (title),y`. The pointer is `const` because the Cell is
the Transition routine's to write and not the program's — and it is the one
`extern` in the subset that names nothing of the assembler's.

<!-- include 07-slots/in-c/intro.ngc -->
```c
[[implements(title)]] static const u8 introTitle[] = "INTRO";

static const u8 introText[] = "THE FIRST SCREEN";

[[implements(draw)]] void introDraw()
{
  printLine( (u16)introText, sizeof( introText ) - 1 );
}

void introStart()
{
  showScreen();
  [[transition(level)]] return;
}
```
<!-- end -->

`[[implements(...)]]` is `.implements`, and it stands on a function or on an
object at file scope according to what the Slot is. The Phase's own entry calls
the resident `showScreen`, which reads and calls back into the Phase, and no
name crosses between the two Phases in either direction.
