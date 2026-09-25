# Reference: the assembler

What an `.asm` Module may hold. The chapters before this one teach the ideas.
This one lists the forms.

The 6502 instruction set is not here. Mnemonics and addressing modes are the
same as in every other assembler. What this chapter lists is what the tool adds.

## Lines and labels

A line holds a label definition, a statement, or both:

```asm
copyRow ldx #0
@loop   lda source,x
```

A label definition is an identifier at column one. Indent a statement. Only the first token of a line can stand at column one, so a line
never defines two labels.

An identifier at column one with `=` after it defines a Constant. With anything
else after it, it defines a Label.

```asm
screenWidth = 40                ; Constant
copyRow                         ; Label
```

A Label whose name is a mnemonic is a warning. `rts` at column one is a legal
label definition, and the warning is what tells you that you lost an indent.

### Local labels

A name with a leading `@` is local to the enclosing `.proc` or macro body.
Outside both it is an error.

Use `@` for a label that repeats. A plain name is unique in its scope. In a
Proc it becomes a Symbol under the Proc's name, such as `worker.inner`.

| Reference | Resolves to |
|---|---|
| `@name` | the one definition of `name` in the scope. Several is an error. |
| `@+name` | the nearest definition after the reference |
| `@-name` | the nearest definition before the reference |
| `@` | defines an anonymous local label |
| `@+` | the nearest anonymous label after the reference |
| `@-` | the nearest anonymous label before the reference |

Direction compares position in the source, never address.

Named and anonymous labels are separate. `@-` passes over every `@name`, and
`@-name` passes over every `@`. The direction does not repeat, so `@++name` does
not exist. The direction marker appears only in references. `@` followed by
digits is an error.

## Numbers, strings and characters

| Form | Base |
|---|---|
| `123` | decimal |
| `$1F` | hexadecimal |
| `%1010` | binary |
| `'a'` | the character's value in a character set |

There is one notation per base. `0x`, a trailing `h` and a leading `0` for octal
are errors.

`_` may separate digits and must stand between them. `$FF_FF` is a literal.
`$_FF` and `$FF_` are errors.

A digit sequence must not touch an identifier character. There are no
floating-point literals, which leaves `..` free to mean a range.

Escapes are `\\`, `\"`, `\'`, `\n`, `\t` and `\0`. Any other escape is an error.
An escape is one character. There is no `\xNN`, because a raw byte belongs in
the data directive around the string.

