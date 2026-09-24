# Panes

Chapter five used a Bank as somewhere to **keep** things: the program read
storage through the Window and copied it into memory it could address all the
time. This chapter leaves the bytes where they are and runs against them there.

## The map stays in the Bank

The program is chapter five's, and the Project gains one line.

<!-- include 06-panes/in-a-bank/main.ngp -->
```
include "atari/130xe.ngp"

modules { "print.asm" "shared.asm" "intro.asm" "level.asm" }

panes in ext { level }

resident { print, shared }

phase intro { needs intro  then level  entry introStart }
phase level { needs level             entry levelStart }
entry intro

container xex
```
<!-- end -->

`panes in ext { level }` declares a **Pane**: a named set of Sections that one
switch shows together, in the Window `ext`. The solver gives it one state of
that Window for the whole run, as it gives a Section one address. A Section
joins it by saying so.

<!-- include 06-panes/in-a-bank/level.asm tag=section -->
```asm
.section in level
levelMap
        .byte "#####################################"
        .byte "#...................................#"
        .byte "#...................................#"
        .byte "#......########.....................#"
        .byte "#......#......#.....................#"
        .byte "#......#......#.....................#"
        .byte "#......########.....................#"
        .byte "#...................................#"
        .byte "#...................................#"
        .byte "#####################################"
levelMapEnd
.ends
```
<!-- end -->

## What that changed

<!-- map 06-panes/in-a-bank phase=level rows=level,shared -->
```
phase level (1)
  zero page: 143 of 256 bytes
  memory:    16623 of 56576 bytes
  ...
  $0083-$0084  level.drawMap.at             temporary          level  shares with ngaDst ngaPtr portbDst
  ...
  $0085-$0085  level.drawMap.left           temporary          level  shares with ngaOffset ngaValue portbSize
  ...
  $20DF-$2103  level.drawMap                proc               level  waits in bank 0 at $0012 (copy, 39 bytes)
  $2104-$2113  level.levelStart             proc               level  waits in bank 0 at $0039 (copy, 18 bytes)
  ...
  $4000-$4171  level.levelMap               section            level  in pane level (state 4)
  $4000-$403F  shared.spare                 section root       intro..level
  ...
```
<!-- end -->

The map stands at `$4000`, which chapter five refused — but it refused a
Section with a **Payload** there, and this one has none:

<!-- map 06-panes/in-a-bank storage -->
```
storage
  bank 0: 75 of 16384 bytes
    $0000-$0011  frame intro -> level          live intro
    $0012-$0038  level.drawMap                copy (39 bytes)  live intro
    $0039-$004A  level.levelStart             copy (18 bytes)  live intro
  bank 1: 0 of 16384 bytes
  bank 2: 0 of 16384 bytes
  bank 3: 0 of 16384 bytes
```
<!-- end -->

Seventy-five bytes where chapter five had four hundred and thirty-one. The
three hundred and seventy of the map are not in storage at all. A Pane's
Section with bytes is written into its Bank by the Container, once, when the
program is loaded, and no Transition copies it again: it is already at the
address it is read from, and the switch is what brings it there.

## Who may name what

The Bank is only at `$4000` while it is shown. The rest of the time those
addresses are base RAM, so code that reads `levelMap` without having shown the
Pane would read whatever base RAM holds. The tool therefore refuses the name,
not the read:

<!-- include 06-panes/outside/level.asm tag=outside -->
```asm
.proc levelStart
        lda levelMap            ; not under the Pane, so the Bank is not shown
        .with level
        jsr drawMap
@stop   jmp @stop
.endp
```
<!-- end -->

<!-- diagnostics 06-panes/outside -->
```
error[NGA2412]: `levelMap` is in pane `level`, and this code does not run with that pane shown: put the statement under a `.with`, or the code in a Proc declared `under` it
  --> level.asm:56:13
   |
56 |         lda levelMap            ; not under the Pane, so the Bank is not shown
   |             ^^^^^^^^
```
<!-- end -->

It refuses the *address* too, and not only the load — `#<levelMap` is as
refused as `lda levelMap`, since neither is worth anything unless the Bank is
there. So code that touches a Pane's Sections has to be code that runs with the
Pane shown, and that is what the `under` attribute declares:

<!-- include 06-panes/in-a-bank/level.asm tag=draw -->
```asm
; A Trampoline: it stands in base RAM, out of the Window, but runs with `level`
; shown — which is what lets it name what the Pane holds. Chapter five's loop,
; moved here whole.
.proc drawMap, under level
at      .ztemp 2
left    .ztemp 1

        lda #<levelMap
        sta at
        lda #>levelMap
        sta at+1
        lda #( levelMapEnd - levelMap ) / levelWidth
        sta left
@row
        ldx at
        ldy at+1
        lda #levelWidth
        jsr printLine
        clc
        lda at
        adc #levelWidth
        sta at
        bcc @counted
        inc at+1
@counted
        dec left
        bne @row
        rts
.endp

.proc levelStart
        .with level
        jsr drawMap
@stop   jmp @stop
.endp
```
<!-- end -->

`.proc drawMap, under level` is a **Trampoline**: it stands in base RAM, out of
the Window, so the switch does not take it away, and it runs with `level`
shown, so it may name what the Pane holds. `.with level` over the `jsr` is what
shows the Pane and puts the base back afterwards — a `.with` covers the one
statement that follows it, and the statement it covers here is the call.

## Two Sections at one address

