# Reference: the Project file

A run takes one `.ngp` file and nothing else. The file says which Modules the
program has, what machine it runs on, and what the tool builds it into. No
`.asm` or `.ngc` file carries any of that.

This chapter lists every block and every option. The chapters before it teach
the ideas. This one is for looking things up.

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

```
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

```
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

A group may carry `base WINDOW = STATE` beside its list. Every Phase that needs
the group inherits it.

## `phase`

```
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

`base WINDOW = STATE` names the state a Window shows while the Phase runs and
nothing has switched. The Window must be one the target declares. The state must
be a named state of it. Give it at most once per Window per Phase.

A Phase that says nothing about a Window takes the base its groups agree on. If
they disagree, that is an error. If no group says anything, the variant's base
stands.

A Phase that no path from the entry reaches is a warning. An edge that no
`.transition` takes is also a warning.

## `resident`

```
resident { hud, music }
```

Names the Modules present in every Phase. This includes Phases declared later
and Phases in files included later.

A name may be a group. A name that is neither a Module nor a group is an error.

There is no way to name a subset of Phases other than listing them.

## `entry`

```
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
```
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

A Project adds its own `target` block for what the variant cannot know. A DOS in
low memory is one line:

```
target { region dos $0700 .. $1FFF reserved }
```

Blocks sum, so this narrows the variant's Regions rather than replacing them.

### `region`

```
region ram    $0000 .. $CFFF  ram
region stack  $0100 .. $01FF  reserved
region io     $D000 .. $D7FF  register
```

A range with one property. Both ends are inclusive. The name is optional.

| Property | Meaning |
|---|---|
| `ram` | the solver allocates here |
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

```
register COLBK  $D01A
register AUDF1  $D200, 2
```

A named `register` Region of one byte, or of the width after the comma. The
width is 1 or 2.

### `units`

```
units extension 4
```

Declares a unit set of N Banks. N is between 1 and 256, because a unit's number is one byte.

The name gives every Module a Symbol whose value is the count.

The block says how many Banks there are and nothing else. What the hardware
calls each one belongs to the driver.

### `window`

```
window ext $4000 .. $7FFF                  views main, extension  base main
window os  $C000 .. $CFFF, $D800 .. $FFFF  views rom, ram         base rom
```

Declares a Window: the ranges the hardware switches together, and the states it
can show.

Each name after `views` is a unit set of the target, or a named state of the
hardware. A unit set contributes one state per Bank. List each name once.

`base` names the state held when nothing has switched. A Window with no `base`
has ranges in no pool.

The tool numbers the states from zero in the order written, and a set's Banks in
their own order. The driver receives a state by that number.

A Window names no register. Showing a state is the driver's work, and a driver
shows every Window the target declares.

Region names, unit set names and Window names share one name space.

### `cpu`

```
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

```
containers xex, atr
```

What the machine takes. Write this in a variant. A Project that names a
Container the Target does not offer is an error.

Where no Target says anything, the Project's word stands.

## `storage`

```
storage { units extension }          ; the Banks of a unit set
storage { units 720  size 128 }      ; units by number, such as a disk
```

What a Transition loads from.

`units NAME` takes the Banks of the unit set the target declares under that
name. The size of one unit is the size of the Window that shows the set. Do not
write `size` beside it.

`units N` declares N units known by their numbers, and needs one `size`. The
size is between one byte and the address space.

Declare units once. A storage holds at most 256 units, because a unit's number is one byte.

Storage is one space of `units × size` bytes. Every Section a Transition loads
has a Payload packed into it. An image may run from one unit into the next. A
Payload with no room left is an error.

With no `storage` block there is no storage. A Section that needs a Payload is
then an error.

## `panes`

```
panes in ext      { level, tables[8] }
panes in os = ram { under }
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

```
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

```
container xex
```

What the tool builds the program into.

| Name | Result |
|---|---|
| `raw` | the raw image. This is the default. |
| `xex` | an Atari DOS binary |
| `atr` | a bootable double-density diskette |

Give it at most once. A Container constrains Place, so it belongs here. The `-o`
option names a path and chooses nothing.

## `optimize`

```
optimize speed
```

What to prefer where one choice costs speed against size. Write `speed`, `size`
or `fit`. A document that says nothing gets `fit`.

`fit` takes as much speed as the tool can promise to give back. It leaves any
choice it cannot undo.

An Intent fills a silence and overrides nothing. The tool wraps a function
written `inline` under every Intent.

## `constants`

```
constants {
  CART   = 1
  LEVELS = 8
}
```

A name the Project gives a value. Every Module sees it. The assembler reads one
in `.if`, a macro argument takes one, and C reads one through an `extern const`.

The value is a literal. It is not an expression. Do arithmetic in the source, where the tool reads
the value.

Giving one name a value twice is an error. A Module that declares a name the
Project declared is an error.

## `diagnostics`

```
diagnostics {
  deny  NGA2410
  allow NGA5120
  off   NGA3007
}
```

`deny` raises a finding to an error. `allow` lowers it to a warning. `off`
suppresses it.

An identifier that names no diagnostic is an error.

Setting one identifier twice is an error. The message names both places.

The block takes effect after the tool has read the Project. It cannot change how the tool
reports the findings it raised while reading the block. It never overrides the
command line.

## `include`

```
include "targets/atari-assets.ngp"
```

Names another Project file. Its content becomes part of this Project.

The tool resolves the path as it resolves a Module path. See
[the library](#the-library).

A cycle is an error. There are no include guards.

Inclusion takes a whole document. An included `.ngp` is a `.ngp` that could
stand alone. Blocks sum, so an included file that carries its own
`modules { ... }` composes with the file that included it.

There is no glob.

## The library

The tool ships some files as source instead of compiling them in. They are a
storage driver per mechanism, a decoder per format, a machine's OS, and a machine
variant that names them. They live in `lib/`.

Name a library file in `modules` or `include` as you name a file of your own.
The tool looks beside the document first and in the library second. A file of
your own under the same name therefore wins.

The tool finds the library in three ways, in this order:

1. The directory `--lib` names.
2. `lib/` beside the executable, which is how a release archive carries it.
3. The source tree the tool came from.
