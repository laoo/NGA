# Cartridges

Every Container so far has been something a machine **loads**: a DOS reads a
`.xex` into memory, a boot record reads sectors off a diskette. A cartridge is
not loaded at all. Its ROM is decoded onto the bus by the board, the CPU reads
it where it lies, and the only thing software can do is choose which part of a
large ROM stands in a window.

That changes what a Container is for. There is no loader to write and no order
to load things in; there is an image whose offsets are the hardware's, and the
board does the rest.

## The same program, another machine

Chapter six's program, unchanged. `print.asm`, `shared.asm`, `intro.asm` and
`level.asm` are the same files, the Phases are the same, the Pane is the same
and so is the Trampoline that draws the map. Three lines of the Project differ:

<!-- include 14-cartridges/cartridge/main.ngp -->
```ngp
include "atari/xegs128.ngp"

modules { "print.asm" "shared.asm" "intro.asm" "level.asm" }

panes in switched { level }

resident { print, shared }

phase intro { needs intro  then level  entry introStart }
phase level { needs level             entry levelStart }
entry intro

container car "xegs128"
```
<!-- end -->

`include` names a machine holding a 128 KB cartridge; the Pane's Window is
called `switched` on this board rather than `ext`; and the Container is a
`.car`, which **names the board it is an image of**:

```ngp
container car "xegs128"
```

The board is quoted for the reason a processor is — `8k` begins with a digit,
and no identifier of this language may — and it is not decoration. A board
decides how large the image is, where in it each bank stands, and what number a
reader has to find in the header to know which of the hundred-odd cartridge
mappers this one is. Eight boards are written: `"8k"` and `"16k"`, which have no
banks at all, and the six of the XEGS family from `"xegs32"` to `"xegs1024"`.

## The machine

<!-- include ../lib/atari/xegs128.ngp -->
```ngp
; An Atari 800XL holding a 128 KB XEGS cartridge: sixteen banks of eight
; kilobytes, of which the last is the fixed part the CPU always sees at
; $A000-$BFFF and the other fifteen are storage, brought into $8000-$9FFF one at
; a time by the byte written to $D5FF. A cartridge is ROM the CPU reads where it
; lies, so nothing is loaded and there is no DOS: the machine's whole RAM below
; the cartridge is the program's, $0700 included.
target {
  cpu "6502"

  ; The one Container a machine holding a cartridge takes. A `.xex` needs a DOS
  ; to load it, and this machine has none.
  containers car

  ; Fifteen and not sixteen: the sixteenth bank is the fixed part under another
  ; name, and one set of bytes with two names is one too many.
  units  banks 15
  window switched $8000 .. $9FFF  views banks
}
storage { units banks }

target {
  region ram    $0000 .. $7FFF  ram
  region stack  $0100 .. $01FF  reserved
  region io     $D000 .. $D7FF  register

  ; The part of the cartridge no switch takes away, which is where the solver
  ; puts everything nothing writes. $8000-$9FFF is in no Region at all: it is
  ; the window, it shows one Bank at a time, and only a Pane or storage stands
  ; there — as the boot record's page is in no Region of the diskette's variant.
  region cart   $A000 .. $BFFF  rom

  ; What the driver writes. Any address of $D500-$D5FF is the same register.
  register CARTSEL $D5FF
}

; The OS, the driver for this storage and the decoders the tool ships. Every
; Transition calls the driver and the decoders, so they are resident; a decoder
; nothing uses is dropped.
modules { "atari/os.asm"  "atari/charsets.asm"  "atari/cart.asm"  "stream/zx0.asm" }
resident { os, charsets, cart, zx0 }
```
<!-- end -->

Three things in it are worth stopping on.

**The fixed part is a `rom` Region.** That is a fourth thing a Region can be,
beside `ram`, `reserved` and `register`: memory the solver places in and no code
writes. Everything the program never writes goes there — every Proc, every table
of constants — and the solver puts it there without being told, because it can
see which Sections nothing writes. Where it cannot see, the source says
`readonly`; that is the whole of chapter thirteen's problem seen from the other
side, and the next section shows it.

**`$8000-$9FFF` is in no Region at all.** It is the window, it shows one bank at
a time, and the only things that stand there are a Pane's Sections and storage.
A range in no Region is in no pool, so the solver never allocates there — the
same trick a diskette's variant uses to keep the boot record's page to itself.

**The window has no `base`.** On a 130XE the Window has one: base RAM is under
it, and it is memory the program uses, so a `.with` shows a Bank and puts the
RAM back afterwards. Under a cartridge's window there is no memory, only the
next bank. So nothing is put back:

> A `.with` on a Window with no base switches the bank and **leaves it
> switched**. What the window shows outside a `.with` is undefined, and the tool
> refuses every way of reading it.

Which is how one thinks about banks anyway: not *what is in the window by
default* but *the thing I want is in bank n*. It costs half of what a `.with`
costs on the 130XE — two instructions and no exit — and it is what makes a Pane
on a cartridge reachable at all.

## What it costs, and what it gives back

The map, in one Phase:

<!-- map 14-cartridges/cartridge phase=level rows=level,print -->
```
phase level (1)
  zero page: 153 of 256 bytes
  memory:    16051 of 32256 bytes
  ...
  $0088-$0089  level.drawMap.at             temporary          level  shares with ngaPtr
  ...
  $008A-$008A  level.drawMap.left           temporary          level
  ...
  $8000-$8171  level.levelMap               section            level  in pane level (state 14)
  ...
  $A0B3-$A0CB  print.printLine              proc               intro..level
  $A0DE-$A102  level.drawMap                proc               level
  $A103-$A10D  level.levelStart             proc               level
  ...
```
<!-- end -->

