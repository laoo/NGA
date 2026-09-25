# Banks and Windows

A diskette keeps storage out of the address space: a unit is a sector, and
nothing of it is anywhere until the driver reads it. A machine with banked
memory puts storage *in* the address space, a piece at a time, at an address
the rest of the program cannot use while it is there. The mechanism is the one
chapter four built. What changes is what it costs.

## The same program, another machine

Chapter four's program, unchanged. `print.asm`, `intro.asm` and `level.asm`
are the same files; so are the Phases, the `resident` line and the
`.transition`. Two lines of the Project differ:

<!-- include 05-banks-and-windows/banked/main.ngp -->
```ngp
include "atari/130xe.ngp"

modules { "print.asm" "intro.asm" "level.asm" }

resident { print }

phase intro { needs intro  then level  entry introStart }
phase level { needs level             entry levelStart }
entry intro

container xex
```
<!-- end -->

`include` names an Atari 130XE rather than an 800XL with a drive, and the
Container is a `.xex` rather than a diskette. The second is not a free choice:
a machine says what it takes, and this one takes no `.atr` — it has no drive in
its description, and its storage is not something a Container could write
sectors of.


## The machine

The variant is worth reading in full, since every `include` so far has stood
for one.

<!-- include ../lib/atari/130xe.ngp -->
```ngp
; An Atari 130XE: four extended Banks as a unit set, the Window PORTB brings
; one of them into, the Window over the OS ROM, every Region of the address
; space, and the Modules a program on this machine needs. What PORTB takes to
; show a state is the driver's and not here, and the OS is a Module rather than
; a Region because it occupies memory for a while rather than being a truth
; about addresses.
target {
  cpu "6502"

  ; DOS loads a `.xex`, and a Project naming any other Container is refused
  ; here rather than at the writer.
  containers xex

  units  extension 4
  window ext  $4000 .. $7FFF  views main, extension  base main
  window os   $C000 .. $CFFF, $D800 .. $FFFF  views rom, ram  base rom
}
storage { units extension }

target {
  region ram    $0000 .. $CFFF  ram
  region        $D800 .. $FFFF  ram
  region stack  $0100 .. $01FF  reserved
  region io     $D000 .. $D7FF  register

  ; This machine takes a `.xex` and nothing else, and a `.xex` is loaded by a
  ; DOS, which stays where a DOS of the 2.x family stays. A Project whose DOS
  ; takes more says so with a `reserved` Region of its own, which narrows this
  ; one further.
  region dos    $0700 .. $1FFF  reserved

  ; What the driver writes. A Project that wants more of the hardware by name
  ; declares it, as it declares the memory its DOS holds.
  register PORTB  $D301
}

; The OS, the driver for this storage and the decoders the tool ships, listed
; as a file of the Project's own would be. Every Transition calls the driver
; and the decoders, so they are resident; a decoder nothing uses is dropped.
modules { "atari/os.asm"  "atari/charsets.asm"  "atari/portb.asm"  "stream/zx0.asm" }
resident { os, charsets, portb, zx0 }
```
<!-- end -->

One of its Regions is not the machine's doing. A `.xex` is loaded by a DOS, and
a DOS of the 2.x family stays below `$2000` for as long as the program runs, so
`region dos $0700 .. $1FFF reserved` keeps the solver out of it — the
`reserved` of chapter three, used for the first time. The variant may say it
because it takes no other Container; a Project whose DOS takes more narrows it
further with a `reserved` Region of its own.

`units extension 4` declares a **unit set**: four units the hardware has, known
by number and by nothing else. `storage { units extension }` says those units
are the storage, so a unit is a **Bank** — which is what a Bank is, and why the
map calls it that here and called it a unit on the diskette.

`window ext $4000 .. $7FFF views main, extension base main` declares a
**Window**: a set of address ranges the hardware switches as one, and the
states it can show. `main` is base RAM, `extension` is the four Banks, one
state each, and `base main` says which state is held when nothing has switched.
The Window names no register. What it takes to show a state is the driver's,
and `atari/portb.asm` is the driver that knows.

## What the tool did differently