Look at the first map again. `shared.spare` and `level.levelMap` both begin at
`$4000`, both in Phase `level`, and neither moved. Chapter four said two
Sections may share bytes when they are never in memory at once; that was half
of the rule. This is the other half. A **View** is one state of one Window, and
two Sections collide only when their Phases overlap **and** their Views can be
seen together. A Section in no Pane is in the Window's base state; the map is
in a Bank; one switch stands between them, so the same address serves both.

`shared.spare` is in the program for no reason other than to show it. Nothing
names it, which is why it is `root`, and nothing would miss it.

## More than one of the same thing

A Pane is one Bank. `NAME[N]` declares a **family**: N Panes of one layout on
consecutive Banks, and every Section `in` it stands in each member at the
same address.

<!-- include 06-panes/a-family/main.ngp -->
```
include "atari/130xe.ngp"

modules { "print.asm" "intro.asm" "rows.asm" }

panes in ext { slots[3] }

resident { print }

phase intro { needs intro  then level  entry introStart }
phase level { needs rows              entry levelStart }
entry intro

container xex
```
<!-- end -->

<!-- include 06-panes/a-family/rows.asm tag=family -->
```asm
; One layout, three times. `slotRow` stands at the same address in every
; member of the family, so the two routines below name it once and reach a
; different Bank's bytes depending on which member is shown.
.section in slots
slotRow
        .res rowWidth
.ends

; Fills this member's row with `mark`. The byte comes through memory and not
; through a register: a `.with` shows the Window before the statement it
; covers, and showing it is the driver's code, which keeps nothing.
.proc fillRow, in slots
        ldy #rowWidth - 1
        lda mark
@byte   sta slotRow,y
        dey
        bpl @byte
        rts
.endp

.proc showRow, in slots
        ldx #<slotRow
        ldy #>slotRow
        lda #rowWidth
        jsr printLine
        rts
.endp
```
<!-- end -->

<!-- map 06-panes/a-family phase=level rows=rows -->
```
phase level (1)
  zero page: 145 of 256 bytes
  memory:    16273 of 56576 bytes
  ...
  $0088-$0089  rows.mark                    section            level
  ...
  $20DF-$212D  rows.levelStart              proc               level  waits in bank 0 at $000C (copy, 81 bytes)
  ...
  $4000-$4024  rows.slotRow                 section            level  in pane slots (state 2)
  $4025-$402F  rows.fillRow                 proc               level  in pane slots (state 2)
  $4030-$4039  rows.showRow                 proc               level  in pane slots (state 2)
  ...
```
<!-- end -->

One row and two routines, each at one address, and the map names `state 2` for
all three — which is the **first** member's. The other two members are the
states after it and no row says so, because a Pane's Sections are not in
storage: the Container writes them into each member's Bank when the program is
loaded, once per member, and no Transition copies them again.

<!-- include 06-panes/a-family/rows.asm tag=use -->
```asm
; Two ways of naming a member. `slots + n` is one known where it is written;
; `slots, x` is whichever member's state is in `X`, which is how a loop walks
; them. The family's name on its own is the first member's state, so the
; counter starts there and `slots + 3` is one past the last.
.proc levelStart
        lda #'A'
        sta mark
        .with slots + 0
        jsr fillRow
        lda #'B'
        sta mark
        .with slots + 1
        jsr fillRow
        lda #'C'
        sta mark
        .with slots + 2
        jsr fillRow

        lda #slots
        sta which
@show
        ldx which
        .with slots, x
        jsr showRow
        inc which
        lda which
        cmp #slots + 3
        bne @show
@stop   jmp @stop
.endp
```
<!-- end -->

On the machine that puts three rows on the screen, `AAA…`, `BBB…` and `CCC…`.
Every one of them was written by the same `sta slotRow,y` and read by the same
`printLine` call at the same address; what differed each time was which Bank
was underneath.

That is what a family is for, and it is also its limit. The Sections are
declared **once**, so a Section with bytes has the same bytes in every member —
N copies of one layout, not N different things. Three different level maps are
three Panes, not a family of three; three working areas that code addresses
identically are the family.

The code that touches them has to stand **in** the family, which is where this
differs from `level` above. `under` names a Pane and not a family, since which
member is shown is known only at run time, so there is no Trampoline to
declare: `fillRow` and `showRow` are Procs `in slots`, standing in every member
beside the row they read, called under a `.with` that has shown one.

## In C

The Section takes the attribute, and the block takes the switch.

<!-- include 06-panes/in-c/level.ngc -->
```c
[[in(level)]] static const u8 levelMap[] =
  "#####################################"
  "#...................................#"
  "#...................................#"
  "#......########.....................#"
  "#......#......#.....................#"
  "#......#......#.....................#"
  "#......########.....................#"
  "#...................................#"
  "#...................................#"
  "#####################################";

void levelStart()
{
  [[with(level)]]
  {
    u16 at = (u16)levelMap;
    for ( u8 left = 10; left != 0; --left )
    {
      printLine( at, 37 );
      at += 37;
    }
  }
  for ( ;; )
  {
  }
}
```
<!-- end -->

`[[with(level)]]` stands over a **block** where the assembler's `.with` stands
over one statement, so the loop is inside the switch rather than the other way
round and the Window is shown once for all ten rows. The compiler writes the
block out as a macro under a `.with`, which is the Trampoline the assembler
version declares by hand. What it costs is the same; what it takes to say is
less.
