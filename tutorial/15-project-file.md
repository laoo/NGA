# Reference: the Project file

A run takes one `.ngp` file and nothing else. The file says which Modules the
program has, what machine it runs on, and what the tool builds it into. No
`.asm` or `.ngc` file carries any of that.

This chapter lists every block and every option. The chapters before it teach
the ideas. This one is for looking things up.

## The command line

```sh
nga main.ngp -o game.xex --map game.map
```

`-o` names the file to write, and `--map` a file for the memory map.
`--deny`, `--allow` and `--off` change a finding's severity, as the
[`diagnostics`](#diagnostics) block does. `nga --help` lists the rest.

`--explain` is for a build that ends in `NGA5206`, no layout satisfies every
constraint. The tool asks the solver once more and names the fewest pairs of
Sections that cannot be kept apart, one `NGA5207` for each pair. Making room for
one Section of each pair is the fix.

## How the tool reads the file

Line endings mean nothing. Every construct ends because its own shape is
complete. Layout is free.

A block may appear more than once. The occurrences sum. Order does not matter,
and a block may name something declared later or in a file included later.

Nothing is a keyword. The tool reads a block's name and every word inside it by
position.

Lists are comma-separated: `needs a, b` and `resident { a, b }`. The `modules`
block has no commas, because each entry has a structure of its own.

A value is an expression in the assembler's expression language. Write `$4000`
here as you write it in source. The Project has no Symbols, so a name in an
expression is an error.

Comments start with `;` and run to the end of the line.

Two entries that contradict each other are an error. The last one does not win.
This keeps the result independent of the order of your `include` lines.

## `modules`

```ngp
modules {
  "src/intro.asm"
  "gfx/tiles.asm"                        as gfxTiles
  binary( "gfx/font.bin", align = 1024 )
}
```

Each entry is a file path or a generator call. The Module takes its name from
the file stem. Use `as NAME` to give it another name.

Two Modules with the same name are an error. The message proposes `as`.

A path is relative to the file that holds it. Use `/` on every platform. A path
that names no file there names one in [the library](#the-library).

A path is a plain string. It may hold non-ASCII characters. It may not carry a
character set prefix, because nothing translates it.

A `.ngc` path is a Module in the C subset. The tool compiles it before it
assembles it.

A Project with no Modules is an error.

A generator call writes a Module instead of reading one. See
[Generators](08-generators.md). One argument stands without a name and is the
file the generator works from. A name with a value is an option, such as
`align = 1024`. A name on its own is a flag, such as `root`.

A name before the brace makes the block a [group](#group).

## `group`

```ngp
modules engine { "engine/ball.asm"  "engine/draw.asm" }
modules sfx    { "engine/beep.asm" }
group   play   { engine, sfx }

phase level1 { needs play, one  then level2 }
```

A group is a name for a list of Modules. It stands wherever a list of Modules
stands, which is `needs` and `resident`. It means every Module it holds, each
one once.

Two blocks declare a group. A named `modules` block is a group of the Modules
it declares. A `group` block lists names, never paths.

A group may name another group to any depth. A group that reaches itself is an
error.

One name for both a Module and a group is an error. A group may not stand in a
`transform` entry, which names one Module.

A group that some list named and that holds no Module is a warning.

## `phase`

```ngp
phase intro { needs intro, hud   then game }
phase game  { needs game, hud    then boss, intro  entry gameStart }
```

`needs` lists the Modules the Phase requires in memory. A name may be a group.

`then` lists the Phases a Transition may lead to.

Both may repeat inside a block and sum. Two `phase` blocks with one name sum.

A Module's Residency is the set of Phases whose `needs` name it. Its Sections
inherit that Residency. Nothing else declares Residency.

A Module that no Phase names is never in memory. That is a warning.

A name in `needs` that is no Module is an error. A name in `then` that is no
Phase is an error.

`entry NAME` names the Label the Phase starts at. Write `one.start` for a Label
in a Namespace. Without `entry`, the tool looks for a Label named `entry`. The
name must have exactly one definition among the Modules the Phase needs. It may
be private. It must be a Label.

`without NAME` takes a Module out of the Phase that `resident` put in. It works
only for a ROM the driver switches out, such as `osrom` on the 130XE:

```ngp
phase deep { needs deep  without osrom, mathpack }
```

While `deep` runs, the driver has the OS ROM switched out, and the RAM under it
belongs to the Phase. Code in `deep` cannot call the OS, because the OS is not in
the Phase. The program also has to look after the interrupt vectors at `$FFFA`,
which are in that RAM now.

The driver says which Modules it can switch out. Any other name after `without`
is an error, and so is the entry Phase of a `.xex` doing without anything, since
its DOS loads it with every ROM in.

`final` marks a Phase the program never leaves: it has no `then`, and its entry
never returns.

```ngp
phase game { needs game  without osrom, mathpack  entry gameStart  final }
```

The tool then keeps out of `game` what runs only when a program leaves a Phase
or returns: the code that ends the program, and a driver that only switches
ROMs. A game that loops in its last Phase gets that memory back. A `final`
Phase with `then` is an error, and so is one whose entry may return.

A Phase that no path from the entry reaches is a warning. An edge that no
`.transition` takes is also a warning.

## `resident`

```ngp
resident { hud, music }
```

Names the Modules present in every Phase. This includes Phases declared later
and Phases in files included later.

A name may be a group. A name that is neither a Module nor a group is an error.

There is no way to name a subset of Phases other than listing them. A Phase can
leave out of that only a ROM the driver switches, with `without`.

## `load`

```ngp
load { needs osram, dos, fade  init fadeStart }
```

Describes what happens while the DOS loads a `.xex`, before the program starts.

A Module named in `needs` that holds only pinned Sections with no bytes says
where the machine's memory is during the load: the OS's variables, the DOS's
code and buffers. Nothing the file writes while it loads goes there: no Section
with bytes, and none of the code the tool runs during the load. A buffer with
no bytes may go there, because the DOS is done with the memory by the time the
program writes the buffer. A Section you pin there with bytes, such as a write
to `INITAD`, is allowed with a warning.

Any other Module named in `needs` is code of your own that runs during the
load. `init` names a Label in it, and the `.xex` calls that Label through
`INITAD` before it writes anything else. Several `init`s are called in order.
The tool places this code in memory that is free during the load and writes the
rest of the file after it, so the rest may go over it once it has run. Such a
Module belongs to no Phase.

The library's `atari/800xl.ngp` already lists the OS's memory as `resident`,
which covers the load too. A Project that drops it, to give a Phase without the
OS that memory, names it in `load` instead.

## `entry`

```ngp
entry intro
```

Names the Phase the program starts in. Required when the document declares a
`phase`. Give it at most once.

## A Project with no `phase` block

The Project has one Phase. It is unnamed and holds every Module. It needs no
`entry`.

## `target`

The machine. Write this in a machine variant and include the variant, because
the block states hardware truth. The tool ships variants in
[the library](#the-library):

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

A Project adds its own `target` block for what the variant cannot know. A DOS
that takes more low memory than the variant leaves it is one line:

```ngp
target { region $0700 .. $2FFF reserved }
```

Blocks sum, so this narrows the variant's Regions rather than replacing them.
The Region has no name, because the variant already names `dos` and one name
names one Region.

### `region`

```ngp
region ram    $0000 .. $CFFF  ram
region stack  $0100 .. $01FF  reserved
region io     $D000 .. $D7FF  register
```

A range with one property. Both ends are inclusive. The name is optional.

| Property | Meaning |
|---|---|
| `ram` | the solver allocates here |
| `rom` | the solver allocates here only what no code writes, such as a cartridge's fixed part. See [Cartridges](14-cartridges.md). |
| `reserved` | the solver does not allocate here. A pin here is a warning. |
| `register` | nothing may stand here. A pin here is an error. |

Where two Regions overlap, the more restrictive property wins. A Project can
narrow the variant's memory. A Project cannot turn a register into RAM.

A named Region gives every Module a Symbol of kind `Region`. Its value is the
start address. A Module that defines the same name is an error.

A Region must run from a lower address to a higher one. One name names one
Region.

With no `region` and no `register` anywhere, the Target keeps two stand-in `ram`
Regions, `$0080-$008F` and `$2000-$9FFF`. The first Region you declare replaces
the stand-in.

### `register`

```ngp
register COLBK  $D01A
register AUDF1  $D200, 2
```

A named `register` Region of one byte, or of the width after the comma. The
width is 1 or 2.

### `units`

```ngp
units extension 5
```

Declares a unit set of N Banks. N is between 1 and 65536, because a unit's number
is two bytes at most.

The name gives every Module a Symbol whose value is the count.

The block says how many Banks there are and nothing else. What the hardware
calls each one belongs to the driver.

```ngp
units sdram 2047  linear $4000 .. $1FFFFFF
```

`linear BEGIN .. END` says where the Banks lie in the memory another master,
such as a blitter, reads as one range. Bank 0 starts at `BEGIN`, each next Bank
starts a Window's size further on, and the last must end by `END`. Both ends are
inclusive. This is MariaCEL's, whose blocks are 16 KB.

A Section in a Pane of such a set then has a linear address, read with
[`linearSectionAddress`](16-assembler.md#attribute-access), and a Section may
stand [`in`](16-assembler.md#in-name) the set itself, across its Banks.

### `window`

```ngp
window ext     $4000 .. $7FFF  views extension
window cel4000 $4000 .. $7FFF  views main, sprlist, textab, derived, stripmap, strip
```

The first is the 130XE's, and the second MariaCEL's, whose states are the
blitter's blocks.

Declares a Window: the ranges the hardware switches together, and the states it
can show.

Each name after `views` is a unit set of the target, or a named state of the
hardware. A unit set contributes one state per Bank. List each name once.

No state is held when nothing has switched: what a Window shows outside a
`.with` is undefined. So a Window's ranges are in no pool, and only a Pane's
Sections and storage stand there. `base`, which once named such a state, is an
error (`NGA1242`).

The tool numbers the states from zero in the order written, and a set's Banks in
their own order. The driver receives a state by that number.

A Window names no register. Showing a state is the driver's work, and a driver
shows every Window the target declares.

Region names, unit set names and Window names share one name space.

### `cpu`

```ngp
cpu "65sc02"
```

The processor the machine has. Write `"6502"` or `"65sc02"`. A Target that says
nothing is a 6502. You quote the name because it starts with a digit.

`"65sc02"` adds the `(zp)` addressing modes, `stz`, `trb`, `tsb`, `inc` and `dec` of `A`,
`phx`, `phy`, `plx`, `ply`, `bit` immediate and indexed, `bra`, and
`jmp (abs,x)`.

It does not add the Rockwell bit instructions, `wai` or `stp`. The 65SC02 does
not have them either.

An instruction the Target's processor lacks is an error. The message names the
instruction, the processor it needs, and the processor the Target is. See
[Instructions](12-instructions.md).

### `containers`

```ngp
containers xex, atr
```

What the machine takes. Write this in a variant. A Project that names a
Container the Target does not offer is an error.

Where no Target says anything, the Project's word stands.

## `storage`

```ngp
storage { units extension }          ; the Banks of a unit set
storage { units 720  size 128 }      ; units by number, such as a disk
```

What a Transition loads from.

`units NAME` takes the Banks of the unit set the target declares under that
name. The size of one unit is the size of the Window that shows the set. Do not
write `size` beside it.

`units N` declares N units known by their numbers, and needs one `size`. The
size is between one byte and the address space.

Declare units once. A storage holds at most 65536 units, because a unit's number
is at most two bytes.

Storage is one space of `units × size` bytes. Every Section a Transition loads
has a Payload packed into it. An image may run from one unit into the next. A
Payload with no room left is an error.

With no `storage` block there is no storage. Then nothing waits anywhere: a
Section with bytes is written when the program loads and stays in memory for as
long as a Phase still to come needs it, and a Transition loads nothing.

So a Phase may read and write the bytes of a Phase it leads to, such as a boot
Phase patching the game's code before it enters the game. It may not call that
code: the code would run in a Phase where what it uses is not there. A routine
the boot Phase calls belongs in a Module the boot Phase needs.

A machine variant declares no storage. A library file beside it does, such as
`atari/disk.ngp` for an 800XL's diskette or `atari/extension.ngp` for a
130XE's Banks, and a Project includes it when something waits there.

## `panes`

```ngp
panes in ext               { level, tables[8] }
panes in cel4000 = sprlist { sprlist }
```

A Pane is a named set of Sections that one switch shows together. See
[Panes](06-panes.md). The block names the Window its Panes are in.

The solver gives each Pane one state of the Window for the whole run. It picks a
Bank of the unit set the Window shows.

Write `= STATE` in the header to pin the Panes to a named state. The solver then
has no say. A Window that shows no unit set holds only pinned Panes.

`NAME[N]` declares a family: N Panes of one layout on consecutive Banks. N is
between 1 and 255. Every Section in the family stands in each member at one
address. A pinned Pane is one state, so it cannot be a family.

A Pane whose Sections come to more than a Bank holds is an error. So is a family
wider than its set.

A Pane's name gives every Module a Symbol of kind `Pane`. Its value is the index
of the state the solver gave it.

## `transform`

```ngp
transform rle { music.notes, levels.tiles }
```

Names the Sections whose Payload the tool decodes on the way in. The tool
copies a Section that appears in no `transform` block.

The unit is the Section, not the Module. A Section has no name of its own, so
name it by a Label of it: `module.label`, or `module.space.label` for a Label in
a Namespace. Any Label of the Section names it.

The tool finds the Label whether you exported it or not.

The transform must be a format the tool encodes, and some Module must declare a
decoder for it with `.transform`. That Module must have a Label of that name.

A group may not stand here. A Section that no Transition loads has nothing to
transform, which is a warning.

## `container`

```ngp
container xex
```

What the tool builds the program into.

| Name | Result |
|---|---|
| `raw` | the raw image. This is the default. |
| `xex` | an Atari DOS binary |
| `atr` | a bootable double-density Atari diskette |
| `car` | an Atari cartridge image. See [Cartridges](14-cartridges.md). |
| `bs93` | a Lynx program an emulator copies into memory and enters |
| `lnx` | a Lynx cartridge, with the header a reader shows |
| `lyx` | a Lynx cartridge without the header |
| `prg` | a Commodore PET program, started by a line of BASIC |
| `d64` | a `prg` on a 1541 or 4040 diskette |
| `d80` | a `prg` on an 8050 diskette |
| `d82` | a `prg` on an 8250 diskette |

Give it at most once. A Container constrains Place, so it belongs here. The `-o`
option names a path and chooses nothing.

A cartridge names its board after the Container, quoted:

```ngp
container car "xegs128"
container lnx "64k"
```

`car` takes `8k`, `16k`, or one of `xegs32`, `xegs64` and so on up to
`xegs1024`. `lnx` and `lyx` take `64k`, `128k`, `256k` or `512k`, the board's
size, which sets its page size. A board is required there, and every other
Container takes none. The tool checks the board against the machine's Regions
and Windows.

A `.xex` can be compressed:

```ngp
container xex compress zx0
```

Each segment of the file that gets smaller is stored in that format. While the
file loads, the tool's decoder unpacks it at the addresses it belongs at. A
segment that does not get smaller is stored as it is. Once the file has loaded,
memory holds what it would without compression, so the program needs no
changes. The format may be `zx0` or `rle`. Only a `.xex` takes `compress`.

To keep some Modules uncompressed, list them, or their groups, after `except`:

```ngp
container xex compress zx0 except { loadingPic }
```

They are stored as they are and arrive when they would without compression. A
loading screen is the usual case, since it has to appear at once.

The file is smaller, but the loader has to wait for the decoder. On a slow
drive the load is faster overall, and on a fast device it can be slower.

## `cartridge`

```ngp
cartridge {
  name         "A Lynx of Two Masters"
  manufacturer "NGA"
  rotation     none
}
```

What a reader shows about a `.lnx`: the three fields of its header. `name` holds
at most 31 bytes and `manufacturer` at most 15. A longer one is an error and is
not cut. `rotation` is how the machine is held: `none`, `left` or `right`.

Give each field at most once. Without the block both strings are empty and the
rotation is `none`. The block with any other Container is an error, `lyx`
included, because that image has no header.

## `frames`

```ngp
frames held
```

Makes the Transition routine read an edge's list of what to load in one pass,
before the first image arrives. Without it, the routine goes back to the Frame
between one image and the next, and the driver opens it again each time.

It costs some 130 bytes of the routine and two bytes of zero page, in every
Phase. It pays where opening is slow, such as a Lynx's cartridge or a diskette,
and saves nothing where opening is a Window being switched. On a Lynx, an edge
of forty images took 54 frames without it and 15 with it.

The list is kept in the memory of the edge's largest image, which then loads
last, so it costs no memory of its own. Write it at most once. In a Project
with an edge and no storage there is no Frame, and the line is an error.

## `optimize`

```ngp
optimize speed
```

What to prefer where one choice costs speed against size. Write `speed`, `size`
or `fit`. A document that says nothing gets `fit`.

`fit` takes as much speed as the tool can promise to give back. It leaves any
choice it cannot undo.

An Intent fills a silence and overrides nothing. The tool wraps a function
written `inline` under every Intent.

## `constants`

```ngp
constants {
  CART   = 1
  LEVELS = 8
}
```

A name the Project gives a value. Every Module sees it. The assembler reads one
in `.if`, a macro argument takes one, and C reads one through an `extern const`.

The value is a literal. It is not an expression. Do arithmetic in the source, where the tool reads
the value. A `-` written right after the `=` is the literal's sign, so `SHIFT = -1` and
`DEPTH = -$10` are values. C reads a negative one through an `extern const i8`.

Giving one name a value twice is an error. A Module that declares a name the
Project declared is an error.

## `choose`

```ngp
choose { "vbxe-d640.ngp" "vbxe-d740.ngp" }
include "game.ngp"
```

Builds one `.xex` that can be any of the documents it names, and the program
picks one while the file loads. Use it when one piece of hardware can sit at
more than one address: a VBXE at `$D640` or at `$D740`, for example.

Each document holds values and nothing else: `register` lines in a `target`
block, and a `constants` block. Every document declares the same names. The
tool builds the program as the first document says and places it once. It then
builds it again with the values of each other document and notes every byte
that comes out different.

An `init` of the [`load`](#load) block finds out which document the machine
needs and writes its number into `ngaChoice`: 0 for the first, 1 for the
second. While the rest of the file loads, the tool's own code changes every
noted byte to the value of that document, before anything runs it. Code that
the first `init` runs is not changed, so it reads the places it checks by their
numbers. Code of a later `init` is changed before it runs.

A document may change bytes only. A `.if` that takes branches of different
lengths in two documents is an error. So is a register of a later document
standing where the program could place a section. Only a `.xex` without
storage can be changed this way.

## `diagnostics`

```ngp
diagnostics {
  deny  NGA4501
  allow NGA4606
  off   NGA0113
}
```

`deny` raises a finding to an error. `allow` lowers it to a warning. `off`
suppresses it.

An identifier that names no diagnostic is an error.

Setting one identifier twice is an error. The message names both places.

The block takes effect after the tool has read the Project, with every file it
includes and every Module a generator wrote. It cannot change how the tool
reports a finding raised while reading them, and a generator's warning is one of
those. Where the block stands in the file makes no difference. It never
overrides the command line: `--deny`, `--allow` and `--off` reach every finding.

## `include`

```ngp
include "targets/atari-assets.ngp"
```

Names another Project file. Its content becomes part of this Project.

The tool resolves the path as it resolves a Module path. See
[the library](#the-library).

The tool reads each file once. A second `include` of a file it has already read
adds nothing, so two files can both include a third that they share. The tool
knows a file by the path it resolves to, not by how the `include` spelled it.
This is safe because a file means the same wherever it is included: blocks sum
and their order does not matter, and the paths inside a file are resolved
against that file. A cycle is still an error.

Inclusion takes a whole document. An included `.ngp` is a `.ngp` that could
stand alone. Blocks sum, so an included file that carries its own
`modules { ... }` composes with the file that included it.

There is no glob.

## The library

The tool ships some files as source instead of compiling them in. They are a
storage driver per mechanism, a decoder per format, a machine's OS, a machine
variant that names them, and the storage that goes beside a variant. They live in `lib/`.

Name a library file in `modules` or `include` as you name a file of your own.
The tool looks beside the document first and in the library second. A file of
your own under the same name therefore wins.

The tool finds the library in three ways, in this order:

1. The directory `--lib` names.
2. `lib/` beside the executable, which is how a release archive carries it.
3. The source tree the tool came from.
