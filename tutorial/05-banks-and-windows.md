# Banks and Windows

A diskette keeps storage out of the address space: a unit is a sector, and
nothing of it is anywhere until the driver reads it. A machine with banked
memory puts storage *in* the address space, a piece at a time, at an address
the rest of the program cannot use while it is there. The mechanism is the one
chapter four built. What changes is what it costs.

## The same program, another machine

Chapter four's program, unchanged. `print.asm`, `intro.asm` and `level.asm`
are the same files; so are the Phases, the `resident` line and the
`.transition`. Three lines of the Project differ:

<!-- include 05-banks-and-windows/banked/main.ngp -->
```ngp
include "atari/130xe.ngp"
include "atari/extension.ngp"

modules { "print.asm" "intro.asm" "level.asm" }

resident { print }

phase intro { needs intro  then level  entry introStart }
phase level { needs level             entry levelStart }
entry intro

container xex
```
<!-- end -->

The first `include` names an Atari 130XE rather than an 800XL. The second names
the storage, which is now the 130XE's extended memory rather than a diskette.
And the Container is a `.xex` rather than a diskette. The second is not a free choice:
a machine says what it takes, and this one takes no `.atr` — it has no drive in
its description, and its storage is not something a Container could write
sectors of.


## The machine

The variant is worth reading in full, since every `include` so far has stood
for one.

<!-- include ../lib/atari/130xe.ngp -->
```ngp
; An Atari 130XE: five Banks as a unit set — the four of extended memory and,
; last, the machine's own memory under the Window — the Window PORTB brings
; one of them into, every Region of the address space, and the Modules a
; program on this machine needs. It declares no storage. A program that loads
; anything during a run includes `atari/extension.ngp` beside this, which makes
; the Banks its storage. What PORTB takes to show a state is the
; driver's and not here, and the OS is two Modules rather than a Region because
; it occupies memory for a while rather than being a truth about addresses: its
; variables for the whole of a program, its ROM in every Phase that does not
; say `without osrom` — see 0229. The same register switches the ROMs, so the
; driver is what declares them.
target {
  cpu "6502"

  ; DOS loads a `.xex`, and a Project naming any other Container is refused
  ; here rather than at the writer.
  containers xex

  ; No Window has a base, so the memory under this one is a Bank like the
  ; others and nothing of the program stands there but a Pane or storage — see
  ; docs/decisions/0229-no-window-has-a-base.md.
  units  extension 5
  window ext  $4000 .. $7FFF  views extension
}

target {
  region ram    $0000 .. $CFFF  ram
  region        $D800 .. $FFFF  ram
  region stack  $0100 .. $01FF  reserved
  region io     $D000 .. $D7FF  register
  ; The RAM the OS ROM covers, named so that a Section may say `in underOsLow`
  ; and stand there: only in a Phase without `osrom`, or hidden under it where
  ; nothing needs it — see docs/decisions/0243-a-section-may-name-the-region-it-stands-in.md.
  region underOsLow   $C000 .. $CFFF  ram
  region underOsHigh  $D800 .. $FFFF  ram

  ; This machine takes a `.xex` and nothing else, and a `.xex` is loaded by a
  ; DOS, which stays where a DOS of the 2.x family stays. A Project whose DOS
  ; takes more says so with a `reserved` Region of its own, which narrows this
  ; one further.
  region dos    $0700 .. $1FFF  reserved

  ; What the driver writes. A Project that wants more of the hardware by name
  ; declares it, as it declares the memory its DOS holds.
  register PORTB  $D301

  ; The hardware that holds a Section while a program has handed it one, by
  ; `.root NAME` before the taking of its address, until it hands another or
  ; `.release NAME`: what it holds stays where it is for that long. See
  ; docs/decisions/0247-the-hardware-holds-a-section-for-as-long-as-it-is-given-it.md.
  holder dlist                          ; ANTIC's display list: SDLSTL, or DLISTL
  holder dli                            ; the display list interrupt: VDSLST
  holder vbi                            ; the immediate vertical blank: VVBLKI
  holder vbd                            ; the deferred vertical blank: VVBLKD
  holder nmi                            ; $FFFA, where the OS ROM is switched out
  holder irq                            ; $FFFE, where the OS ROM is switched out
  holder chbase                         ; the character set: CHBAS, or CHBASE
  holder pmbase                         ; the player-missile memory: PMBASE
  holder dosini                         ; DOSINI: called once a boot returns, and on every reset
}

; The OS and the driver, listed as a file of the Project's own would be. The
; driver is here and not beside the storage because it does more than read it:
; it shows a Pane's Bank and switches the ROMs, with or without storage. BASIC
; is not listed: a program that wants it lists `atari/basic.asm` and names it
; in a Phase's `needs`, and the driver switches it out everywhere else.
modules {
  "atari/osram.asm"  "atari/osrom.asm"  "atari/mathpack.asm"
  "atari/charsets.asm"  "atari/portb.asm"
}
resident { osram, osrom, mathpack, charsets, portb }
```
<!-- end -->

