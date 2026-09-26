# Phases

A program that does not need all of itself at once can be written in parts that
take turns in memory. NGA calls a part a **Phase**, and works out from the
program which Sections belong to which.

## A program in two parts

Two things on the screen, one after the other. What puts a line there stands in
a Module of its own, because both parts need it.

<!-- include 04-phases/two-phases/print.asm -->
```asm
; The one thing both Phases need: a line on the screen, through the character
; I/O the OS opens on channel 0 before a program starts. `PUTREC` writes a
; record, so it ends the line itself.

CIOV   = $E456
ICCOM  = $0342
ICBAL  = $0344
ICBLL  = $0348
PUTREC = 9

; Both Phases call it, so both have to see the name.
.export printLine

; Where the line stands, and how long it is. Declaring where the two arguments
; are is what lets a Module of C call this one as a function.
.proc printLine
        .declare arg xy
        .declare arg a

        stx ICBAL
        sty ICBAL+1
        sta ICBLL
        lda #0
        sta ICBLL+1
        lda #PUTREC
        sta ICCOM
        ldx #0
        jsr CIOV
        rts
.endp
```
<!-- end -->

Each part holds what it has to say and passes it on.

<!-- include 04-phases/two-phases/intro.asm -->
```asm
.section
introText
        .byte "INTRO"
introTextEnd
.ends

.proc introStart
        ldx #<introText
        ldy #>introText
        lda #introTextEnd - introText
        jsr printLine
        .transition level
.endp
```
<!-- end -->

`level.asm` is the part the program goes on to, and it has something to carry:
a map, and a procedure that prints it a row at a time. The whole of it is on
the screen when the Phase has started, so a load that fell short would show
rather than being something to go and check.

<!-- include 04-phases/two-phases/level.asm -->
```asm
; What the Phase is for: a map of ten rows, which is what waits in storage
; while the program is in `intro`. Thirty-seven characters is what a row of
; the screen holds once the OS's left margin is taken off.

levelWidth = 37

.section
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

; A row at a time. `at` and `left` are Temporaries of this Proc, as chapter
; two's were, and `printLine` keeps neither of them.
.proc levelStart
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
@stop   jmp @stop
.endp
```
<!-- end -->

## The project says which part is which

<!-- include 04-phases/two-phases/main.ngp -->
```ngp
include "atari/800xl.ngp"

modules { "print.asm" "intro.asm" "level.asm" }

resident { print }

phase intro { needs intro  then level  entry introStart }
phase level { needs level             entry levelStart }
entry intro

container atr
```
<!-- end -->

A `phase` block names the Modules the Phase **needs**, the Phases it may go
**then** to, and the Label it starts at. `entry` says which Phase the program
starts in. `resident` names Modules that are in memory in every Phase, and
`print` is one: both Phases call it.

Two lines are new. `include` takes the machine from the tool's library, which
here is an Atari 800XL with a disk drive — the Regions, the registers, and the
driver that reads the diskette, none of which this Project writes. And
`container atr` is that diskette: the program now has a part that has to wait
somewhere until it is wanted, and a diskette is somewhere.

## What the tool made of it