**`$8000-$BFFF` is gone for good.** A plain XEGS cartridge cannot be switched
off, so 16 KB of the address space is the board's for the whole run — of which
8 KB is the window and 8 KB the fixed part. That is the rent.

**The RAM a DOS would have held is the program's.** There is no DOS: the machine
boots the cartridge, so `$0700` upwards is free, and the variant's `region ram`
runs from zero. Chapter four's program had to leave `$0700-$1FFF` to a DOS of
the 2.x family.

**The ROM is as large as the board.** 128 KB of it here: eight for the fixed
part and fifteen banks of eight for storage and for Panes. A `.xex` this program
would fit in twice over.

## The cold start

One thing a cartridge cannot do is put a byte in RAM. `intro.asm` holds a
string, `introText`, that the program prints — and a string is bytes the program
was given, standing at an address in RAM. On a `.xex` the DOS puts it there. Here
nobody does.

So the tool does it the way it does everything else about a Phase's memory: the
**cold start** is an edge into the entry Phase from nowhere. The string waits in
a bank as any Payload does, the run vector points at a stub, and the stub hands
the Frame of that edge to the Transition routine, which copies the blocks,
writes the Cell, and jumps to the entry Label — exactly what it does when a
`.transition` is taken. Storage says so:

<!-- map 14-cartridges/cartridge storage -->
```
storage
  bank 0: 30 of 8192 bytes
    $0000-$000F  frame into intro              live intro..level
    $0010-$0013  frame intro -> level          live intro
    $0014-$001A  intro.introText              copy (7 bytes)  live intro
    $001B-$001D  nga.cell.ngaCurrentPhase     copy (3 bytes)  live intro
  bank 1: 0 of 8192 bytes
  bank 2: 0 of 8192 bytes
  bank 3: 0 of 8192 bytes
  bank 4: 0 of 8192 bytes
  bank 5: 0 of 8192 bytes
  bank 6: 0 of 8192 bytes
  bank 7: 0 of 8192 bytes
  bank 8: 0 of 8192 bytes
  bank 9: 0 of 8192 bytes
  bank 10: 0 of 8192 bytes
  bank 11: 0 of 8192 bytes
  bank 12: 0 of 8192 bytes
  bank 13: 0 of 8192 bytes
  bank 14: 0 of 8192 bytes
```
<!-- end -->

`frame into intro` is the cold start's; `frame intro -> level` is the edge the
program takes itself. Both are read by the same routine, and the second Phase's
map is not there at all — a Pane's Sections are written into their Bank by the
Container and no edge copies them, which is chapter six's rule and does not
change here.

The price is the routine: some four hundred bytes of the fixed part, in every
program on a banked board, whether or not it takes a `.transition` of its own.
The alternative was a copier of its own for the cold start, and it was refused
because a Payload belongs to a Section and a Frame to an edge: a program with an
edge back into its entry Phase would have carried the same bytes twice, and
nothing would have related the two.

## How it starts

Six bytes at the top of the image are the tool's, and they are what the OS reads
before it runs anything of a cartridge:

| Address | Holds |
|---|---|
| `$BFFA` | the program's start — the cold start's stub, or the entry Label where there is none |
| `$BFFC` | zero, which is what says a cartridge is there |
| `$BFFD` | `$04`: started through `$BFFA`, no disk boot, not the diagnostic cartridge |
| `$BFFE` | the init vector, pointing at an `rts` |

The init vector does nothing on purpose. The OS calls it in the middle of its
own cold start, before `E:` exists, so anything run there would run in a machine
half set up. The jump through `$BFFA` comes at the very end, **after `E:` is
open on channel 0** — which is why `print.asm` works on a cartridge exactly as
it works on a diskette.

All of it runs again on `RESET`, so a cartridge program restarts rather than
resuming.

## Running it

A cartridge is the one Container besides the `.xex` that the suite can put in a
machine and run: an image is memory, and the register that switches its banks is
one store. But the boot protocol above is the OS's, not the suite's, so this
chapter's program was booted on an emulated Atari with the machine's own ROM,
the way the tutorial's programs are:

```
$ scripts/run-on-atari.py tut.car --frames 200
tut.car on 130XE, Atari XL/XE OS ver.2, 200 frames after boot
screen memory at $7C40
  |   INTRO
  |   #####################################
  |   #...................................#
  |   #...................................#
  |   #......########.....................#
  |   #......#......#.....................#
  |   #......#......#.....................#
  |   #......########.....................#
  |   #...................................#
  |   #...................................#
  |   #####################################
```

`INTRO` came out of a string the cold start copied from bank 0; the map was
drawn out of bank 14 without being copied anywhere at all.

## What is not written

Of the cartridge mappers that exist, this tool writes eight. The reason is one
line of hardware: a program of this tool has code that must be reachable *while*
a bank is switched — the Transition routine, the driver, the decoders — so it
needs a part of the ROM that no switch takes away. MegaCart, AtariMax, Williams
and SIC! switch the whole of the cartridge's address space, so on those the
resident code would have to be replicated in every bank or copied into RAM
before any switch happened. Switchable XEGS, whose bit 7 reveals the RAM beneath
the whole cartridge, is the one that would come first: it has a state to name,
so its window would have a base, and everything in this chapter about leaving a
bank switched would have to be read again.