One of its Regions is not the machine's doing. A `.xex` is loaded by a DOS, and
a DOS of the 2.x family stays below `$2000` for as long as the program runs, so
`region dos $0700 .. $1FFF reserved` keeps the solver out of it — the
`reserved` of chapter three, used for the first time. The variant may say it
because it takes no other Container; a Project whose DOS takes more narrows it
further with a `reserved` Region of its own.

`units extension 5` declares a **unit set**: five units the hardware has, known
by number and by nothing else. The variant does not say what they are for.
`atari/extension.ngp`, the second `include`, does, in one line:
`storage { units extension }` says those units are the storage, so a unit is a
**Bank**. That is what a Bank is, and it is why the map calls it that here and
called it a unit on the diskette.

`window ext $4000 .. $7FFF views extension` declares a **Window**: a set of
address ranges the hardware switches as one, and the states it can show — here
the five Banks, one state each. The first four are the 130XE's extended memory.
The fifth is the machine's own memory at `$4000-$7FFF`, which PORTB shows when
it shows none of the other four. The Window names no register. What it takes to
show a state is the driver's, and `atari/portb.asm` is the driver that knows.

No state is held when nothing has switched. What the Window shows at a given
moment is whatever was shown last, so the tool puts nothing of the program at
`$4000-$7FFF` except storage and the Panes of the next chapter. The map's
`memory` line counts sixteen kilobytes fewer than the machine has for that
reason.

## What the tool did differently

<!-- map 05-banks-and-windows/banked storage -->
```
storage
  bank 0: 430 of 16384 bytes
    $0000-$0010  frame intro -> level          live intro
    $0011-$0184  level.levelMap               copy (372 bytes)  live intro
    $0185-$01AD  level.levelStart             copy (41 bytes)  live intro
  bank 1: 0 of 16384 bytes
  bank 2: 0 of 16384 bytes
  bank 3: 0 of 16384 bytes
  bank 4: 0 of 16384 bytes
```
<!-- end -->

Five Banks of sixteen kilobytes, where the diskette had units of sixty-four and
three of them. The Frame is seventeen bytes and was sixteen. The extra byte
says which ROMs the Phase entered needs: the same register that switches the
Banks switches the OS ROM and BASIC, so this driver is the one that switches
them, and the diskette's driver switches nothing.

<!-- map 05-banks-and-windows/banked phase=level rows=portb,nga.transforms -->
```
phase level (1)
  zero page: 143 of 256 bytes
  memory:    16562 of 40192 bytes
  ...
  $0080-$0082  portb.portbPtr               section            intro..level
  ...
  $0083-$0084  portb.portbDst               temporary          intro..level  shares with ngaDst ngaPtr levelStart.at
  ...
  $0085-$0086  portb.portbSize              temporary          intro..level  shares with ngaOffset ngaValue levelStart.left
  ...
  $0087-$0087  portb.portbRun               temporary          intro..level  shares with ngaUnit
  ...
  $2000-$2000  portb.portbFound             section            intro..level
  $2001-$2005  portb.portbValues            section            intro..level
  $2006-$2011  portb.portbBank              proc               intro..level
  $2012-$202F  portb.portbSelectRoms        proc               intro..level
  $2030-$2047  portb.portbOpenStream        proc               intro..level
  $2048-$205D  portb.portbReadByte          proc               intro..level
  $205E-$206D  portb.portbNextUnit          proc               intro..level
  $206E-$20DF  portb.portbCopy              proc               intro..level
  ...
  $2394-$2394  nga.transforms.ngaLoadUnit   section root       intro..level
  $2395-$239B  nga.transforms.ngaLoadMap    proc root          intro..level
  $239C-$23A3  nga.transforms.ngaTransform  proc               intro..level
  ...
```
<!-- end -->

Where the map held `disk.*` it now holds `portb.*`: the same roles, a table of
register values in place of a sector buffer, and no call into the OS at all.
Its scratch shares bytes with the routine's and with `levelStart`'s. Nothing
declared that; it is chapter two's rule applied to Modules written separately.

What is new are two Procs the tool generated for this Container and not for
the other. A `.xex` is loaded into memory by a DOS, and a Bank *is* memory, so
the file fills the Banks by switching each in through the driver and writing to
the Window. `ngaLoadUnit` and `ngaLoadMap` are what the loader calls to switch.
Nothing switches the Window back before the program starts, and nothing does
after a Transition either: the program never relies on what the Window shows
until it has said what to show.

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

The solver never puts a Section in the Window unless it is in a Pane, so only a
pin reaches this. It is the same shape as chapter four's refusal — the tool
holds the program to something the program did not have to state. A pin there
of a Section with no Payload is refused as well (`NGA5235`), since the Window
may be showing any Bank when the code that reads it runs.

Which is the shape of a Bank in this chapter: somewhere to **keep** things.
The program reads storage through the Window and copies it out, and between
Transitions nothing of the program is in a Bank at all. A Bank as somewhere for code and data to **stay**, run in, and be
switched to when they are wanted is the next chapter, and it is where the other
half of chapter four's rule comes in — two Sections may share an address not
only when their Phases never meet, but when what shows them never does.

Nothing here is written in C, because nothing here is written in the program.
The Modules did not change, and neither would a `.ngc`.