<!-- map 05-banks-and-windows/banked storage -->
```
storage
  bank 0: 431 of 16384 bytes
    $0000-$0011  frame intro -> level          live intro
    $0012-$0185  level.levelMap               copy (372 bytes)  live intro
    $0186-$01AE  level.levelStart             copy (41 bytes)  live intro
  bank 1: 0 of 16384 bytes
  bank 2: 0 of 16384 bytes
  bank 3: 0 of 16384 bytes
```
<!-- end -->

Four Banks of sixteen kilobytes, where the diskette had units of sixty-four and
three of them. The Frame is eighteen bytes and was sixteen: an edge carries the
state every Window is to show in the Phase entered, and this machine has two
Windows where the diskette had none.

<!-- map 05-banks-and-windows/banked phase=level rows=portb,nga.transforms -->
```
phase level (1)
  zero page: 143 of 256 bytes
  memory:    16545 of 56576 bytes
  ...
  $0080-$0082  portb.portbPtr               section            intro..level
  ...
  $0083-$0084  portb.portbDst               temporary          intro..level  shares with ngaDst ngaPtr levelStart.at
  ...
  $0085-$0086  portb.portbSize              temporary          intro..level  shares with ngaOffset ngaValue levelStart.left
  ...
  $0087-$0087  portb.portbRun               temporary          intro..level  shares with ngaUnit
  ...
  $2000-$2004  portb.portbValues            section            intro..level
  $2005-$2010  portb.portbSelect            proc               intro..level
  $2011-$202C  portb.portbOpenStream        proc               intro..level
  $202D-$2042  portb.portbReadByte          proc               intro..level
  $2043-$2053  portb.portbNextUnit          proc               intro..level
  $2054-$20C5  portb.portbCopy              proc               intro..level
  ...
  $2367-$2380  nga.transforms.ngaShowBases  proc               intro..level
  $2381-$2390  nga.transforms.ngaRestore    proc root          intro..level
  $2391-$2391  nga.transforms.ngaLoadUnit   section root       intro..level
  $2392-$2398  nga.transforms.ngaLoadMap    proc root          intro..level
  $2399-$23A0  nga.transforms.ngaTransform  proc               intro..level
  ...
```
<!-- end -->

Where the map held `disk.*` it now holds `portb.*`: the same roles, a table of
register values in place of a sector buffer, and no call into the OS at all.
Its scratch shares bytes with the routine's and with `levelStart`'s. Nothing
declared that; it is chapter two's rule applied to Modules written separately.

What is new are three Procs the tool generated for this Container and not for
the other. A `.xex` is loaded into memory by a DOS, and a Bank *is* memory — so
the file fills the Banks by switching each in through the driver and writing to
the Window, then puts base RAM back before the program starts. `ngaLoadUnit`
and `ngaLoadMap` are what the loader calls to switch, `ngaRestore` is what puts
the base back, and `ngaShowBases` — one byte of `rts` on the diskette — is a
real Proc here, because after every Transition there is a base to show again.

## What may not live in the window

While the stream is open, `$4000-$7FFF` holds a Bank. A Section whose Payload
is read through it therefore cannot also live there: the routine would be
writing over the bytes it is reading.

<!-- include 05-banks-and-windows/in-window/level.asm tag=pinned -->
```asm
.section absolute at $4000
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

<!-- diagnostics 05-banks-and-windows/in-window -->
```
error[NGA5205]: `levelMap` is loaded from storage, and cannot live where the stream reads through, at $4000
 --> level.asm:8:1
  |
8 | .section absolute at $4000
  | ^^^^^^^^
```
<!-- end -->

The solver never puts a Section with a Payload in the Window, so only a pin
reaches this. It is the same shape as chapter four's refusal — the tool holds
the program to something the program did not have to state.

Which is the shape of a Bank in this chapter: somewhere to **keep** things.
The program reads storage through the Window and copies it out, and between
Transitions the Window holds base RAM and nothing of the program is in a Bank
at all. A Bank as somewhere for code and data to **stay**, run in, and be
switched to when they are wanted is the next chapter, and it is where the other
half of chapter four's rule comes in — two Sections may share an address not
only when their Phases never meet, but when what shows them never does.

Nothing here is written in C, because nothing here is written in the program.
The Modules did not change, and neither would a `.ngc`.