A literal may carry a character set prefix, such as `atascii"HELLO"`. See
[Character sets](11-character-sets.md) and [`.charset`](#charset) below. A
literal with no prefix must be ASCII.

## `.section`

```asm
.section [ attribute [ , attribute ]... ]
.endsection
.ends

attribute = placement | movable | root | in NAME | temporary
placement = ( zeropage | absolute ) qualifier*
qualifier = at EXPRESSION | align EXPRESSION | within EXPRESSION
```

At most one placement, one `movable`, one `root`, one `in` and one `temporary`,
in any order.

A Section has no name of its own. Every Label it holds stands for it on the left
of a dot. Its first Label is what the memory map and a finding call it. A
Section that holds no Label is an error.

PlacementClass defaults to `absolute`.

### Placement

```asm
.section absolute at $2000
code
.section zeropage at $80
vars
.section absolute align 1024
font
.section absolute within 1024
list
```

`at N` pins the Section. `align N` starts it at a multiple of `N`. `within N`
keeps it from crossing a multiple of `N`. Use `within` for a table indexed by
`,x`, for a display list, and for screen memory.

`at`, `align` and `within` complete the placement, so they never follow a comma.
The class always stands next to the address. Each takes an `Integer` that is a
[declared value](#declared-values). Neither `align` nor `within` may be zero.

A Section larger than its `within` boundary is an error.

### `movable`

A Movable Section may stand at a different address in each Phase it is present
in. The solver moves it only where a layout needs it to.

A Reference holds it still. Code that names it sees one address in every Phase
that code is in. A Section that names itself never moves. A Section named from
resident code never moves.

`at` applies in every Phase, so `at` and `movable` together mean a Section that
does not move.

### `root`

Keeps the Section whether or not any Chunk names it. Use it for memory the
hardware reads with no Reference in the code, such as a display list. See
[Roots](13-roots.md).

The entry Phase's entry Label is the other Root and needs no mark.

### `in NAME`

Puts the Section in a [Pane](06-panes.md) the Project declares. The Section is
then `absolute` and not `movable`. A pin must fall within the Pane's Window.

Only code of the same Pane may name what a Pane holds, unless a
[`.with`](#with) shows it.

### `temporary`

Makes the Section a [Temporary](#ztemp-and-temp). It holds reservations only. It
goes with none of `in`, `movable` or `root`.

## `.proc`

```asm
.proc NAME [ , attribute ]...
.endproc [ then NAME ]
.endp    [ then NAME ]

attribute = placement | movable | root | in NAME | under NAME | as NAME
```

A Proc is a Section of its own that holds code. It takes the attributes a
`.section` takes, less `temporary`. It stands at the top level of a Module,
never inside a `.section` and never inside another Proc.

`.proc NAME` defines a Label at the first byte of the code.

| Attribute | Meaning |
|---|---|
| `under NAME` | the Proc runs with Pane `NAME` shown. See [Panes](06-panes.md). |
| `as NAME` | the Proc's arguments are Proc `NAME`'s Temporaries |

`under` names a Pane and not a family. It stands on a `.proc` and not on a
`.section`, and not beside `in`.

`as` names a Proc and not data. The member declares no argument and no result of
its own, because its signature is the type's.

### A Proc is a scope

A name defined inside is `NAME.inner`, and you write it that way from outside. A
plain `inner` there means nothing. Two Procs may use one name for a label.

A name inside may hide one outside. A [leading dot](#namespaces) reaches past
it.

### A Proc has one entry

From outside, nothing names a position inside a Proc. You may not call one, read
a table kept there, or patch an operand there.

A second Label at the Proc's first Chunk is the entry under another name.

What has to be reached from outside belongs in a `.section`.

### A Proc holds code

A `.res` inside a Proc is an error. Use a `.ztemp`, a `.temp`, or a
`.section`.

A `.section` block inside a Proc opens a Section beside it. The Proc's text
resumes after `.ends`, and the code before and after the block is one run of
Chunks.

A `.ztemp` or a `.temp` inside a Proc declares a Section beside it and takes the
Proc's scope. `worker.ptr` is how a caller names an argument the Proc reads.

### `.declare`

```asm
.declare arg [ TYPE | PLACE ]
.declare ret [ TYPE | PLACE ]

TYPE  = u8 | i8 | u16 | i16 | bool | u8[N]
PLACE = a letter per byte from the low one: a, x or y for a register,
        m for a byte of the reservation
```

`.declare` stands on the line above the `.ztemp` or `.temp` it names.
`.declare arg` makes that variable an argument. The order of the `.declare arg`
lines is the order of the arguments. `.declare ret` makes one the result.

```asm
.proc twice
        .declare arg u8
        .declare ret u8
value   .ztemp 1
        lda value
        asl
        sta value
        rts
.endp
```

Two `.declare`s may stand over one reservation. That byte is then both an
argument and the result.

The type is optional. Where you write one, the reservation under it must hold
exactly its bytes. Where you write none, the shape answers: one byte is a `u8`,
two a `u16`, N a `u8[N]`.

A place puts a byte in a register instead. `.declare arg a` is one byte in `A`.
`.declare ret ax` is two bytes, the low one in `A` and the high one in `X`.
`.declare arg am` is two bytes, the low one in `A` and the high one in the
reservation below.

A declaration whose bytes are all in registers takes no reservation. A register
carries one argument. A place names no register twice. The result may use a
register an argument came in.

The declarations are the contract of both sides of a call. A caller writes
`draw.x`, calls `jsr draw`, and reads `draw.result`. An exported Proc exports
its declared bytes with no `.export` of its own.

### `.endp then NAME`

Chains this Proc to `NAME`. The solver places `NAME` immediately after this
Proc, in every Phase. Control may fall through, and a conditional branch may
reach `NAME`.

Five rules hold the pair together. `NAME` must be a Proc of this Module. It
follows one Proc only. It is neither pinned nor aligned. The two are both
`movable` or neither. The two are in one Pane or in none. A chain that comes
back to where it started is an error.

### Branch reach

A Bcc or a [Jcc](12-instructions.md) reaches its own Section, or the entry of a
Proc chained to it by `then` in either direction. Nothing else. Use `jmp`
anywhere else.

## Data

```asm
.byte ITEM [ , ITEM ]...
.word ITEM [ , ITEM ]...
.hex    STRING
.binary STRING
.base64 STRING
.res EXPRESSION
.assert EXPRESSION
```

`.byte` takes `Integer` or `String` items. `.word` takes `Integer` or `Address`.

`.hex`, `.binary` and `.base64` emit a payload: bytes written as characters of a
string literal.

```asm
.hex    "ad 12 f4"
.binary "00011000 00111100"
.base64 "ICEiIyQlJicoICEiIyQlJicoICEiIw=="
```

| Directive | Alphabet | Bytes |
|---|---|---|
| `.hex` | `0-9 A-F a-f` | 2 characters, 1 byte |
| `.binary` | `0 1` | 8 characters, 1 byte |
| `.base64` | `A-Z a-z 0-9 + / =` | 4 characters, 3 bytes |

Space and tab inside the literal are ignored. A character outside the alphabet
is an error, and so is a length that does not divide into whole bytes.

A payload operand carries no character set prefix. A prefix says translate this,
and a payload holds bytes that were never characters.

`.res` reserves space that occupies addresses and emits no bytes. Its operand is
an `Integer` expression. `.res` takes no fill value.

Reserved space has no defined value on entry to a Phase. It is not zero, and it
is not what it held on the last visit, unless the Section is present in every
Phase in between. Initialise your variables on entry.

What a Container holds of a Section is its initialised extent: from the first
statement that emits bytes to the end of the last. Reserved space in front of
that or behind it costs nothing in storage. Reserved space between initialised
bytes travels with them and holds whatever the load leaves there.

`.assert` states a condition checked after Place. Its expression is the one the
tool cannot fold early.

## `.ztemp` and `.temp`

```asm
NAME .ztemp EXPRESSION
NAME .temp EXPRESSION
```

A Temporary is a Section whose value is not needed while no Section that names
it is running or waiting for a call to return. Two Temporaries never active at
the same time share an address.

The declaration is your contract and the tool does not check it. A plain `.res`
never shares.

`.ztemp` declares a zero page Section of `EXPRESSION` bytes, with `NAME` a Label
at its start, and `temporary`. `.temp` is the same off the zero page. Neither
takes an attribute. A Temporary that wants one is written as a `.section
temporary` block.

Two things the tool refuses. You may not take the address of a Temporary, since
it has none to give while it is not active. A Temporary owned by a Section that
a call from it comes back to is an error, since the second activation destroys
the first value.

A path that comes back through a `.transition` is not such a path. The routine
jumps to the entered Phase's entry and resumes no frame.

## `.transition`

```asm
.transition NAME
```

Enters the Phase named and never returns. What follows it is reached only by a
Label. It emits bytes and stands where an instruction stands.

`NAME` must be a Phase of the Project. From every Phase in which the enclosing
Section is present there must be a `then` to `NAME`.

## References across Phases

A Reference from a Section present in Phases *R* to memory present in Phases *S*
is legal only where every Phase of *R* is in *S*. Otherwise it encodes an
address that holds something else in some Phase the referrer runs in, and the
tool refuses it.

The rule follows a Constant to the Label behind it. Every use in a statement
that emits counts.

Two things are exempt, because they are facts about a Section rather than
memory: `runtimeSectionSize` and `resident`. `.assert` emits nothing and may
look anywhere.

`.transition` crosses Phases and is not a Reference in this sense.

## Slots

```asm
.slot NAME, binding [ , placement ]
.implements SLOT, SYMBOL

binding   = pointer | vector
placement = zeropage | absolute
```

A [Slot](07-slots.md) is a Symbol with at most one live definition per Phase. It
is reached through a Cell the Transition routine rewrites on every edge.

```asm
.slot update, vector                 ; jsr update
.slot screenPtr, pointer, zeropage   ; lda (screenPtr),y

.implements update, levelUpdate
```

The Binding is required. `vector` is a `jmp` and an address. `pointer` is an
address alone. The placement is the Cell's and defaults to `absolute`.

`.implements` says that SYMBOL fills SLOT wherever that Module is present.
SYMBOL is a Label or a Section of the Module the directive stands in. Write both
names in that order, anywhere in the Module. Export a Slot used across Modules.

At most one Implementation is live in any Phase. A use of the Slot from a Module
present in a Phase with no Implementation is an error at the use. A Phase with
no Implementation and no use is fine.

An Implementation stands in no Pane.

## `.with`

```asm
.with PANE
.with FAMILY, x
.with WINDOW, x
.with WINDOW = STATE
```

Shows something over the one statement that follows. Then shows again what stood
before. See [Panes](06-panes.md).

| Form | Shows | State known |
|---|---|---|
| `.with PANE` | that Pane | after Place |
| `.with FAMILY + n` | member `n` of a family | after Place |
| `.with FAMILY, x` | the member whose state is in `X` | at run time |
| `.with WINDOW, x` | the state of the Window that is in `X` | at run time |
| `.with WINDOW = STATE` | a named state of the Window | where you write it |

Several `.with` lines over one statement nest, each on a different Window. The
tool shows them in textual order and puts them back in reverse.

A `.with` that shows what the Window shows already changes nothing. The tool
says so.

The statement under a `.with` may not return. It would leave the Window as
shown. A `jsr` returns to where the exit stands, so you may write one.

Code in a Pane of the Window cannot switch that Window. The switch would take
the code with it.

A `.with` may stand in a macro body over one statement of the body. What it
names may be a parameter.

### `WINDOW = STATE`

`STATE` is a named state of that Window.

The variant lists a Window's states after `views`. A name there is a unit set,
which contributes one state per Bank and has no name of its own, or a named
state of the hardware. Only a named state may stand after `=`.

```asm
window os $C000 .. $CFFF, $D800 .. $FFFF  views rom, ram  base rom
```

```asm
        .with os = ram
        lda underTheRom
```

`rom` and `ram` are named states. A unit set's name is `NGA3003` here, and so is
a name the Window does not list.

You name the state where you write it, so the tool knows what the statement
sees.

### `WINDOW, x` and `FAMILY, x`

`X` holds the state to show. The tool hands it to the driver's `showAt` role.

Use these two forms where the state is data. A loop over Banks is the case they
exist for.

Where you know the state at the line you write, use `.with PANE`.
`ldx #level` is legal, and `.with ext, x` after it shows the right Bank. The
tool then knows nothing about what stands there. It refuses every name of the
Pane under the statement. `.with level` shows the same Bank and keeps the names.

#### Where a state number comes from

You do not write it. A Pane's name is the state the solver gave it. A family's
member is `FAMILY + n`.

```asm
.section
states  .byte level, tables, tables + 1
statesEnd
.ends
```

The solver decides those three bytes. Read one at run time and hand it over:

```asm
        ldy #0
@each   ldx states,y
        .with ext, x
        jsr readThrough
        iny
        cpy #statesEnd - states
        bne @each
```

Only `x` works. `.with ext, y` is an error.

A Pane's value stands in three places: an immediate operand, an item of `.byte`
or `.word`, and the argument of a driver's role. You may compute nothing on it,
so `.assert level == 4` is an error. The one admitted operation is `FAMILY + n`,
with `n` from zero to the count.

A unit set's name is the count of its Banks. `extension` is 4 where the variant
declares four Banks.

#### What you may do under one

You may name nothing of that Window. The tool can promise no state, so it
refuses the name.

The rule follows every `jsr` and `jmp` out of the statement. A routine two calls
away that names a Pane's Section is caught:

```asm
error[NGA3008]: what this statement reaches names `levelData`, in pane `level`,
                which this `.with` does not show
```

You may reach the memory through a pointer. The rule holds over Symbols and not
over addresses.

A Label of the Pane cannot build that pointer. `lda #<levelData` outside the
Pane is `NGA2412`, as `lda levelData` is. Use the Window's own address, which
the variant declares:

```asm
extWindow = $4000               ; the range the variant gave `ext`

        lda #<extWindow
        sta ptr
        lda #>extWindow
        sta ptr+1
        ldx states
        .with ext, x
        jsr readThrough         ; reads (ptr),y
```

Put the Window's address in a pointer, show the state, and work through the
pointer. `fixed` memory and the hardware stay visible.

These two forms suit data laid out the same way in every Bank. To name what one
Bank holds, declare a Pane and write `.with PANE`.

## `.own` and `.root`

```asm
.own [ NAME [ , NAME ]... ]
.root
```

Stands on the line before a statement that takes an address: `#<name`, `#>name`,
an item of `.byte` or `.word`. See [Roots](13-roots.md).

`.own` with no name says this Section follows the address. `.own NAME` names the
follower: a Section, a Proc, or a Label, and several separated by commas.

The tool takes your word for it and checks nothing, except that the follower
must be present wherever the code it follows is.

`.root` says the address goes to the hardware. What it names becomes a Root from
there.

An address of code taken with neither is an error. So is the address of a
Temporary.

A Section that jumps through a pointer and owns no address of code is an error,
because it would jump nowhere.

Where a table holds positions of the Section that jumps through it, use
[`.dispatch`](#dispatch) instead. A `.own` on such a table leaves the flow
unknown.

Either directive before anything but an instruction, a `.byte` or a `.word` is
an error. Both on one statement is an error.

## `.dispatch`

```asm
.dispatch TARGET [ , TARGET ]...
```

Control goes to one of the positions named, chosen by the value in `A`, and
never to the statement after it. See [Instructions](12-instructions.md).

Every target is a position of the Section the statement stands in. A target may
stand more than once, which is how you write holes and a default.

The caller holds the value below the number of targets. The statement does not
check.

Written over several rows it is one statement. A `.dispatch` with no Label
between it and the one above continues it. A Label between two rows ends the
statement.

The form reaches 128 targets on a 65SC02 and 256 on a 6502. Above that is an
error.

It stands in a Section or a Proc and not in a macro body.

## `.macro`

```asm
.macro NAME [ PATTERN ]
  ...
.endm
.endmacro

NAME [ item [ , item ]... ]

PATTERN = NAME [ , NAME ]... [ ... ]
item    = expr | NAME...
```

A macro is a body assembled once into a template and instantiated at each use.
It is never text replayed. See [Macros](09-macros.md).

A definition stands at the top level of a Module, outside `.section`, `.proc`
and any other `.macro`.

The body holds instructions, `.byte`, `.word`, a payload, labels, uses of other
macros, and a `.with` over one of its statements. A Constant, a reservation, a
Section, a `.transition` or any other directive is an error.

A use is the macro's name where a mnemonic stands, indented like one, with
comma-separated arguments. An argument is an expression and nothing else: no
`#`, no `,x` or `,y`, and no parenthesis that indirects.

A name may be qualified, such as `one.fill` or `nga.read`. A qualified name is a
use whatever follows it, because no instruction is qualified.

A macro is a Symbol of kind `Macro`. It is private to its Module, exported with
`.export`, and never a value.

A macro may use itself. The body's conditionals are decided before any Chunk is
instantiated, so a use in a branch not taken is never expanded:

```asm
.macro fill n
.if n > 0
        .byte n
        fill n - 1
.endif
.endm
```

A recursion whose condition never turns false is an error at 1024 uses deep, or
at 65536 statements produced.

An argument with a [declared value](#declared-values) is substituted as that
value. This keeps `fill n - 1` one node deep however far the recursion goes.

An argument that adds a declared number is carried as a sum. `walk n - 1, at + 2`
substitutes `table + 6` at the fourth use, not `table + 2 + 2 + 2`.

A name in the body means what the macro's own Module sees. A name in an argument
means what the use's Module sees.

A label of a body is the body's, per expansion, and appears in no symbol table.
It is unique in its body and may not take a parameter's name.

A use is one Chunk. A Label before it names its start. Nothing outside can name
a position inside.

### Packs

```asm
.macro NAME [ NAME , ]... NAME...

.match NAME
.case [ PATTERN ]
  ...
.case [ PATTERN ]
  ...
.endmatch
```

The last parameter of a macro may be a pack, written `rest...` with the dots
touching the name. It takes every argument the names before it do not. Only the
last parameter may be one.

A pack has two readings, and the parser tells them apart by where the name
stands:

- `rest...` as an item of a list spreads the pack. Its elements stand in its
  place. An empty pack spreads to nothing. A spread is an item and never an
  operand.
- `rest` alone in an expression is how many elements it has. It is an `Integer`
  with a declared value.

`.match` chooses a `.case` by how many elements a pack has. A `.case` fits when
the pack has exactly as many elements as the pattern's fixed names, or at least
that many when the pattern ends in a pack of its own. The first case that fits
is kept. A `.case` with no names fits the empty pack.

```asm
.macro push args...
.match args
.case first, rest...
        lda #first
        pha
        push rest...
.case
.endmatch
.endm
```

No case fitting is an error at the use. A case an earlier one always fits before
is a warning at that case.

A `.match` names a pack and stands in a macro body. It holds `.case` blocks and
nothing before the first, at least one, and closes with `.endmatch`.

## Conditional assembly

```asm
.if CONDITION
  ...
.elsif CONDITION
  ...
.else
  ...
.endif
.endi
```

Chooses which statements are assembled. `.elsif` may repeat. `.elsif` after
`.else` is an error. See [Conditionals and packs](10-conditionals.md).

The condition must be a [declared value](#declared-values). It decides how many
bytes there are, so a condition naming a position or a size would make a size
depend on itself.

The tool answers the condition once every Module's Symbols are known and before
anything walks a Chunk. A condition inside a branch that was not taken is never
asked. A macro use in one is never expanded.

A branch holds what a macro body holds: instructions, `.byte`, `.word`, a
payload, `.res`, macro uses and a `.with` over one of them. A Section, a Proc, a
macro, a Namespace, a Charset, a Constant, a Temporary, a `.transition`, an
`.assert`, an `.export` or a `.off` is an error.

No Symbol is ever conditional. Write a label in a branch as `@name`.

`.if` may stand in a `.section`, a `.proc` or a macro body. In a `.section` a
branch holds no labels at all, because `@` needs a Proc or a macro body.

A local label of a branch belongs to that branch. A definition is visible in the
same branch or one it contains, and nowhere else.

In a macro body the condition may name a parameter. The tool answers it per use,
before the body is instantiated.

## Namespaces

```asm
.namespace NAME [ . NAME ]...
  ...
.endnamespace
.endns
```

A Namespace is a scope for Symbols reached with a dot. A Symbol defined inside
`.namespace one` is `one.start`. `.namespace one.two` opens both and closes with
one `.endns`.

A block stands at the top level of a Module, or in a Proc, and in no Section and
no macro body. It holds Sections, Procs, Temporaries, Constants, Slots and
macros.

A block in a Proc holds what stands beside the Proc, and not the Proc's code,
data, labels or declarations.

Several Modules open one Namespace by opening blocks of one name.

`.charset` stands at the top level and nowhere else.

### How a name is found

The tool searches from the block outwards. Inside `.namespace one.two`, `NAME`
means `one.two.NAME` if that exists, then `one.NAME`, then `NAME`. At each step
it takes this Module's own Symbols first and the exported ones second.

A qualified name such as `one.gfx` is the same search applied to the whole.

A Namespace is not a value. A Symbol named exactly as a Namespace is an error,
except for the Label of a `.proc`.

A name defined inside a block hides the one its Module defines under the same
bare name.

A leading dot means the Module's top level. Inside `.proc worker`, `.count`
means the `count` at the top level whatever the enclosing scopes define.

`.export`, `.transform`, `.driver`, `.implements` and `then` resolve the name in
the block they stand in and take no qualification.

`nga` is the tool's Namespace. No Module opens or defines it.

## `.charset`

```asm
.charset NAME [ : BASE [ ^ EXPRESSION ] ]
  STRING = EXPRESSION
  ...
.endcharset
.endch
```

Declares a Charset and defines a Symbol of that kind. It emits nothing. See
[Character sets](11-character-sets.md).

An entry maps a run. The first character of the string takes the byte on the
right and the rest follow consecutively.

```asm
.charset font
  "ABCDEFGHIJKLMNOPRSTUWYZĄĆĘŁŃÓŚŹŻ" = $00
  "0123456789"                        = $20
  " .,!?"                             = $2A
.endch
```

The order is the one you typed. There is no range form. A character literal on
the left is an error, so write a one-character run as `"A" = $21`.

A run is a list of code points rather than text. It may hold whatever the font
draws. It carries no prefix.

The right side is an `Integer` with a [declared value](#declared-values). A run
that passes `$FF` is an error.

Two code points may share a byte and the tool says nothing. Mapping one code
point twice inside a block is an error.

The tool checks a prefixed literal once every declaration in the program is
known. A character its set does not map is an error there.

### Derivation

```asm
.charset inv : atascii ^ $80
.endch
```

A derived Charset takes every mapping of its base and applies one transform to
each byte. `^` is the only transform.

Entries in a derived block override the base and may add a code point the base
does not map. The order is base, then transform, then overrides.

The base may come from another Module. A Charset derived from itself is an
error.

A block is closed whether or not it holds anything. A block holds entries, blank
lines and comments, and nothing else.

## `.export`

```asm
.export NAME [ , NAME ]...
```

A Symbol is private to its Module until you export it. Any kind may be exported:
a Label, a Constant, a Charset, a macro, a Slot.

Exporting a Section name lets another Module read its
[attributes](#attribute-access). A Section is not a value, so its bare name is
an error wherever an operand belongs.

`.export` emits nothing. It may stand anywhere a statement may stand, and before
the definition as readily as after it.

Exporting a name the Module does not define is an error. So is exporting a
position inside a Proc.

An export is not a Root. A Module that uses the name provides the Reference that
keeps it.

The Project needs no export to reach a Section.

## `.driver` and `.transform`

```asm
.driver ROLE NAME
.driver ROLE WINDOW NAME
.driver stream WINDOW

.transform FORMAT LABEL
```

`.driver` declares that this Module is the storage driver and what provides each
role. One Module of the program declares the driver, and its Module is resident.

`.transform` declares that this Module's `LABEL` decodes `FORMAT`. The Label is
one of this Module's and the declaration exports it. A format has one decoder
across the program. A program with a Transition declares one for `copy`.

## `.off`

```asm
.off CODE [ , CODE ]...
```

Silences the warning each `CODE` names on the statement that follows, and
nowhere else. Several `.off` lines before one statement all apply to it.

```asm
.off NGA5218
.section absolute at $0100
stackBottom
        .res 16
.ends
```

Only a warning is silenced. An error never is, so a `deny` outranks a `.off`.

A `.off` that silenced nothing is a warning of its own, so one left behind after
the finding went away does not sit in the source looking like it works.

A `CODE` naming no diagnostic is an error.

## `.source`

```asm
.source "PATH", LINE
```

Says that the text from here to the next `.source` came from `LINE` of the file
`PATH`. A [generator](08-generators.md) writes this into the assembler it emits,
so that a finding on its output names the line of its input.

The path is a bare string. The line counts from 1. The directive emits nothing
and applies by position, so it stands wherever a line may stand.

## Expressions

### Types

| Type | Produced by |
|---|---|
| `Integer` | numeric and character literals, `label - label`, `runtimeSectionSize`, `resident`, comparisons, byte extraction |
| `Address` | a Label, a Slot, `label + n`, `runtimeSectionAddress` |
| `String` | a string literal |
| `Section` | the name of a Section, only before a dot |
| `Pane` | the name of a Pane, and a family's member `pane + n` |

A Charset is not an expression type. A Charset names itself only as a literal's
prefix.

### Operators

```asm
-x  ~x  !x          Integer -> Integer;  an Address is an error
<x  >x              Integer -> Integer;  Address -> Integer

a + b               Int+Int -> Int   Addr+Int -> Addr   Addr+Addr is an error
a - b               Int-Int -> Int   Addr-Int -> Addr   Addr-Addr -> Int
                                                        Int-Addr is an error
a * b   a / b       Int, Int -> Int
a << b  a >> b      Int, Int -> Int
a & b  a | b  a ^ b Int, Int -> Int  Addr, Int -> Int
a == b  a != b      Int, Int -> Int  Addr, Addr -> Int  Addr, Int -> Int
a < b   a <= b      Int, Int -> Int  Addr, Addr -> Int  Addr, Int -> Int
a > b   a >= b
a && b  a || b      Int, Int -> Int
a ? b : c           Int, then two of one type -> that type
```

`String` and `Section` have no operators.

Negating one Address is an error although subtracting two is not. The distance
between two positions is a quantity. The negation of one position is not.

Bitwise operators accept an Address with an Integer and yield an Integer, so
`buffer & $FF00` is a page base.

Comparisons accept an Address with an Integer, so `.assert displayList == $2000`
asks the question `.assert` exists to ask.

### The conditional value

```asm
CONDITION ? TAKEN : OTHERWISE
```

Chooses between two values. The condition is an `Integer`. The two answers must
be of one type, which is the type of the whole. That type is an `Integer` or an
`Address`.

The tool evaluates the condition and the answer taken, and nothing else. So
`narrow ? 4 : buffer.runtimeSectionSize` has a declared value when `narrow`
holds, and it can decide the width of an instruction.

Two `Address` answers must lie in one Section and one space.

This is a conditional value and not conditional assembly. Both answers are
References, so both keep what they name from being pruned.

### Subtracting addresses

`Addr - Addr` is legal only where both sides lie in the same Section and the same
space. A comparison between two Addresses follows the same rule. One against an
Integer does not.

The tool refuses the rest because the number would be a property of the layout
rather than of the program. Another build may produce a different one with no
change to the source.

Within one Section the rule gives two things. `dataEnd - data` is the size of
what lies between them, known after Size and before any address exists. The
offset of a Label inside its Section is `label - mySection.runtimeSectionAddress`.

### Precedence

Weakest to strongest:

```asm
? :                   conditional, right-associative
||
&&
|
^
&
==  !=
<  <=  >  >=          comparison
<x  >x                byte extraction, prefix
<<  >>
+  -
*  /
-x  ~x  !x            other prefixes
.                     attribute access
```

Every level except byte extraction is C's, in C's order. The conditional is C's
too: loosest of all, and right-associative.

Byte extraction is the one level C does not have. It binds looser than
arithmetic and tighter than comparison, so `<foo+1` is the low byte of `foo+1`,
and `<foo == 0` is `(<foo) == 0`. Write `(<foo)+1` for the other reading.

Two combinations require parentheses rather than being resolved:

```asm
a + b << 2            ; error: parenthesise
a & $FF == 0          ; error: parenthesise
```

The parser refuses to guess in exactly these two places and names the two
readings it is between.

### Depth

Operators nest at most 64 deep, and so do parentheses. The two are counted
apart.

### Attribute access

```asm
label.runtimeSectionAddress     Address: where the Section starts
label.runtimeSectionSize        Integer: the Section's size, reservations included
label.resident                  Integer: 1 when the Section is present in every Phase
```

The attributes belong to a Section. Every Label of the Section reaches them,
wherever in it the Label stands. Anything that is not a Label has no attributes.

A name inside a Proc cannot take them, because `NAME.runtimeSectionSize` is the
Proc's size.

`resident` is read from the Project. `.assert vars.resident` is how source states
that it depends on a Section being present in every Phase.

`storagePayloadAddress` and `storagePayloadSize` are reserved and not built.

## Declared values

An expression has a declared value when every leaf of it is a numeric or
character literal, or a Constant whose own expression has a declared value.
Nothing that names a position or a size may appear in it.

The rule is about the shape of the definitions and not about when a value
becomes available. `lda VBLANK + 2` is legal whether `VBLANK` is defined above,
below, or in another Module. A cycle among Constants is an error.

A declared value is required by `at`, `align`, `within`, a `.charset` entry, a
derivation mask, an `.if` condition, and any operand that is an `Integer` rather
than an `Address`.

### Integer operands

An operand may be an `Integer` rather than an `Address`. Use this to reach a
hardware register. A register the Target names is such an `Integer`.

```asm
lda $D01A
lda COLBK
lda VBLANK + 2
```

Such an operand has no PlacementClass to inherit, so its value decides the width
of the instruction. That is why the value must be declared.

`lda dataEnd - data` has Labels at its leaves and has no declared value. The
tool reports that it cannot determine the placement class.

A hardcoded address is a claim the solver cannot see. What the tool says depends
on the Region the address falls in:

| Where the address lands | Reported |
|---|---|
| where the solver never allocates: hardware, ROM, or a reserved Region | a note, and only where a name exists that you could have written instead |
| in allocatable RAM covered by no reserved Region | a warning: the solver may give that byte to something else |

Severity is controllable per identifier from the Project.

## Abbreviations

A directive that closes a block has two spellings: the full name and one
unambiguous prefix of it.

| Full | Short |
|---|---|
| `.endsection` | `.ends` |
| `.endproc` | `.endp` |
| `.endmacro` | `.endm` |
| `.endnamespace` | `.endns` |
| `.endcharset` | `.endch` |
| `.endif` | `.endi` |

`.end` alone is never legal, because it does not say which block it closes.
`.endch` is one letter longer than the rule alone would make it, because
`.endc` would stop being unambiguous later.

Nothing else in the language is abbreviated.

## Errors

The parser reports every error in a file. An unclosed `.section`, `.proc` or
`.macro` is reported at the directive that opened it.

The parser does not resolve names. An undefined symbol is not a parse error, and
a file full of names declared in other Modules parses cleanly.