<!-- map 04-phases/two-phases phase=intro -->
```
phase intro (0)
  zero page: 144 of 256 bytes
  memory:    16487 of 62592 bytes
  $0000-$007F  os.osZero                    section root       intro..level
  $0080-$0083  disk.diskAt                  section            intro..level
  $0084-$0085  disk.diskDst                 temporary          intro..level  shares with ngaDst ngaPtr levelStart.at
  $0084-$0085  nga.transition.ngaDst        temporary          intro..level  shares with diskDst ngaPtr levelStart.at
  $0084-$0085  nga.transition.ngaPtr        temporary          intro..level  shares with diskDst ngaDst levelStart.at
  $0086-$0087  disk.diskLeft                temporary          intro..level  shares with ngaOffset ngaValue levelStart.left
  $0086-$0087  nga.transition.ngaOffset     temporary          intro..level  shares with diskLeft ngaValue levelStart.left
  $0086-$0087  nga.transition.ngaValue      temporary          intro..level  shares with diskLeft ngaOffset levelStart.left
  $0088-$0089  nga.transition.ngaEntry      temporary          intro..level
  $008A-$008B  nga.transition.ngaFramePos   temporary          intro..level
  $008C-$008C  nga.transition.ngaWanted     temporary          intro..level
  $008D-$008D  nga.transition.ngaCount      temporary          intro..level
  $008E-$008E  nga.transition.ngaFrameUnit  temporary          intro..level
  $008F-$008F  nga.transition.ngaUnit       temporary          intro..level
  $0200-$06FF  os.osRam                     section root       intro..level
  $0700-$0705  nga.boot.ngaBootRecord       section root       intro..level
  $0706-$07B9  nga.boot.ngaBootLoad         proc root          intro..level
  $087D-$087F  nga.boot.ngaBootImage        section root       intro..level
  $0880-$0892  disk.diskOpenStream          proc               intro..level
  $0893-$08AB  disk.diskReadByte            proc               intro..level
  $08AC-$08D3  disk.diskFill                proc               intro..level
  $08D4-$08EC  print.printLine              proc               intro..level
  $08ED-$08F1  intro.introText              section            intro
  $08F2-$08FB  nga.transition.ngaFrameOpen  proc               intro..level
  $08FC-$08FC  nga.cell.ngaCurrentPhase     section            intro..level
  $08FD-$08FD  nga.boot.ngaBootIdle         proc root          intro..level
  $08FE-$08FE  nga.transforms.ngaShowBases  proc               intro..level
  $0900-$09FF  disk.diskBuffer              section            intro..level
  $0A00-$0A2C  disk.diskCopy                proc               intro..level
  $0A2D-$0A3E  intro.introStart             proc               intro
  $0BC6-$0C0E  nga.transition.ngaTransition proc               intro..level
  $0C0F-$0C9F  nga.transition.ngaEnter      proc               intro..level
  $0CA0-$0CA9  nga.transition.ngaFrameSkip  proc               intro..level
  $0CAA-$0CB1  nga.transforms.ngaTransform  proc               intro..level
  $C000-$CFFF  os.osRomLow                  section root       intro..level
  $D800-$FFFF  os.osRomHigh                 section root       intro..level
```
<!-- end -->

<!-- map 04-phases/two-phases phase=level rows=print,level -->
```
phase level (1)
  zero page: 144 of 256 bytes
  memory:    16873 of 62592 bytes
  ...
  $0084-$0085  level.levelStart.at          temporary          level  shares with diskDst ngaDst ngaPtr
  ...
  $0086-$0086  level.levelStart.left        temporary          level  shares with diskLeft ngaOffset ngaValue
  ...
  $08D4-$08EC  print.printLine              proc               intro..level
  ...
  $0A2D-$0B9E  level.levelMap               section            level  waits in unit 0 at $0010 (copy, 372 bytes)
  $0B9F-$0BC5  level.levelStart             proc               level  waits in unit 0 at $0184 (copy, 41 bytes)
  ...
```
<!-- end -->

Most of that first map is not the program. `os.*` is the Atari's own software,
which the variant declares so the solver knows those addresses are taken.
`disk.*` is the driver that reads the diskette. `nga.*` is what the tool added
once the Project had an edge: the routine a Phase change calls, the byte
holding which Phase is current, and the boot record the diskette starts from.
None of it differs between the two Phases, so the second map leaves it out —
`...` stands where those rows were — and keeps the three the program wrote.

The last column is the Phases a Section is in. Everything saying `intro..level`
stands at one address in both maps — `print.printLine` among them, which is
what `resident` bought. The rest says `intro` or `level`, and an address a
Section of one Phase holds is held in the other map by a Section of the other,
or by nothing at all. The two Phases have the same bytes because they are never
both in memory, and the rows of `level` say where the ones not in memory are
instead.

Which Section is paired with which is the solver's business and follows no
order of yours; it pairs them by what fits.

The two Phases need not be the same size, and here they are not: `level` holds
a map and `intro` holds five characters of text. So the map of `intro` has a hole in
it, as wide as the difference. Nothing resident can fill it — a Section present
in both Phases stands at one address in each, and in `level` that address is in
use — and only another Section of `intro` could, of which there is none. The
span a Phase is given is as wide as the widest Phase needs.

## What waits on the diskette

<!-- map 04-phases/two-phases storage -->
```
storage
  unit 0: 429 of 65536 bytes
    $0000-$000F  frame intro -> level          live intro
    $0010-$0183  level.levelMap               copy (372 bytes)  live intro
    $0184-$01AC  level.levelStart             copy (41 bytes)  live intro
  unit 1: 0 of 65536 bytes
  unit 2: 0 of 65536 bytes
```
<!-- end -->

Sixty-three bytes, and the whole of the second Phase is in them. A Section
that emits bytes and is needed in the Phase entered but not in the Phase left
has a **Payload**: a copy in storage, which a Phase change reads back to the
address the map gives it. The **Frame** in front of them is the tool's own:
which Sections this edge loads, where each waits, and where each lands.

A **unit** is the model's word for a piece of storage, and it is the model's
and not the medium's: the Project says how many there are and how big one is,
a Payload is named by a unit and an offset in it, and the driver turns that
into whatever the hardware understands. Here the variant asked for units of
65536 bytes, and the driver reads one as 256 sectors from sector 4 on, the
first three being the boot record. So `unit 0 at $0010` is sixteen bytes into
the fourth sector of the diskette, and the other two units are sectors the
program never reaches.

`live intro` says how long those bytes are needed for. They are read while the
program is in `intro`, so nothing else may use them until then.

## The statement

`.transition level` is what ends `introStart`. It compiles to nine bytes: a
call into the routine, then the Phase entered and, for each Phase this code
stands in, where that edge's Frame waits — the code cannot know at run time
which Phase it is in, so it carries an entry for each. The routine reads them
through the return address, loads what the Frame lists, and jumps to
`levelStart`. It does not return, and nothing of `intro` is in memory
afterwards.

A Phase change is therefore a statement and a table, not a call to a loader you
wrote. What it reads and where from is decided when the program is built.

## What it refuses

The two Phases are not in memory together, so code in one may not name a
Section of the other. If `introStart` reads `levelMap`:

<!-- include 04-phases/crossed/intro.asm tag=cross -->
```asm
.proc introStart
        ldx #<introText
        ldy #>introText
        lda #introTextEnd - introText
        jsr printLine
        lda levelMap            ; the Phase this belongs to has not loaded it
        .transition level
.endp
```
<!-- end -->

<!-- diagnostics 04-phases/crossed -->
```
error[NGA2414]: reference to `levelMap`, which is not in memory in phase `intro` while `introStart` is
  --> intro.asm:13:13
   |
13 |         lda levelMap            ; the Phase this belongs to has not loaded it
   |             ^^^^^^^^
```
<!-- end -->

The finding names both sides and both Phases. This is checked for every
reference in the program, which is what makes Phases something the tool
enforces rather than something you remember.

## What a Payload costs

`level.levelMap` is 370 bytes where it runs and waits in storage as 372 — its
own size, and two more for the length in front of it. One line in the Project
changes that, and nothing else about the program does:

<!-- include 04-phases/compressed/main.ngp tag=transform -->
```ngp
transform zx0 { level.levelMap }
```
<!-- end -->

<!-- map 04-phases/compressed storage -->
```
storage
  unit 0: 86 of 65536 bytes
    $0000-$000F  frame intro -> level          live intro
    $0010-$002C  level.levelMap               zx0 (29 bytes)  live intro
    $002D-$0055  level.levelStart             copy (41 bytes)  live intro
  unit 1: 0 of 65536 bytes
  unit 2: 0 of 65536 bytes
```
<!-- end -->

Twenty-nine bytes. `transform` names the Sections whose Payload is turned into
another form on its way into storage and back on its way out; a Section named
in none is copied, which is a transform like the others and not the absence of
one. The Section does not change and neither does the program: the decoder came
with the machine's variant, the tool numbered it, and the Frame says which
number each block uses. What the screen shows is the same map either way, which
is the only check worth having on a decoder.

## In C

The Phases are two `.ngc` Modules, and the Project changes in one line.

<!-- include 04-phases/in-c/intro.ngc -->
```c
static const u8 introText[] = "INTRO";

void introStart()
{
  printLine( (u16)introText, sizeof( introText ) - 1 );
  [[transition(level)]] return;
}
```
<!-- end -->

`[[transition(level)]]` stands on a bare `return;` and is written as
`.transition level` in its place. The rest is the C of chapter one: a text at
file scope, and a call.

`print.asm` does not change at all. C can call it because of the two lines in
front of its body — `.declare arg xy` and `.declare arg a` say where the
arguments stand, which is all a caller of either language needs to know.
