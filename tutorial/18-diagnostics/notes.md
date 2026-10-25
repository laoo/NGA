Entries for chapter 18, one per identifier, joined into it by the `catalogue`
marker. The heading is the identifier and nothing else. Everything under it
until the next heading is the entry.

An entry says when the tool raises the finding and what to do about it. Its
fences in `asm`, `c` and `ngp` are the files of one example, which
`scripts/tutorial.py` builds: what the tool said is put after the last fence,
and an example that does not raise its own identifier fails the run. A link
to a chapter is written from this file, `../07-slots.md`, and the script makes
it the chapter's own. The script's header is the contract.

## NGA0001

Outside strings and comments, an `.asm` Module or a Project file holds ASCII
letters, digits and the punctuation of the language. Any other character there
raises this, such as a backslash or a non-ASCII letter. Typographic quotes
pasted from a word processor are a common case.

```asm
.section
title   .byte “HELLO”
.ends
```

Type the ASCII character that was meant, here `"`.

## NGA0002

A string literal is still open where the file ends: the last line lacks its
closing `"`, and no line ending follows it. The same mistake on any other line
is [`NGA0003`](#nga0003). Close the string.

## NGA0003

A string literal ends on the line it starts on, and this one reaches the end of
its line without a closing `"`. There is no line continuation. Everything after
the opening quote belongs to the string, a `;` comment included.

```asm
.section
title   .byte "PRESS START
.ends
```

Close the string. A longer text is several strings, on one `.byte` line or on
several.

## NGA0004

A character literal opens with `'` and is not closed before the end of its line.
An apostrophe inside one is written `\'`.

```asm
.proc entry
        lda #'A
        rts
.endp
```

Close it: `'A'`.

## NGA0005

A character literal stands for the value of one character, and `''` holds none.
For a space write `' '`, and for the zero byte `'\0'` or `0`. An apostrophe is
`'\''`: `'''` reads as an empty literal followed by an open one.

```asm
.section
blank   .byte ''
.ends
```

## NGA0006

A character literal is the value of one character. An escape such as `'\n'`
counts as one. For several bytes, write a string: `.byte "AB"`.

```asm
.section
keys    .byte 'AB'
.ends
```

## NGA0007

A backslash in a string or a character literal starts an escape. The escapes are
`\\`, `\"`, `\'`, `\n`, `\t` and `\0`. There is no `\xNN` and no `\r`.

```asm
.section
prompt  .byte "READY\x9B"
.ends
```

A byte no escape spells goes beside the string in the data directive:
`.byte "READY", $9B`.

## NGA0009

Raised inside a comment, a string or a character literal whose bytes are not
UTF-8. Outside them, such a byte is [`NGA0001`](#nga0001). A comment or a
literal reports it once. The usual cause is a file saved in a legacy code page
with accented letters in its comments. Save the file as UTF-8.

## NGA0010

`$` begins a hexadecimal number and `%` a binary one, and here no digit follows.
The language has no remainder operator, so `%` written as one raises this.

```asm
COUNT = 20

.proc entry
        lda #COUNT % 8
        rts
.endp
```

For a power of two, `COUNT & 7` is the remainder of division by 8.

## NGA0011

A number has one base: decimal with no prefix, hexadecimal after `$`, binary
after `%`. Raised for a digit the base does not have, such as `2` after `%`,
and for a letter that touches a number, which is read as one of its digits.
`0FFh`, the notation of other assemblers, is read as a decimal number, and `F`
is not a decimal digit.

```asm
.proc entry
        lda #0FFh
        rts
.endp
```

Write `$FF`. A number reports this once, however many of its digits are wrong.

## NGA0012

A `_` groups the digits of a number, as in `%1010_0101` or `65_536`. It needs a
digit on each side: it cannot follow the prefix, end the number, or stand next
to another `_`.

```asm
.section
mask    .byte %_1010_0101
.ends
```

A number reports this once, however many separators are misplaced in it.

## NGA0013

Hexadecimal is written with `$`. `0x` is C's notation, and neither an `.asm`
Module nor a Project file takes it. A `.ngc` Module does.

```asm
.proc entry
        lda #0x1F
        rts
.endp
```

## NGA0014

A local label is named, `@loop`, or anonymous, `@`. A digit after `@`, `@+` or
`@-` is refused: `@1` is not a name, and `@-2` does not count two labels back.

```asm
.proc entry
        ldx #8
@1      dex
        bne @1
        rts
.endp
```

Name the label, or write `@` and reach the nearest one with `@-` or `@+`. See
[Local labels](../16-assembler.md#local-labels).

## NGA0015

A control character, a byte below `$20` or the byte `$7F`, stands outside a
string, a character literal and a comment. Tab, line feed and carriage return
are allowed. The usual cause is a form feed or another byte left by an editor
or a file conversion. Delete it; the location shows where. The line quoted under
the message shows the byte as its symbol from Unicode's Control Pictures, `␀`
for a NUL and `␌` for a form feed.

## NGA0016

The tool computes with signed 64-bit integers, so the largest literal is
`$7FFF_FFFF_FFFF_FFFF`.

```asm
.section
big     .word $1_0000_0000_0000_0000
.ends
```

A value that is representable but does not fit the byte or word it is written
to is [`NGA6101`](#nga6101).

## NGA0017

A decimal literal of more than one digit starts with `0`. C reads such a
literal as octal, so `017` is 15 there and 17 to anyone who never learned C.
The tool takes neither reading. `0` alone is zero, and a `0` after `$` or `%`
is an ordinary digit.

```asm
.proc entry
        ldx #017
        rts
.endp
```

Drop the zero, `ldx #17`, or write the value in hexadecimal or binary,
`ldx #$0F`. The same rule in a `.ngc` Module is [`NGA7006`](#nga7006).

## NGA0100

The grammar needs a value here and finds none. Typical causes are an
operand left out, as in `lda #`, a comma at the end of a list of data, and `*`
written for the current address, which the language does not have. Put a Label
where an address is wanted.

```asm
.section
table   .byte 1, 2, 3,
.ends
```

## NGA0101

A `(` in an expression or an operand has no `)` before the end of the line. The
location is the `(` left open.

```asm
WIDTH = 40

.proc entry
        lda #(WIDTH * 2
        rts
.endp
```

## NGA0102

`<` in front of an operand takes its low byte, and `>` its high byte. `<<` is
the left shift and needs a value on each side. Written in front of an operand
it is read as `<`, so nothing else is reported.

```asm
.proc entry
        lda #<<screen
        rts
.endp

.section
screen  .res 40
.ends
```

Remove one `<`.

## NGA0103

Two pairs of operators are refused without parentheses: a shift with `+` or
`-`, and `&`, `|` or `^` with a comparison. C gives both an order that is often
not the one meant: it reads `a & $FF == 0` as `a & ($FF == 0)`. The tool names
the two operators and does not choose.

```asm
ROWS = 24

.proc entry
        lda #ROWS + 1 << 2
        rts
.endp
```

Add the parentheses for the reading you mean, `(ROWS + 1) << 2` or
`ROWS + (1 << 2)`. See [Precedence](../16-assembler.md#precedence).

## NGA0104

An expression nests at most 64 operators deep, and at most 64 parentheses
deep, the two counted apart. A chain such as `1+1+1` counts each operator, so a long sum
written term by term reaches the limit. Split it into Constants that each hold
a part: the limit applies to each expression on its own.

## NGA0110

A statement begins with a mnemonic, a macro's name or a directive. At column one
a line may also begin with a Label or a Constant. Raised for a line that begins
with anything else. Two common cases come from other assemblers: `*= $2000` to
set the address, and a comment written with `//`.

```asm
*= $2000

.proc entry
        rts
.endp
```

A comment starts with `;`. A Section says where it goes with an attribute,
`.section absolute at $2000`, or names no address and the solver picks one. See
[Placement](../16-assembler.md#placement).

## NGA0111

A statement ends where its grammar is complete, and the rest of its line is
empty or a `;` comment. Raised for whatever follows: most often a comma missing
between two values, or a second statement on the same line, as in
`lda #1 : sta $80`. A line holds one statement. Not raised for a token the
lexer refused, such as `% 8`, which has a finding of its own.

```asm
.section
table   .byte 1, 2 3
.ends
```

## NGA0112

A name at column one is a Label, and what follows it on the line must be a
statement. Raised where it cannot be one. Usually an instruction with an
operand has lost its indentation: `lda #0` at column one is a Label `lda`
followed by `#0`, and also gets [`NGA0113`](#nga0113). A Label written with a
colon, as other assemblers write one, is [`NGA0199`](#nga0199) instead.

```asm
.proc entry
lda #0
        sta $D01A
        rts
.endp
```

Indent the instruction.

## NGA0113

A label in column one that is spelled like an instruction is almost always an
instruction that lost its indentation. The tool cannot tell the two apart,
because a label is marked by position and nothing else.

```asm
.proc entry
        lda #0
        sta $0600
rts
.endp
```

Here the `rts` became a label, and the Proc now runs off its end. Indent the
line. Where a label is meant to be named like this, turn the
warning off on the line below with `.off NGA0113`. A Proc's name is not such a
label: `.proc inc` names it after a directive, which no lost indent writes, so
it draws nothing, and a C function called `inc` is written that way.

## NGA0114

`@-` and `@+` belong to a reference, where they choose the nearest definition
before or after it. A definition is `@name` or `@` alone.

```asm
.proc entry
        ldx #8
@-loop  dex
        bne @-loop
        rts
.endp
```

Write the definition without the sign. Since this one is refused, the reference
below it finds no `@loop`, which is [`NGA2210`](#nga2210).

## NGA0115

A local label, `@name` or `@`, takes its scope from the `.proc` or the macro
body it stands in. In a `.section` or at the top level of a Module there is
none, and the definition and every reference are refused.

```asm
.section
clear   ldx #0
@loop   sta $0600,x
        inx
        bne @loop
        rts
.ends
```

Write the code as a `.proc`, or give the label a plain name. See
[Local labels](../16-assembler.md#local-labels).

## NGA0116

`.declare` makes a Temporary of a Proc an argument or the result. It stands
inside the `.proc`, on the line above the `.ztemp` or `.temp` it names.

```asm
        .declare arg u8
value   .ztemp 1

.proc twice
        lda value
        asl
        sta value
        rts
.endp
```

Move both lines into the Proc. See [`.declare`](../16-assembler.md#declare).

## NGA0117

`.declare` applies to the next statement, which must be a `.ztemp` or a `.temp`.
Only blank lines and comments may stand between. Raised where something else
comes first, or nothing does.

```asm
.proc twice
        .declare arg u8
        lda value
        asl
        sta value
        rts
value   .ztemp 1
.endp
```

Move the `.declare` to the line above its reservation. A `.declare` that keeps
every byte in a register, as `.declare arg a`, takes no reservation and is not
held to this.

## NGA0118

The word after `.declare` is the role: `arg` for an argument, `ret` for the
result. A type or a place may follow it, as in `.declare arg u8`.

```asm
.proc twice
        .declare u8
value   .ztemp 1
        lda value
        asl
        sta value
        rts
.endp
```

## NGA0119

After `arg` or `ret` comes a type, a place, or nothing. The count of `u8[N]` is
a number written out, from 1 to 65535, and a Constant's name is refused there.
A place is one or two letters, one per byte from the low one, and at least one
of them names a register.

```asm
LENGTH = 4

.proc clear
        .declare arg u8[LENGTH]
buffer  .ztemp LENGTH
        lda #0
        sta buffer
        rts
.endp
```

Where the size is a Constant, write no type. The size of the reservation then
gives it, so `.declare arg` over `buffer .ztemp LENGTH` is a `u8[4]`.

## NGA0120

A directive is a `.` and one of the words listed in
[The assembler](../16-assembler.md). The usual cause is a directive of another
assembler. `.org` has no counterpart: a Section says where it goes with
`.section absolute at $2000`, or the solver places it. `.db` and `.dw` are
`.byte` and `.word`. A file that `.include` would read is a Module of its own,
named in the Project's `modules` block, and a binary file is a
`binary( "file.bin" )` entry there.

```asm
.org $2000

.proc entry
        rts
.endp
```

## NGA0121

Each block has its own closer: `.ends`, `.endp`, `.endm`, `.endif`, `.endns`,
`.endch` and `.endmatch`, and `.end` is none of them. The block stays open, so
it is also reported as never closed.

```asm
.proc entry
        rts
.end
```

Where `.end` marks the end of the source, as in other assemblers, delete it: a
Module ends where its file does. See
[Abbreviations](../16-assembler.md#abbreviations).

## NGA0122

The grammar needs a name here and finds something else, or the end of the line.
Raised after a directive that declares or names something, such as `.proc`,
`.macro`, `.namespace`, `.slot` or `.transition`; after `then` at `.endp`; after
`in`, `under` or `as` among a Section's attributes; and after a `.` with no word
behind it. A number or a string is not a name.

```asm
.proc entry
        rts
.endp then
```

## NGA0123

A Section's attributes are a placement, `root`, `in NAME`, `temporary`,
`readonly` and `foreign`, and a `.proc` adds `under NAME` and `as NAME`. `at`,
`align` and `within` are not attributes. They complete a placement, so they
stand right after `zeropage` or `absolute`, with no comma.

```asm
.section at $2000
table   .byte 1, 2, 3
.ends
```

Write `.section absolute at $2000`. The word is dropped together with what
follows it up to the next comma, here `$2000`, and the Section opens with the
attributes that are left. What it holds and its `.ends` are read as usual. See
[Placement](../16-assembler.md#placement).

## NGA0124

A `.section` closes with `.ends` before the end of the file, and one opened
inside a `.proc` closes before its `.endp`. The finding stands at the
`.section` left open.

```asm
.proc entry
        inc counter
        rts
.section
counter .byte 0
.endp
```

Add the `.ends`.

## NGA0125

A `.proc` closes with `.endp` before the end of the file. The finding stands
at the `.proc` left open. A `.endp` inside an `.if` branch is refused,
[`NGA0169`](#nga0169), and leaves the Proc open.

```asm
.proc entry
        rts
```

## NGA0126

`.ends` and `.endsection` close a `.section`, and none is open here. The usual
cause is a `.proc` closed with `.ends`, which also leaves the Proc open.

```asm
.proc entry
        rts
.ends
```

Close a Proc with `.endp`. The mirror mistake is [`NGA0127`](#nga0127).

## NGA0127

`.endp` and `.endproc` close a `.proc`, and none is open here. The usual cause
is a `.section` closed with `.endp`, which also leaves the Section open. A
`.proc` line that was refused, for a missing name or a wrong attribute, leaves
its `.endp` with nothing to close as well.

```asm
.section
table   .byte 1, 2, 3
.endp
```

Close a Section with `.ends`. The mirror mistake is [`NGA0126`](#nga0126).

## NGA0128

A `.section` ends at its `.ends`, and another cannot open before that. The usual
cause is a missing `.ends`. The refused line is dropped, so what follows it
lands in the first Section.

```asm
.section
table   .byte 1, 2, 3
.section
other   .byte 4
.ends
```

Close the first Section before the second opens. Inside a `.proc`, one
`.section` at a time may stand beside the Proc's code.

## NGA0129

A `.proc` ends at its `.endp`, and another cannot open before that. The usual
cause is a missing `.endp` on the Proc above.

```asm
.proc entry
        jsr helper
        rts

.proc helper
        rts
.endp
```

Close the first Proc before the second opens. A `.proc` inside a `.section` is
[`NGA0137`](#nga0137).

## NGA0130

A Proc has one result, so it has one `.declare ret`. A result of two bytes is
one reservation of two bytes, `.declare ret u16` over `result .ztemp 2`, or a
pair of registers, `.declare ret ax`.

```asm
.proc split
        .declare ret u8
low     .ztemp 1
        .declare ret u8
high    .ztemp 1
        lda #0
        sta low
        sta high
        rts
.endp
```

## NGA0131

A Section has one placement, `zeropage` or `absolute`, with its `at`, `align`
and `within`. The second one is reported and dropped, and the first stands.

```asm
.section zeropage, absolute
ptr     .res 2
.ends
```

Keep one. A qualifier given twice in one placement is
[`NGA0132`](#nga0132), and another attribute given twice, such as `root`, is
[`NGA0133`](#nga0133).

## NGA0132

`at`, `align` and `within` complete the placement of a `.section` or a `.proc`,
and each of them is given at most once.

```asm
.section absolute align 256 align 512
table   .res 256
.ends
```

Keep one of the two. A second placement class, as in `absolute at $10,
zeropage`, is [`NGA0131`](#nga0131), and any other attribute written twice is
[`NGA0133`](#nga0133).

## NGA0133

Raised for `root`, `temporary`, `readonly`, `foreign`, `as` or `under` written a
second time on one `.section` or `.proc`. Remove the repeat.

```asm
.section readonly, root, readonly
table   .byte 1, 2, 4, 8
.ends
```

`in` is the one attribute that may repeat. Each `in` names another Region the
Section may stand in, and the solver picks one of them.

## NGA0134

A `.slot` names its Binding after a comma: `vector` for a Slot that is called or
jumped to, `pointer` for one that is read through, as in `lda (slot),y`. The
tool does not guess, because the Binding decides the Cell: a `jmp` and an
address for `vector`, the address alone for `pointer`.

```asm
.slot update
```

Here the Slot is called, so it is `.slot update, vector`. See
[Slots](../07-slots.md).

## NGA0135

The third word of a `.slot` places its Cell, and is `zeropage` or `absolute`,
spelled out. Leave it off and the Cell is `absolute`.

```asm
.slot screen, pointer, zp
```

## NGA0136

`NAME .ztemp SIZE` and `NAME .temp SIZE` name the variable they reserve, and
the name is required: a plain identifier at column one. Raised where the name
is missing, and where it is local, as `@ptr`. A Temporary is a Section of its
own, and a local label is a position inside a Proc.

```asm
.proc entry
@ptr    .ztemp 2
        rts
.endp
```

Drop the `@`. A `.ztemp` written in a Proc takes the Proc's scope, so `ptr` here
is `entry.ptr`, and another Proc may have a `ptr` of its own.

## NGA0137

A Proc is placed on its own, as a Section is, so `.proc` stands at the top level
of a Module and not inside a `.section`.

```asm
.section
table   .byte 1, 2, 4, 8
.proc entry
        lda table
        rts
.endp
.ends
```

Close the `.section` with `.ends` before the `.proc`. The `.endp` of the refused
Proc is reported as well, [`NGA0127`](#nga0127). The other way round is allowed:
a `.section` block inside a Proc opens a Section beside it.

## NGA0138

Raised for an instruction, data, a reservation, a label, a macro use or an
`.if` that stands outside every `.section`, `.proc` and macro body. What
produces bytes or names a position needs somewhere to put them, and `.section`
and `.proc` are the two places that have an address.

```asm
.proc entry
        lda table
        rts
.endp

table   .byte 1, 2, 4, 8
```

Put `table` in a `.section` block. What has no address stays at the top level: a
Constant, a `.ztemp` or `.temp`, an `.assert`, an `.export`.

## NGA0139

`.declare arg` makes the reservation below it an argument, and one byte carries
one argument. Two `.declare arg` lines above one `.ztemp` make the same byte two
arguments.

```asm
.proc add
        .declare arg u8
        .declare arg u8
left    .ztemp 1
right   .ztemp 1
        clc
        lda left
        adc right
        sta left
        rts
.endp
```

Put each `.declare` on the line right above its own `.ztemp`, rather than all
of them at the top of the Proc. Two `.declare`s over one reservation are allowed
when one is `arg` and the other `ret`: the byte then brings the argument in and
the result out. See [`.declare`](../16-assembler.md#declare).

## NGA0140

The 6502 indexes an indirect address in two ways: `(zp,x)` adds `x` before the
pointer is read, and `(zp),y` adds `y` after. There is no `(zp,y)`.

```asm
.section zeropage
ptr     .res 2
.ends

.proc entry
        ldy #0
        lda (ptr,y)
        rts
.endp
```

Write `lda (ptr),y`.

## NGA0141

The processor has `(zp,x)` and `(zp),y` and no other indirect mode. So
`(table),x` is not indirect, and the parentheses group an expression instead.
The operand that comes out is an ordinary address, and the instruction is three
bytes where the indirect form is two.

```asm
.section zeropage
table   .res 2
.ends

.proc entry
        ldx #0
        lda (table),x
        rts
.endp
```

The warning is there because the two spellings are one character apart. Write
`(table,x)` for the indirect form, or drop the parentheses for the indexed one.

## NGA0142

In an indirect operand the comma is followed by an index register, and the
registers are written `x` and `y`, in lowercase.

```asm
.section zeropage
ptr     .res 2
.ends

.proc entry
        ldx #0
        lda (ptr,X)
        rts
.endp
```

Write `(ptr,x)`. Outside parentheses, `lda table,X` is
[`NGA2304`](#nga2304) instead.

## NGA0143

`.off` takes an identifier as the tool prints it: `NGA` in capitals and four
digits. A code that names nothing is refused rather than ignored, because a
`.off` that silences nothing would look as if it worked.

```asm
.proc entry
.off nga0113
        rts
.endp
```

The Project's `diagnostics` block refuses such a code the same way, as
[`NGA1118`](#nga1118).

## NGA0144

`.off CODE` silences that warning on the statement on the next line, and
nowhere else. Raised when no such warning was reported there: the code is the
wrong one, the warning points at another line, or the finding went away and the
`.off` stayed. Not raised while the build has errors, since the Step that would
raise the warning may not have run.

```asm
.proc entry
.off NGA0113
        lda #0
        rts
.endp
```

Remove the `.off`, or move it to the line above the one the warning points at.

## NGA0145

`.export` takes a plain name and looks for it in the block it stands in, so
`worker.inner` after `.export` is refused. To export a position inside a Proc,
or a name inside a `.namespace` block, write the `.export` inside that block.

```asm
.export worker, worker.inner

.proc worker
inner   lda #$10
        rts
.endp
```

Write `.export inner` inside `.proc worker`. Another Module then reads
`worker.inner` and writes the operand of the instruction there, as [A Proc has
one entry](../16-assembler.md#a-proc-has-one-entry) allows, and never jumps to
it.

## NGA0150

A `.charset` block ends at `.endcharset` or `.endch`. Without one, the block
ends at the next line that begins with a directive, and the tool reads on from
that line as usual. The finding points at the `.charset`.

```asm
.charset digits
  "0123456789" = $10

.section
score   .byte digits"0042"
.ends
```

Add `.endch` after the last entry. A block is closed even when it holds no
entries.

## NGA0151

An `.endch` or `.endcharset` with no `.charset` open. Usually there is one too
many. It also follows a `.charset` that was refused, as one in a Namespace is,
[`NGA0164`](#nga0164), and a block that ended early at a directive inside it,
[`NGA0150`](#nga0150).

```asm
.charset digits
  "0123456789" = $10
.endch
.endch
```

## NGA0152

Each line of a `.charset` block is one entry: a string, `=`, and the byte its
first character takes. Raised for a line that starts with anything but a
string, a character literal included, and for one with no `=`.

```asm
.charset font
  "ABCDEFGHIJKLMNOPQRSTUVWXYZ" = $00
  '.' = $1A
.endch
```

Write one character as a run of one: `"." = $1A`.

## NGA0153

An entry maps a run of characters, the first to the byte on the right and the
rest to the bytes after it. An empty string maps nothing.

```asm
.charset font
  "ABCDEFGHIJKLMNOPQRSTUVWXYZ" = $00
  "" = $1A
.endch
```

Put the characters in the quotes, or remove the line.

## NGA0154

The left side of an entry lists code points of the set being declared, so it
carries no prefix. That holds in a derived set too: an entry that adds to the
base or overrides it is written plain.

```asm
.charset font
  "ABCDEFGHIJKLMNOPQRSTUVWXYZ" = $00
.endch

.charset bright : font ^ $80
  font"." = $9A
.endch
```

Write `"." = $9A`.

## NGA0155

A macro definition stands at the top level of a Module, or in a `.namespace`
block there. It does not stand inside a `.section`, a `.proc` or another
`.macro`.

```asm
.proc entry
.macro clear at
        lda #0
        sta at
.endm
        rts
.endp
```

Move the definition above the Proc. The `.endm` of the refused macro is reported
as well, [`NGA0157`](#nga0157).

## NGA0156

A `.macro` without `.endm` or `.endmacro` takes every line after it, to the end
of the file, as its body. The finding points at the `.macro`.

```asm
.proc entry
        clear $80
        rts
.endp

.macro clear at
        lda #0
        sta at
```

Add `.endm` after the last line of the body. Where a `.proc` or a `.section`
follows the unclosed macro, it stands in the body and is reported as
[`NGA0158`](#nga0158).

## NGA0157

An `.endm` or `.endmacro` with no `.macro` open. Usually there is one too many,
or the `.macro` it was meant to close was refused, [`NGA0155`](#nga0155).

```asm
.macro clear at
        lda #0
        sta at
.endm
.endm
```

## NGA0158

A macro body holds instructions and data. It does not hold declarations. A
macro is instantiated where it is used, so a declaration inside one would
declare the same name again at every use.

Besides instructions, labels and uses of other macros, a body takes `.byte`,
`.word`, a payload, `.if`, `.match`, `.with` and `.source`. Everything else is
refused: a Constant, a reservation, a Section, any other directive.

```asm
.macro wait frames
count   .ztemp 1
        lda #frames
        sta count
@loop   dec count
        bne @loop
.endm
```

Declare the Temporary in the Proc that uses the macro and pass it in as an
argument, as [Macros](../09-macros.md#what-a-body-may-hold) does. Define a Constant
outside the body.

## NGA0160

A `.namespace` block holds whole Sections, Procs and macros, so it opens outside
them: at the top level of a Module, or in a Proc, where it holds what stands
beside the Proc. It does not open inside a `.section` or a macro body.

```asm
.section
.namespace colors
red     .byte $32
.endns
.ends
```

Open the Namespace above the `.section` and close it after `.ends`. The `.endns`
of the refused block is reported as well, [`NGA0162`](#nga0162).

## NGA0161

A `.namespace` with no `.endnamespace` or `.endns` before the end of the file,
or, for one opened in a Proc, before the Proc's `.endp`. The finding points at
the `.namespace`.

```asm
.namespace sound
.proc play
        rts
.endp
```

Add `.endns`. `.namespace one.two` opens two levels, and one `.endns` closes
both.

## NGA0162

An `.endns` or `.endnamespace` with no `.namespace` open. One `.endns` closes a
`.namespace one.two` whole, so a second one for the inner name is one too many.
It also follows a `.namespace` that was refused, [`NGA0160`](#nga0160).

```asm
.namespace sound.music
.proc play
        rts
.endp
.endns
.endns
```

## NGA0163

A Namespace holds whole blocks. An `.endns` while a `.section`, `.proc` or
`.macro` opened inside the Namespace is still open would leave that block in two
scopes. In a Proc, `.endns` closes only a Namespace that was opened in the same
Proc.

```asm
.namespace sound
.proc play
        rts
.endns
.endp
```

Close the inner block first: here `.endp` goes above `.endns`. The refused
`.endns` leaves the Namespace open, which is reported too,
[`NGA0161`](#nga0161).

## NGA0164

A literal's prefix is one identifier, as in `font"A"`, with no room for a
Namespace. A Charset declared in a Namespace could not be named from outside
it, so `.charset` stands at the top level of a Module.

```asm
.charset font
  "ABCDEFGHIJKLMNOPQRSTUVWXYZ" = $00
.endch

.namespace title
.charset bright : font ^ $80
.endch
.endns
```

Move the block out of the Namespace. The `.endch` of the refused block is
reported as well, [`NGA0151`](#nga0151).

## NGA0165

The conditional value, `CONDITION ? TAKEN : OTHERWISE`, needs both answers.
Raised where the `:` after the first answer is missing.

```asm
NTSC = 1
frames = NTSC ? 60
```

Write the second answer: `frames = NTSC ? 60 : 50`. See
[The conditional value](../16-assembler.md#the-conditional-value).

## NGA0166

An `.if` without `.endif` or `.endi`. The finding points at the `.if`.

A block opens and closes within one branch. So in a Proc, the `.endp` below an
unclosed `.if` stands in its last branch and is refused,
[`NGA0169`](#nga0169), and the Proc is then reported unclosed,
[`NGA0125`](#nga0125).

```asm
FAST = 1

.proc entry
.if FAST
        lda #1
.else
        lda #2
        rts
.endp
```

An `.endif` above the `.endp` clears all three.

## NGA0167

An `.else`, `.elsif`, `.endif` or `.endi` with no `.if` open. Usually there is
one too many, or a second `.else` in one `.if`. It is also raised where the
innermost open block is a `.match`, which closes with `.endmatch` first.

```asm
FAST = 1

.proc entry
.if FAST
        lda #1
.endif
        rts
.endif
.endp
```

## NGA0168

`.else` is the branch taken when no condition above it holds, so it comes last.
Move the `.elsif` above the `.else`.

```asm
LEVEL = 1

.proc entry
.if LEVEL == 1
        lda #1
.else
        lda #3
.elsif LEVEL == 2
        lda #2
.endif
        rts
.endp
```

## NGA0169

A conditional branch holds instructions and data, for the same reason a macro
body does.

A branch takes instructions, `.byte`, `.word`, a payload, `.res`, macro uses and
`.with`. What defines a Symbol is refused: a Constant, a Temporary, a Section, a
Proc, a macro, a Namespace, a Charset. So are `.transition`, `.assert`,
`.export` and `.off`. No Symbol is conditional, since two branches would want
one name and only one of them is there. A block also opens and closes within one
branch, so an `.endp` or `.endm` inside an open `.if` is refused too.

```asm
NTSC = 1

.proc entry
.if NTSC
frames = 60
.else
frames = 50
.endif
        lda #frames
        rts
.endp
```

A name with two values is one Constant with a conditional value, written at the
top level: `frames = NTSC ? 60 : 50`. See
[Not the other way](../10-conditionals.md#not-the-other-way).

## NGA0170

A plain label defines a Symbol, and no Symbol is conditional. In a branch a
label is local, written `@name`, and each branch may define its own. A
`.section` has no local labels, so a branch in one holds no label at all.

```asm
FAST = 1

.proc entry
        ldx #8
.if FAST
loop    dex
        bne loop
.endif
        rts
.endp
```

Write `@loop` and `bne @loop`.

## NGA0171

A pack, `name...`, takes every argument the names before it do not, so it is the
last name of a `.macro` header or of a `.case`. Put the fixed parameters first.

```asm
.macro store values..., at
        lda #0
        sta at
.endm
```

Here it is `.macro store at, values...`.

## NGA0172

`.match` counts the elements of a pack, and a pack is a parameter of a macro.
Outside a macro body there is none to count. To choose statements by a
Constant, use `.if`.

```asm
.proc entry
.match args
.case
.endmatch
        rts
.endp
```

The `.case` and `.endmatch` of the refused `.match` are reported too, as
[`NGA0173`](#nga0173) and [`NGA0174`](#nga0174). In a body, a `.match` on a
parameter that is not a pack is [`NGA2326`](#nga2326).

## NGA0173

A `.case` with no `.match` open. The `.match` is missing, was refused, or was
closed by an `.endmatch` above this `.case`.

```asm
.macro first values...
.case head, rest...
        .byte head
.endm
```

Open the block with `.match` and the pack's name above the first `.case`, and
close it with `.endmatch` after the last.

## NGA0174

An `.endmatch` with no `.match` open. Usually there is one too many. It is also
raised where the innermost open block is an `.if` that has not reached its
`.endif`.

```asm
.macro pair values...
.match values
.case a, b
        .byte a, b
.case
.endmatch
.endmatch
.endm
```

## NGA0175

A `.match` without `.endmatch`. The finding points at the `.match`. As with an
`.if`, the block closes before the macro does, so the `.endm` below it is
refused, [`NGA0169`](#nga0169), and the macro is reported unclosed,
[`NGA0156`](#nga0156).

```asm
.macro push values...
.match values
.case first, rest...
        lda #first
        pha
        push rest...
.case
.endm
```

Add `.endmatch` after the last `.case`.

## NGA0176

Every statement of a `.match` belongs to one of its `.case`s, and a statement
between `.match` and the first `.case` belongs to none.

```asm
.macro push values...
.match values
        clc
.case first, rest...
        lda #first
        pha
        push rest...
.case
.endmatch
.endm
```

Move it above the `.match` where every case needs it, or into the cases that do.

## NGA0177

A spread is the pack's name with `...` touching it: `values...`. Dots after a
space, or after anything but a bare name, as in `(values)...`, are refused.

```asm
.macro table values...
        .byte values ...
.endm
```

Remove the space. Spreading a name that is not a pack is
[`NGA2325`](#nga2325).

## NGA0178

A `.match` with no `.case` has nothing that could fit, so every use of the macro
would be an error. Add the cases. A `.case` with no names fits the empty pack.

```asm
.macro push values...
.match values
.endmatch
.endm
```

When a `.match` has cases and none fits the arguments of a use, the error is
[`NGA2328`](#nga2328), at that use.

## NGA0179

`temporary` lets a Section share its address with another Temporary that is
never active at the same time. A Root is read by the hardware at any time, so
its bytes would never be free to share. A Temporary holds reservations and no
bytes, so `readonly` would say nothing about it. `in` is refused as well,
whether it names a Pane or a Region.

```asm
.section temporary, root
buffer  .res 64
.ends
```

Drop one of the two. A reservation that must keep its address to itself is a
`.res` in a `.section` without `temporary`.

## NGA0180

Raised for `temporary` among the attributes of a `.proc`. A Temporary holds
reservations only, and its bytes belong to other Temporaries while it is not
active.

```asm
.proc entry, temporary
        rts
.endp
```

For variables the Proc needs only while it runs, declare a `.ztemp` or a
`.temp` inside it; see [`.ztemp` and `.temp`](../16-assembler.md#ztemp-and-temp).
To reuse the memory of code that runs once, put that code in a Module that only
the Phases running it need.

## NGA0181

Raised for a `.source` line that is not `.source "PATH", LINE`: a path without
quotes, a path with a character set prefix, or no line number after it. The
path names a file, so it is a bare string that nothing translates.

```asm
.source tiles.txt, 3
.proc entry
        rts
.endp
```

Quote the path: `.source "tiles.txt", 3`. See
[`.source`](../16-assembler.md#source).

## NGA0182

Raised for a `.source` whose line number is 0. Lines count from 1, as an editor
counts them. A program that writes `.source` lines and counts from zero has to
add one.

```asm
.source "tiles.txt", 0
.proc entry
        rts
.endp
```

## NGA0183

Raised where `.hex`, `.binary` or `.base64` is followed by something other than
a string literal. The quotes mark where the payload ends, so a comment may
follow it on the line.

```asm
.section
sprite  .hex ad12f4
.ends
```

Write `.hex "ad12f4"`. Space and tab inside the quotes are ignored, so the
bytes may be grouped: `"ad 12 f4"`. See [Data](../16-assembler.md#data).

## NGA0184

Raised once for each character of a payload that its directive's alphabet
lacks: `0-9 A-F a-f` for `.hex`, `0 1` for `.binary`, and `A-Z a-z 0-9 + / =`
for `.base64`. Space and tab are the only other characters allowed, and they
are skipped. The usual causes are a `$` in front of hex digits and commas
between bytes. A `;` inside the quotes is such a character too, not a comment.

```asm
.section
sprite  .hex "$ad12f4"
.ends
```

## NGA0185

Raised for a `=` anywhere in a `.base64` payload except its last one or two
characters. `=` pads the last group of four characters. One in the middle
usually means that two encoded strings were joined into one literal.

```asm
.section
sprite  .base64 "QUE=QUJD"
.ends
```

Encode the bytes again as one string, or give each string its own `.base64`
line. Two lines in one Section emit their bytes one after the other.

## NGA0186

Raised where the characters of a payload do not make whole bytes. `.hex` takes
two characters to a byte and `.binary` eight. `.base64` takes groups of four,
each three bytes, with `=` filling out the last group. The count leaves out
spaces and tabs.

```asm
.section
sprite  .binary "0001100 00111100"
.ends
```

The first byte here has seven digits. A digit dropped or typed twice is the
usual cause. Not raised where a character was refused already, since that
character is why the count is short; see [`NGA0184`](#nga0184).

## NGA0187

Raised for a character set prefix on the literal of `.hex`, `.binary` or
`.base64`. A prefix asks for text to be translated into a Charset, and a
payload is bytes that were never characters.

```asm
.section
sprite  .hex atascii"ad12f4"
.ends
```

Remove the prefix. To emit text in a Charset, write the prefixed literal after
`.byte`; see [Character sets](../11-character-sets.md).

## NGA0188

Raised inside a `.namespace` block that stands in a `.proc`, once for each line
that belongs to the Proc itself: an instruction, data, a label or a `.declare`.
Such a block holds what stands beside the Proc, which is `.section` blocks,
`.ztemp` and `.temp` reservations, and Constants.

```asm
.proc entry
.namespace state
count   .ztemp 1
        lda #0
.endns
        sta state.count
        rts
.endp
```

Close the block with `.endns` before the code. The Proc's labels need no
Namespace, because the Proc is a scope of its own: a label `loop` inside
`entry` is `entry.loop` from outside.

## NGA0189

Raised where a `.declare` reserves no byte, and a `.ztemp` or `.temp` stands on
the line below it. One that keeps every byte in registers, such as `.declare arg
a`, reserves none, and nor does one that names another Proc's result, such as
`.declare ret low.result`. A `.declare` names the reservation below it, and
these take none, so a reservation there is taken for a mistake.

```asm
.proc plot
        .declare arg a
column  .ztemp 1
        sta column
        rts
.endp

.proc entry
        lda #3
        jsr plot
        rts
.endp
```

Where the `.ztemp` is the Proc's own scratch, move it above the `.declare`.
Where it was meant to carry the argument, leave out the place, and the argument
is passed in the reservation. See [`.declare`](../16-assembler.md#declare).

## NGA0190

Raised where two `.declare arg` lines of one Proc put a byte in the same
register. A register carries one argument into the call. The result may use a
register an argument came in.

```asm
.proc print
        .declare arg ax
        .declare arg x
        rts
.endp

.proc entry
        jsr print
        rts
.endp
```

Move one of the arguments to a register that is still free, or to a
reservation.

## NGA0191

Raised for a place that names one register for both bytes: `aa`, `xx` or `yy`.
A place is a letter per byte, low byte first, and a register holds one byte.

```asm
.proc plot
        .declare arg xx
        rts
.endp

.proc entry
        jsr plot
        rts
.endp
```

Use two registers, such as `ax`, or a register and the reservation, such as
`am`.

## NGA0192

Raised for `readonly` on a `zeropage` Section. `readonly` lets the solver put
the Section in a `rom` Region. The zero page is the scarcest memory the machine
has, and the tool does not spend it on bytes nothing writes.

```asm
.section zeropage, readonly
mask    .byte $0F
.ends
```

Drop `zeropage` to let the bytes go to ROM where the Target has some, or drop
`readonly` to keep them on the zero page. See [Cartridges](../14-cartridges.md)
for what `readonly` is for.

## NGA0193

Raised for `readonly` on a `.proc`. A Proc that no statement writes into is
read-only without the word, and can go in a `rom` Region where the Target has
one. A Proc that some statement writes into, such as one whose operand another
routine patches, stays in RAM. The word changes neither, so the tool drops it.

```asm
.proc entry, readonly
        rts
.endp
```

Remove the word.

## NGA0194

Raised for `foreign` beside `temporary` or `readonly`. `foreign` marks bytes
that another master of the address space reads while the program runs, such as
a display buffer. A Temporary's bytes belong to other Sections between its
uses, and `readonly` asks for ROM, where nothing writes.

```asm
.section absolute align 4, root, foreign, readonly
screen  .res 8160
.ends
```

Drop the other word.

## NGA0195

Raised for `foreign` on a `.section` that is not `root`. The other master
reaches the Section through an address the program hands to the hardware, and
no Reference follows that. Without `root`, Prune finds nothing that reaches the
Section and drops it.

```asm
.section absolute align 4, foreign
screen  .res 8160
.ends
```

Add `root`. See [Roots](../13-roots.md).

## NGA0196

Raised for `foreign` on a `.proc`. `foreign` says the CPU does not read the
bytes, and the CPU is what runs a Proc.

```asm
.proc entry
        jsr draw
        rts
.endp

.proc draw, foreign
        rts
.endp
```

Remove the word. Bytes that another processor runs are data to the CPU, and
belong in a `.section`.

## NGA0197

Raised for a word other than `fast` after the comma of
`.transition NAME, ...`. `fast` asks storage to start everything the edge loads
at the beginning of a unit, so that a driver which reaches an offset by reading
up to it never has to. It has an effect only where the driver works that way.
Under `optimize speed` every edge is treated as `fast`.

```ngp
modules { "main.asm" "game.asm" }

phase title { needs main  then game }
phase game  { needs game  entry play }
entry title
```

```asm
.proc entry
        .transition game, quick
.endp
```

```asm game.asm
.proc play
        jmp play
.endp
```

Write `fast`, or nothing. The statement is dropped with the finding, which is
why the edge is then also reported as one nothing takes.

## NGA0198

Raised for `movable` among the attributes of a `.section` or a `.proc`. The
word once let a Section stand at a different address in each Phase. A Section
now has one address in every Phase it is present in.

```asm
.section absolute, movable
sprites .res 64
.ends
```

Delete the word. Where two Phases cannot hold the bytes at one address, give
each Phase a Section of its own, in a Module that only that Phase needs.

## NGA0199

A Label is a name at column one and nothing more, and the colon other
assemblers write after one is refused. The Label is defined all the same, and
what follows the colon is read as the statement, so the colon costs this one
finding.

```asm
.proc entry
        ldx #8
loop:   dex
        bne loop
        rts
.endp
```

Drop the colon.

## NGA0200

Raised where `.declare arg` names a byte of another Proc, such as `.declare arg
low.result`. An argument is a byte the caller writes for this Proc, in a
reservation of its own or in a register. Only a result may be another Proc's,
since the caller reads it after the call wherever it was left.

```asm
.proc low
        .declare ret u16
result  .ztemp 2
        rts
.endp

.proc show
        .declare arg low.result
        rts
.endp

.proc entry
        jsr low
        jsr show
        rts
.endp
```

Declare the argument over a `.ztemp` of the Proc, and have the caller copy the
value there. See [`.declare`](../16-assembler.md#declare).

## NGA1101

Raised where a list of Modules in the Project names something that is neither a
Module nor a group: `needs`, `without`, `resident`, `load`, a `group`, or the
Module part of a `transform` entry. A Module answers to the stem of its file,
or to the name after `as`, and to nothing else. Once `as` renames a Module, its
stem names nothing.

```ngp
modules { "main.asm" as game }

resident { main }
```

```asm
.proc entry
        rts
.endp
```

Use the name the `modules` block gives; see
[`modules`](../15-project-file.md#modules).

## NGA1102

Raised where two entries of `modules` come to one Module name: two files with
one stem in different directories, or a file and a generator call whose file
has that stem. The note, [`NGA1173`](#nga1173), points at the first entry. A
Project file that two `include` lines reach is read once, so its `modules`
block does not raise this.

```ngp
modules {
  "main.asm"
  "levels/main.asm"
}
```

```asm
.proc entry
        rts
.endp
```

Give one of them another name with `as`: `"levels/main.asm" as level`.

## NGA1103

Raised when the file given to the run does not end in `.ngp`. A run takes a
Project and nothing else, because what a Module is called, which Phases need it
and what the Target is are written in the Project and in no source file.

Write a Project that names the source file, which can be one line,
`modules { "main.asm" }`, and run `nga main.ngp`. See
[The Project and Modules](../01-the-project-and-modules.md).

## NGA1104

Follows [`NGA1103`](#nga1103) and says how a source file takes part in a build:
a Project names it in its `modules` block.

## NGA1105

Raised inside `constants` where an entry is not a name, `=` and one number
literal, such as `8`, `$F0` or `%1010`, with an optional `-` before it as its
sign, as in `-1`. An expression is refused, and so is the name of another
constant: nothing in the block marks where an expression would end, so a value
is one literal. A `-` before a name is refused as well. An entry that breaks is
reported once, and what is left of it is read past.

```ngp
constants {
  LEVELS = 8
  LIVES  = LEVELS
}

modules { "main.asm" }
```

```asm
.proc entry
        lda #LIVES
        rts
.endp
```

Write the number, or do the arithmetic in the source that reads the constant.
See [`constants`](../15-project-file.md#constants).

## NGA1106

A constant of the `constants` block is set once in the whole Project, counting
every file an `include` reaches. A second value would depend on which file is
read first.

```ngp
constants {
  LEVELS = 8
  LEVELS = 9
}

modules { "main.asm" }
```

```asm
.proc entry
        lda #LEVELS
        rts
.endp
```

To give a variant a value that a Project can change, keep the constant out of
the variant and set it in the Project.

## NGA1107

Follows [`NGA1106`](#nga1106) and points at the first place the constant is
set.

## NGA1108

Raised for a word after `container` in the Project, or after `containers` in a
`target` block, that names no Container the tool writes. The message lists the
words there are.

```ngp
container cart

modules { "main.asm" }
```

```asm
.proc entry
        rts
.endp
```

A cartridge image is `car`, followed by the board's name in quotes, as in
`container car "xegs128"`; see [Cartridges](../14-cartridges.md).

## NGA1109

Raised for a second `container` line in the Project, counting every file an
`include` reaches, even where both name the same Container. What the build
writes cannot depend on which file is read first. The note
[`NGA1140`](#nga1140) points at the first one.

```ngp
include "machine.ngp"
container xex

modules { "main.asm" }
```

```ngp machine.ngp
include "atari/800xl.ngp"
container xex
```

```asm
.proc entry
        rts
.endp
```

Keep one, in the Project rather than in a file it includes.

## NGA1110

Raised for a word at the top level of a Project file that starts no block the
tool knows. It is usually a misspelling. The blocks are listed in
[the Project file](../15-project-file.md).

```ngp
module { "main.asm" }
```

```asm
.proc entry
        rts
.endp
```

The block here is `modules`. A word with no `{` after it is reported once, here,
and the tool reads on from the word after it.

## NGA1111

Raised when the file ends inside a block. The finding points at the block's
first line, and the `}` is missing somewhere after it.

```ngp
modules { "main.asm" }

target {
  region $0080 .. $00FF ram
  region $2000 .. $BFFF ram
```

```asm
.proc entry
        rts
.endp
```

A block left open before another block is reported differently. The `}` of the
next block closes the open one, and the next block's name is refused as an
entry of it.

## NGA1112

Raised at the top level of a Project file for something that starts no block: a
quoted path outside `modules`, a stray `}`, or punctuation such as `:` after a
word. An `include` whose path is not quoted is [`NGA1248`](#nga1248).

```ngp
"main.asm"
```

```asm
.proc entry
        rts
.endp
```

A path stands inside a `modules` block: `modules { "main.asm" }`.

## NGA1113

Raised where the name of a block is not followed by `{`. A block has its body in
braces even when it holds one entry: `resident { hud }`. `include`, `entry`,
`container` and `optimize` are lines of their own and take none.

```ngp
modules "main.asm"
```

```asm
.proc entry
        rts
.endp
```

What stood where the brace should be is then read at the top level, which adds
[`NGA1112`](#nga1112).

## NGA1114

Raised inside `modules` for an entry that is neither a quoted path nor a
generator call such as `binary( "font.bin" )`. The usual causes are a path
without quotes and a comma between entries: unlike `needs` and `resident`,
`modules` takes no commas.

```ngp
modules { main.asm }
```

```asm
.proc entry
        rts
.endp
```

## NGA1115

Raised for `as` in a `modules` entry with no name after it. The name is a bare
identifier, not a quoted string. A quoted one is read as the path of the next
entry instead, which adds [`NGA1122`](#nga1122).

```ngp
modules { "main.asm" as "game" }
```

```asm
.proc entry
        rts
.endp
```

Write `"main.asm" as game`.

## NGA1116

Raised for a character set prefix on a quoted string of the Project file: a
Module path, an `include`, a generator's argument, the name after `cpu`.
Nothing in a Project file is translated for the target. A path may hold
non-ASCII characters as they are.

```ngp
modules { atascii"main.asm" }
```

```asm
.proc entry
        rts
.endp
```

Remove the prefix.

## NGA1117

Raised inside `diagnostics` where an entry does not start with `deny`, `allow`
or `off`. `deny` makes a finding an error, `allow` makes it a warning, and `off`
silences it.

```ngp
modules { "main.asm" }

diagnostics {
  error NGA4501
}
```

```asm
.proc entry
        rts
.endp
```

See [`diagnostics`](../15-project-file.md#diagnostics).

## NGA1118

Raised where the `diagnostics` block, `--deny`, `--allow` or `--off` names an
identifier the tool does not have. It is refused rather than skipped, since a
line that silenced nothing would look as if it worked. In source, `.off` with
such an identifier is [`NGA0143`](#nga0143).

```ngp
modules { "main.asm" }

diagnostics {
  off NGA113
}
```

```asm
.proc entry
        rts
.endp
```

An identifier is `NGA` and four digits, as the tables above list them.

## NGA1119

Raised where one identifier is given a severity twice in the Project, counting
every file an `include` reaches, even where both say the same. Which one won
would depend on the order the files are read. The note [`NGA1120`](#nga1120)
points at the first. Given twice on the command line, by `--deny`, `--allow`
or `--off`, it is raised with no position, for the same reason.

```ngp
modules { "main.asm" }

diagnostics {
  deny  NGA4501
  allow NGA4501
}
```

```asm
.proc entry
        rts
.endp
```

Keep one. To change a severity that an included file sets, use `--deny`,
`--allow` or `--off` on the command line, which win over the Project.

## NGA1120

Follows [`NGA1119`](#nga1119) and points at the first place the identifier's
severity is set.

## NGA1121

Raised for an `include` that reaches a file still being read, directly or
through other files. The tool reads each file once, but a file that is still
being read has not been read yet, so the cycle would not end.

```ngp
include "machine.ngp"

modules { "main.asm" }
```

```ngp machine.ngp
include "main.ngp"

target { region $2000 .. $BFFF ram }
```

```asm
.proc entry
        rts
.endp
```

Remove the `include` that leads back.

## NGA1122

Raised for a file the tool cannot read: a Module in `modules`, a file an
`include` names, the file a generator works from, or the Project file given to
the run. A path is relative to the file that holds it, not to the directory the
tool runs in. Where no file is there, the tool looks in
[the library](../15-project-file.md#the-library). The message names the place
beside the file.

```ngp
modules { "main.asm" "sprite.asm" }
```

```asm
.proc entry
        rts
.endp
```

## NGA1123

Raised when the Project, with every file it includes, names no Module. The
`modules` block is missing or empty. The Modules the tool adds for what the
Project declares, such as its `constants`, its named Regions or the loader of a
`.xex`, do not count.

```ngp
target {
  region $0080 .. $00FF ram
  region $2000 .. $BFFF ram
}
```

Name the source files in a `modules` block; see
[`modules`](../15-project-file.md#modules).

## NGA1125

A Phase is named before its brace, with a bare word: `phase game { ... }`, not
`phase "game"`. The name is what `then` and the document's `entry` refer to.
A body without a name is skipped whole, so nothing inside it is checked until
the name is there.

```ngp
modules { "main.asm" }

phase { needs main }
```

```asm
.proc entry
        rts
.endp
```

## NGA1126

A `phase` block holds `needs`, `then`, `without`, `entry` and `final`, and
nothing else. The tool reports the first word it does not know and skips to the
next one it does, so what stood between is not read.

```ngp
modules { "main.asm" }

phase game { needs main  init start }
entry game
```

```asm
.proc entry
        rts
.endp
```

`init` is a word of the `load` block. A Phase names the Label it starts at with
`entry`. A missing comma ends here too: in `needs main music`, the name `music`
is read as the next entry of the block.

## NGA1127

Raised where a word that takes a name is followed by something else: a list
that ends in a comma, an `entry` or a `then` with nothing after it, or a quoted
name. `needs` and `then` name Modules and Phases as bare words, `main` and not
`"main.asm"`. In a `target` block, `register`, `holder`, `units` and `window`
take the name of what they declare first.

```ngp
modules { "main.asm" }

phase game { needs main, }
entry game
```

```asm
.proc entry
        rts
.endp
```

`cpu` takes the processor's name in quotes, and `cpu 6502` without them is
[`NGA1248`](#nga1248).

## NGA1128

`resident` names Modules and groups, and a Module's name is the stem of its
file, or the name after `as` in `modules`. A quoted path is not a name.

```ngp
modules { "main.asm" }
resident { "main.asm" }
```

```asm
.proc entry
        rts
.endp
```

Write `resident { main }`.

## NGA1129

Names in `resident`, `group` and `panes` are separated by commas. The tool takes
both names anyway, so nothing else is reported.

```ngp
modules { "main.asm"  "music.asm" }
resident { main music }
```

```asm
.proc entry
        rts
.endp
```

```asm music.asm
.proc play
        rts
.endp
```

In the `needs` of a `phase` block, the second name is read as the next entry of
the block instead, and the finding is [`NGA1126`](#nga1126).

## NGA1130

A `then`, or the `entry` of the document, names a Phase that no `phase` block
declares. The name may be misspelt, or the Phase renamed. Declare the Phase or
correct the name.

```ngp
modules { "main.asm" }

phase title { needs main  then game }
entry title
```

```asm
.proc entry
        rts
.endp
```

A name in `needs` or `resident` that names no Module and no group is
[`NGA1101`](#nga1101) instead.

## NGA1131

The Phase the program starts in is named once in the whole Project, counting
every file an `include` reaches. A second `entry` is refused even when it names
the same Phase, because otherwise the start would depend on which file is read
first.

```ngp
modules { "main.asm" }

phase game { needs main }
entry game
entry game
```

```asm
.proc entry
        rts
.endp
```

Keep the `entry` in the Project file and out of the files it includes.

## NGA1132

Follows [`NGA1131`](#nga1131) and points at the first `entry`.

## NGA1133

A Project with `phase` blocks says which Phase the program starts in, with
`entry NAME` at the top level of the document. An `entry` inside a `phase`
block does not count: it names the Label that Phase starts at.

```ngp
modules { "main.asm" }

phase game { needs main }
```

```asm
.proc entry
        rts
.endp
```

Add `entry game`. A Project with no `phase` block has one Phase and needs no
`entry`; see [the Project file](../15-project-file.md#a-project-with-no-phase-block).

## NGA1134

In a Project with `phase` blocks, a Module is in memory in the Phases whose
`needs` name it, directly or through a group, and in every Phase if `resident`
names it. Nothing names this one. The build goes on without it.

```ngp
modules { "main.asm"  "tools.asm" }

phase game { needs main }
entry game
```

```asm
.proc entry
        rts
.endp
```

```asm tools.asm
.proc dump
        rts
.endp
```

Name the Module in the `needs` of the Phases that use it, or in `resident`, or
take it out of `modules`. A Module that only the `load` block needs is not
reported.

## NGA1135

A `target` block holds `region`, `register`, `holder`, `units`, `window`,
`containers` and `cpu`. The tool reports the first word it does not know and
skips to the next one it does.

```ngp
modules { "main.asm" }

target { container xex }
```

```asm
.proc entry
        rts
.endp
```

`container` stands at the top level of the Project and names what the tool
builds. `containers`, in a `target` block, says what the machine takes. This
finding also follows an entry that broke in the middle, whose remaining words
are read as entries of their own, as in [`NGA1139`](#nga1139).

## NGA1136

A word that takes a value has none after it: a `register` without its address,
`units NAME` without its count, a `size` with nothing after it, or a `..` that
ends before the second address. Write the number.

```ngp
modules { "main.asm" }

target { register COLBK }
```

```asm
.proc entry
        rts
.endp
```

## NGA1137

The Project has no Symbols, so a name means nothing in a value of the Project
file. That includes a Constant of its own `constants` block and a `register`
declared beside it. Constants are for the Modules, which read them in source.
Write the number out.

```ngp
constants { GTIA = $D000 }

modules { "main.asm" }

target { register COLBK GTIA + $1A }
```

```asm
.proc entry
        rts
.endp
```

## NGA1138

The value holds no name and still does not come to a number. Usually it is
quoted. The Project file quotes paths and the name of a processor, but a number
is written bare: `$D01A`, not `"$D01A"`. A division by zero ends here too.

```ngp
modules { "main.asm" }

target { register COLBK "$D01A" }
```

```asm
.proc entry
        rts
.endp
```

## NGA1139

A `region`, a `window` and a unit set's `linear` take a range: two values with
`..` between them, both ends inclusive. A `-` between the two makes one value
of them, their difference, and the `..` is then missing.

```ngp
modules { "main.asm" }

target { region $2000-$9FFF ram }
```

```asm
.proc entry
        rts
.endp
```

The words after the broken range are read as entries of the block, which is the
[`NGA1135`](#nga1135) after it. Write `region $2000 .. $9FFF ram`.

## NGA1140

Follows [`NGA1109`](#nga1109) and points at the first `container`.

## NGA1141

The Project's `container` names a Container that the Target's `containers`
entry leaves out. A machine variant says there what its machine can load: the
library's `atari/130xe.ngp` takes `xex` alone, and `atari/800xl.ngp` takes `xex`
or `atr`.

```ngp
target { containers xex }

container atr

modules { "main.asm" }
```

```asm
.proc entry
        rts
.endp
```

Name a Container the machine takes, or include the variant of another machine.
Not raised where the Project names no `container`, nor where no Target says
what it takes.

## NGA1142

Follows [`NGA1141`](#nga1141) and points at the `containers` entry of the
Target, usually in the machine variant the Project includes.

## NGA1143

`fast` and `small` are not among the words. A Project without `optimize` gets
`fit`. What each word prefers is in
[the Project file](../15-project-file.md#optimize).

```ngp
modules { "main.asm" }

optimize fast
```

```asm
.proc entry
        rts
.endp
```

## NGA1144

`optimize` is said once in the whole Project, counting every file an `include`
reaches. A second one is refused even when it says the same word.

```ngp
modules { "main.asm" }

optimize speed
optimize size
```

```asm
.proc entry
        rts
.endp
```

Keep it in the Project file and out of the files it includes.

## NGA1145

Follows [`NGA1144`](#nga1144) and points at the first `optimize`.

## NGA1146

The words are lowercase, so `"65SC02"` is refused as well. A 65C02 has every
instruction of the 65SC02, so a machine with one is written `"65sc02"`.
Neither word allows the Rockwell bit instructions, `wai` or `stp`; see
[`cpu`](../15-project-file.md#cpu).

```ngp
modules { "main.asm" }

target { cpu "65c02" }
```

```asm
.proc entry
        rts
.endp
```

## NGA1147

`entry` inside a `phase` block names the Label the Phase starts at, and a Phase
has one. Two `phase` blocks of one name sum, so a second `entry` is refused even
in another block, or in another file.

```ngp
modules { "main.asm" }

phase game { needs main  entry start }
phase game { entry restart }
entry game
```

```asm
.proc entry
        rts
.endp
```

Keep one of them.

## NGA1148

Follows [`NGA1147`](#nga1147) and points at the first `entry` of that Phase.

## NGA1149

`cpu` is said once in the whole Project, and a machine variant says it: every
variant in the library does. A Project that includes a variant cannot change
its processor.

```ngp machine.ngp
target { cpu "6502" }
```

```ngp
include "machine.ngp"

target { cpu "65sc02" }

modules { "main.asm" }
```

```asm
.proc entry
        rts
.endp
```

For another processor, write a variant of your own and include that one.

## NGA1150

The word after `transform` is the format the listed Sections are encoded in,
such as `zx0` or `rle`. It is also the name of the decoder a Module declares
with `.transform`. Write it before the brace: `transform zx0 { main.tiles }`. A
body without a name is skipped whole.

```ngp
modules { "main.asm" }

transform { main.tiles }
```

```asm
.proc entry
        rts
.endp
```

## NGA1151

A `transform` applies to Sections, not to Modules. A Section has no name of its
own, so an entry names it by one of its Labels, after the Module's name and a
dot: `main.tiles`, or `main.space.tiles` for a Label in a Namespace. Any Label
of the Section will do, exported or not.

```ngp
modules { "main.asm" }

transform zx0 { main }
```

```asm
.proc entry
        rts
.endp
```

The `found` part of the message is what stood after the Module's name, here the
closing brace.

## NGA1152

Follows [`NGA1149`](#nga1149) and points at the first `cpu`, usually in the
machine variant the Project includes.

## NGA1155

A `region` ends with its property, and here the range is followed by something
else, usually the closing brace. The property is `ram`, `rom`, `register` or
`reserved`; see [`region`](../15-project-file.md#region). The finding points at
`region`, since what follows the range is often on the next line.

```ngp
modules { "main.asm" }

target { region $2000 .. $9FFF }
```

```asm
.proc entry
        rts
.endp
```

## NGA1156

`readonly` is an attribute of a `.section`. The Region where such Sections go on
a cartridge is `rom`. A Region's name goes before its range, as in `region io
$D000 .. $D7FF register`, so a name written after the range is read as the
property and is reported here as well.

```ngp
modules { "main.asm" }

target { region cart $A000 .. $BFFF readonly }
```

```asm
.proc entry
        rts
.endp
```

## NGA1157

Both ends of a Region are inclusive: the end is the last address in it. A Region
that reaches the top of memory ends at `$FFFF`, not at `$10000`. The start must
not be above the end either.

```ngp
modules { "main.asm" }

target { region $E000 .. $10000 ram }
```

```asm
.proc entry
        rts
.endp
```

## NGA1158

A named Region gives every Module a Symbol, so one name stands for one Region.
This includes the Regions that `register` declares and those of an included
variant. A second declaration is refused even when it is the same.

```ngp
modules { "main.asm" }

target {
  region ram $0080 .. $00FF ram
  region ram $2000 .. $9FFF ram
}
```

```asm
.proc entry
        rts
.endp
```

Give the two Regions two names, or none. A Project that narrows a Region of its
variant leaves its own unnamed: `atari/130xe.ngp` names `dos`, so a Project
that includes it and reserves more for its DOS writes
`region $0700 .. $2FFF reserved`. A name a unit set or a Window already has is
[`NGA1180`](#nga1180).

## NGA1159

Follows [`NGA1158`](#nga1158) and points at the first declaration of the name,
which may be in an included variant.

## NGA1160

Every byte of a `register` lies in the 64 KB address space. The address is
above `$FFFF` or below zero, or it is `$FFFF` with a width of two, whose second
byte would be past the end.

```ngp
modules { "main.asm" }

target { register COLBK $D01A0 }
```

```asm
.proc entry
        rts
.endp
```

## NGA1161

A `register` entry declares one byte, or two with `, 2` after the address. A
wider block of hardware is a Region with the property `register`:
`region io $D000 .. $D7FF register`.

```ngp
modules { "main.asm" }

target { register AUDF1 $D200, 4 }
```

```asm
.proc entry
        rts
.endp
```

## NGA1163

Follows a finding about a Region and points at the line of the `target` block
that declares it: a pin into a `register` Region ([`NGA5217`](#nga5217)), a
`reserved` one ([`NGA5218`](#nga5218)) or a `rom` one ([`NGA5233`](#nga5233)),
a `foreign` Section with bytes over one ([`NGA5253`](#nga5253)), a Module that
defines a name the Project declares ([`NGA2272`](#nga2272)), or a cartridge
whose ROM does not match the machine's `rom` Region ([`NGA1219`](#nga1219)).
It names what it points at: a Region, a register, a unit set, a Window or a
Pane.

## NGA1164

A `storage` block holds `units` and `size`, and nothing else:
`storage { units 720  size 128 }` for units known by their numbers, and
`storage { units extension }` for the Banks of a unit set. The tool reports the
first word it does not know and skips to the next one it does.

```ngp
modules { "main.asm" }

storage { count 720  size 128 }
```

```asm
.proc entry
        rts
.endp
```

## NGA1165

`units` with a number declares units known by their numbers, such as the
sectors of a diskette, and nothing else says how many bytes each one holds. Add
`size`, in this block or in another `storage` block of the Project:
`storage { units 720  size 128 }`.

```ngp
modules { "main.asm" }

storage { units 720 }
```

```asm
.proc entry
        rts
.endp
```

Units that are the Banks of a unit set, `units extension`, take their size from
the Window that shows the set and need no `size`.

## NGA1166

`size` is said once in the whole Project, counting every `storage` block and
every file an `include` reaches. A second one is refused even when it gives the
same number.

```ngp
modules { "main.asm" }

storage { units 720  size 128 }
storage { size 256 }
```

```asm
.proc entry
        rts
.endp
```

A Project that includes a storage file of the library, such as
`atari/disk.ngp`, and has a `storage` block of its own meets this. Use one or
the other.

## NGA1167

Follows [`NGA1166`](#nga1166) and points at the first `size`.

## NGA1168

Storage has one kind of unit: the Banks of a unit set, `units extension`, or
units known by their numbers, `units 720`. `units` is said once in the whole
Project, in either form, and a second one is refused even when it says the same.

```ngp
modules { "main.asm" }

storage { units 720  size 128 }
storage { units 1040 }
```

```asm
.proc entry
        rts
.endp
```

As with [`NGA1166`](#nga1166), a storage file of the library included beside a
`storage` block of the Project's own meets this. Use one or the other.

## NGA1169

Follows [`NGA1168`](#nga1168) and points at the first `units`.

## NGA1170

Raised for `units` in a `target` block, which declares a unit set, and in a
`storage` block, which counts units by number. The count is at most 65536,
because a unit's number is written in no more than two bytes: in a Frame, in
the table of a `.transition` and in the driver's cell.

```ngp
storage { units 131072  size 512 }

modules { "main.asm" }
```

```asm
.proc entry
        rts
.endp
```

Declare the units the program can use rather than every unit the medium has.
For storage the count is a ceiling, not a reservation: the image holds only
what its Payloads come to.

## NGA1171

`size` in a `storage` block is what one unit holds, such as one sector. It is
between 1 and 65536 bytes. The storage is `units × size` bytes, so `size` is
not the size of the whole medium.

```ngp
storage { units 720  size 720 * 128 }

modules { "main.asm" }
```

```asm
.proc entry
        rts
.endp
```

A diskette of 720 sectors of 128 bytes is `units 720  size 128`.

## NGA1172

A named `modules` block is a group, and a group's name stands in the same
lists as a Module's: `needs`, `resident` and `load`. One name for both would
mean two things there. It usually happens when a named block holds a file of
the same stem.

```ngp
modules { "main.asm" }
modules sfx { "sfx.asm" }
```

```asm
.proc entry
        rts
.endp
```

```asm sfx.asm
.section
beep    .byte 1, 2, 3
.ends
```

Rename the group, or give the Module another name with `as`.

## NGA1173

Follows [`NGA1172`](#nga1172) or [`NGA1102`](#nga1102) and points at the entry
that gives the Module its name.

## NGA1174

A group may name other groups to any depth, but it may not reach itself through
them. The tool expands a group where a `needs`, `resident` or `load` list names
it, and reports the cycle at the name that closes it.

```ngp
modules { "main.asm" }
group game { main, hud }
group hud  { game }

phase play { needs game }
entry play
```

```asm
.proc entry
        rts
.endp
```

Take out one of the names that close the cycle, here `game` in `hud`.

## NGA1175

A `needs`, `resident` or `load` list names a group that holds no Module: a
named `modules` block with nothing in it, or a `group` whose members are all
empty groups. A group that no list names is not reported.

```ngp
modules { "main.asm" }
modules sfx {
; "sfx/beep.asm"
}

phase play { needs main, sfx }
entry play
```

```asm
.proc entry
        rts
.endp
```

Fill the group, or take its name out of the list.

## NGA1176

A `transform` entry names a Section as `module.label`, and `without` names one
ROM Module. Neither takes a group.

```ngp
modules { "main.asm" }
modules art { "tiles.asm" }

transform rle { art.tiles }
```

```asm
.proc entry
        rts
.endp
```

```asm tiles.asm
.section
tiles   .byte 1, 2, 3
.ends
```

Name the Module that holds the Section. It is named after the file's stem, so
here the entry is `tiles.tiles`.

## NGA1177

A `group` block lists the names of Modules and of other groups, separated by
commas. It takes no path. A file joins a group through a named `modules` block,
which declares the Module and the group at once.

```ngp
modules { "main.asm" }
group sfx { "beep.asm" }
```

```asm
.proc entry
        rts
.endp
```

Here that is `modules sfx { "beep.asm" }`.

## NGA1178

The tool skips the body of a `group` with no name, so nothing in it is
grouped.

```ngp
modules { "main.asm" }
group { main }
```

```asm
.proc entry
        rts
.endp
```

Name it, as in `group play { main }`.

## NGA1179

A `window` in a `target` block gives its name, its address ranges separated by
commas, and then `views` and the states it can show: unit sets, or named
states of the hardware. Here `views` and the states are missing.

```ngp
target {
  units  extension 4
  window ext $4000 .. $7FFF
}

modules { "main.asm" }
```

```asm
.proc entry
        rts
.endp
```

Write `window ext $4000 .. $7FFF  views extension`.

## NGA1180

Regions, unit sets, Windows and Panes share one name space, because each name
is a Symbol every Module sees. Two Regions of one name are
[`NGA1158`](#nga1158) instead.

```ngp
include "atari/130xe.ngp"

target { region screen $3C00 .. $3FFF reserved }

modules { "main.asm" }

panes in ext { screen }
```

```asm
.proc entry
        rts
.endp
```

Rename one of the two. Where the first is declared in a machine variant,
rename the one in the Project.

## NGA1181

Follows [`NGA1180`](#nga1180) and points at the first declaration of the name,
which may be in a machine variant the Project includes.

## NGA1182

Each range of a `window` runs from a lower address to a higher one, both ends
inclusive and both inside `$0000-$FFFF`. The finding stands at `window`,
whichever of its ranges is wrong.

```ngp
target {
  units  extension 4
  window ext $C000 .. $10000  views extension
}

modules { "main.asm" }
```

```asm
.proc entry
        rts
.endp
```

The usual cause is an end written as the start plus the size. The end is the
last address of the range: `$C000 .. $FFFF`.

## NGA1183

Each name after `views` is a state the Window can show, and each is listed
once. The tool numbers the states in the order they are written, and the driver
receives a state by that number.

```ngp
target {
  window blit $4000 .. $7FFF  views sprites, palette, sprites
}

modules { "main.asm" }
```

```asm
.proc entry
        rts
.endp
```

Remove the second `sprites`.

## NGA1185

`storage { units NAME }` takes the Banks of a unit set the Target declares with
`units`. A usual mistake is the name of the Window that shows the set, as here.
Another is `atari/extension.ngp` included beside a machine that has no
`extension` set, such as `atari/800xl.ngp`: it is the 130XE's storage.

```ngp
include "atari/130xe.ngp"

storage { units ext }

modules { "main.asm" }
```

```asm
.proc entry
        rts
.endp
```

Name the set: `storage { units extension }`.

## NGA1186

A unit set used as storage takes the size of its unit from the Window that
shows the set. No `window` of the Target lists this set after `views`, so
there is no size.

```ngp
target { units extension 4 }

storage { units extension }

modules { "main.asm" }
```

```asm
.proc entry
        rts
.endp
```

Declare the Window that shows the set, as in
`window ext $4000 .. $7FFF  views extension`. Storage that the CPU never sees
through a Window, such as a diskette, is counted instead:
`storage { units 720  size 128 }`.

## NGA1187

A unit of a set is as large as the Window that shows it. Raised where `storage`
names a set that two Windows of different sizes show, so that a unit would have
two sizes.

```ngp
target {
  units  extension 4
  window ext  $4000 .. $7FFF  views extension
  window high $8000 .. $9FFF  views extension
}

storage { units extension }

modules { "main.asm" }
```

```asm
.proc entry
        rts
.endp
```

Make the two Windows the same size, or take the set out of one Window's
`views`.

## NGA1188

With `units NAME`, a unit is a Bank, and its size is the size of the Window
that shows the set. `size` goes only with `units N`, the units known by
number.

```ngp
include "atari/130xe.ngp"

storage { units extension  size $4000 }

modules { "main.asm" }
```

```asm
.proc entry
        rts
.endp
```

Delete `size`.

## NGA1189

`units NAME N` in a `target` block declares the N Banks of a unit set.

```ngp
target { units extension 0 }

modules { "main.asm" }
```

```asm
.proc entry
        rts
.endp
```

Where the machine has no such memory, leave out the `units` line and every
`window` that shows the set.

## NGA1190

A `panes` block opens with `in` and the Window its Panes are in, then
`= STATE` where they are pinned, then the body: `panes in ext { level }`. The
tool skips the body of a block whose header it cannot read.

```ngp
include "atari/130xe.ngp"

modules { "main.asm" }

panes ext { level }
```

```asm
.proc entry
        rts
.endp
```

Here `in` is missing.

## NGA1191

The name after `panes in` is a Window the Target declares with `window`. The
usual mistake is the name of the unit set the Window shows: on the 130XE the
Window is `ext` and the set is `extension`.

```ngp
include "atari/130xe.ngp"

modules { "main.asm" }

panes in extension { level }
```

```asm
.proc entry
        rts
.endp
```

Write `panes in ext { level }`.

## NGA1192

`= STATE` pins the block's Panes to a named state of the Window: a name in its
`views` that is not a unit set. A unit set is a run of Banks, and the solver
picks the Bank.

```ngp
include "atari/130xe.ngp"

modules { "main.asm" }

panes in ext = extension { level }
```

```asm
.proc entry
        rts
.endp
```

To put a Pane in a Bank, leave out `= STATE`: `panes in ext { level }`. See
[Panes](../06-panes.md).

## NGA1193

`NAME[N]` declares a family of N Panes on consecutive Banks, and N is between 1
and 255, because a member is chosen by an index in a byte.

```ngp
include "atari/mariacel.ngp"

modules { "main.asm" }

panes in cel8000 { frames[300] }
```

```asm
.proc entry
        rts
.endp
```

Declare two families instead, such as `framesA[150], framesB[150]`.

## NGA1194

A Window that shows no unit set has only named states, so the solver has no
Bank to give a Pane there.

```ngp
include "atari/mariacel.ngp"

modules { "main.asm" }

panes in cel4000 { strips }
```

```asm
.proc entry
        rts
.endp
```

On MariaCEL, `cel4000` shows the blitter's blocks, so this is
`panes in cel4000 = strip { strips }`.

## NGA1195

A family is N Panes on consecutive Banks. A header with `= STATE` gives each of
its Panes one named state, which is a single state and not a run of Banks.

```ngp
include "atari/mariacel.ngp"

modules { "main.asm" }

panes in cel4000 = strip { strips[2] }
```

```asm
.proc entry
        rts
.endp
```

Drop the `[2]`, or declare the family in a Window that shows a unit set, where
the solver gives it its Banks.

## NGA1196

The body of a `panes` block is a list of names separated by commas, each
followed by `[N]` where it declares a family. Raised for anything else there,
such as a comma before the closing brace, a quoted name, or a count that `]`
does not close. Two names with no comma between them are
[`NGA1129`](#nga1129).

```ngp
include "atari/130xe.ngp"

modules { "main.asm" }

panes in ext { level, title, }
```

```asm
.proc entry
        rts
.endp
```

Delete the last comma.

## NGA1197

`final` marks a Phase the program never leaves. The tool then keeps out of it
what only leaving or returning needs. A `then` in the same Phase says the
program does leave it. See [`phase`](../15-project-file.md#phase).

```ngp
modules { "title.asm"  "game.asm" }

phase title { needs title  entry titleStart  then game }
phase game  { needs game   entry gameStart   then title  final }
entry title
```

```asm title.asm
.proc titleStart
        .transition game
.endp
```

```asm game.asm
.proc gameStart
@loop   jmp @loop
.endp
```

Drop `then title` if the game never goes back to the title, or drop `final` if
it does. The entry of a `final` Phase must not return either, which is
[`NGA4613`](#nga4613).

## NGA1198

A `load` block says what the machine holds while a DOS loads a `.xex`, and
which code of the program runs during the load through `INITAD`. No other
Container is loaded by a DOS that calls `INITAD`.

```ngp
include "atari/800xl.ngp"

modules { "main.asm"  "fade.asm" }

load  { needs fade  init fadeStart }
phase main { needs main }
entry main

container atr
```

```asm
.proc entry
        rts
.endp
```

```asm fade.asm
.proc fadeStart
        rts
.endp
```

Remove the `load` block, or build a `.xex` with `container xex`. See
[`load`](../15-project-file.md#load).

## NGA1199

A `load` block holds two kinds of entry: `needs` and a list of Modules, and
`init` and a Label to call. Where the program starts is the entry Phase's
`entry`, not a word of `load`.

```ngp
include "atari/800xl.ngp"

modules { "main.asm"  "fade.asm" }

load  { needs fade  run fadeStart }
phase main { needs main }
entry main

container xex
```

```asm
.proc entry
        rts
.endp
```

```asm fade.asm
.proc fadeStart
        rts
.endp
```

To call `fadeStart` while the file loads, write `init fadeStart`.

## NGA1203

The `modules` block knows two generators: `binary`, which takes a file's bytes
as they are, and `script`, which runs JavaScript. Any other name before `(` is
refused.

```ngp
modules {
  "main.asm"
  png( "gfx/title.png" ) as title
}
```

```asm
.proc entry
        rts
.endp
```

For a file in another format, convert it to raw bytes with a tool of your own
and read it with `binary`, or write a `script` that decodes the bytes
`nga.read` returns. See [Generators](../08-generators.md).

## NGA1204

`binary` takes the file and the attributes a `.section` takes: `section`,
`namespace`, `placement`, `at`, `align`, `within`, `in`, `root`, `temporary`
and `readonly`. A `script` hands every argument to the script, so a `script`
call does not raise this.

```ngp
modules {
  "main.asm"
  binary( "gfx/font.bin", label = font )
}
```

```asm
.proc entry
        rts
.endp
```

The Label at the start of the Section is `section = font`.

## NGA1205

A generator takes each argument once. The one exception is `in` of `binary`,
which may name several Regions. A second quoted path is the file given twice: a
call works from one file. A `script` is held to the same rule, since it would
see only the last value under a name and only the first path.

```ngp
modules {
  "main.asm"
  binary( "gfx/font.bin", "gfx/font2.bin" ) as font
}
```

```asm
.proc entry
        rts
.endp
```

Write one call per file, and each becomes a Module with a Section of its own.
To keep the bytes of both files in one Section, read them with `nga.read` in a
`script`.

## NGA1206

Each argument of `binary` takes one kind of value. `section`, `namespace` and
`in` take a name, written bare. `at`, `align` and `within` take a number.
`placement` takes `absolute` or `zeropage`. `root`, `temporary` and `readonly`
take no value: the name alone sets them.

```ngp
modules {
  "main.asm"
  binary( "maps/level1.map", in = "level" ) as maps
}
```

```asm
.proc entry
        rts
.endp
```

Only the path is quoted. Write `in = level`.

## NGA1207

The file a generator works from is its one argument without a name, a quoted
path: the data for `binary`, the script for `script`.

```ngp
modules {
  "main.asm"
  script( file = "tools/rows.js", width = 37 ) as rows
}
```

```asm
.proc entry
        rts
.endp
```

Write the path alone: `script( "tools/rows.js", width = 37 )`. A bare word, as
in `binary( font )`, is a flag and not a path.

## NGA1208

What a generator returns is checked before it becomes the text of a Module.
The text after the colon says where the fault is, as a path into what the
script returned, and what is wrong there. See
[What a script returns](../08-generators.md#what-a-script-returns).

```ngp
modules {
  "main.asm"
  script( "tools/squares.js" ) as squares
}
```

```asm
.proc entry
        rts
.endp
```

```js tools/squares.js
export default () =>
{
  const squares = [];
  for ( let i = 0; i < 32; i++ )
  {
    squares.push( i * i );
  }
  return { sections: [ { name: "squares", items: [ squares ] } ] };
};
```

An array of numbers is bytes, and `16 * 16` is not a byte. `{ words: squares }`
writes two bytes for each. `binary` raises this too where its Module's name is
no name, since it names the Section after the Module: `binary( "font-8x8.bin" )`
needs `as font` or `section = font`.

## NGA1209

The arguments of a generator call are separated by commas and closed by `)`.
The message names the first token after an argument that is neither.

```ngp
modules {
  "main.asm"
  binary( "gfx/font.bin"  align = 1024 ) as font
}
```

```asm
.proc entry
        rts
.endp
```

Here the comma before `align` is missing. An unquoted path stops the call too,
at its first `.` or `/`: quote the path.

## NGA1210

The script did not run to its end. It threw, it is not valid JavaScript, it has
no default export, or it used something the engine does not have. The text
after the colon is the engine's, with the line of the script. There is no
`Date`, no `Math.random`, no module to import, and no file access but
`nga.read`, which throws where the file is not there.

```ngp
modules {
  "main.asm"
  script( "tools/noise.js" ) as noise
}
```

```asm
.proc entry
        rts
.endp
```

```js tools/noise.js
export default () =>
{
  const noise = [];
  for ( let i = 0; i < 16; i++ )
  {
    noise.push( Math.floor( Math.random() * 256 ) );
  }
  return { sections: [ { name: "noise", items: [ noise ] } ] };
};
```

`Math.random` is undefined here, so calling it is `not a function`. There is no
random because a build must give the same bytes every time it runs. For a table
that looks random, write a seeded generator in the script. To refuse an input,
call `nga.error` rather than throw; that is [`NGA1212`](#nga1212).

## NGA1211

A script is stopped after a fixed amount of work, counted by the engine and not
in seconds, so a slow machine stops it where a fast one does. The allowance is
generous, and a script that reaches it almost always loops forever.

```ngp
modules {
  "main.asm"
  script( "tools/pairs.js", count = 37 ) as pairs
}
```

```asm
.proc entry
        rts
.endp
```

```js tools/pairs.js
export default ( args ) =>
{
  let pairs = 0;
  for ( let left = args.count; left !== 0; left -= 2 )
  {
    pairs++;
  }
  return { sections: [ { name: "pairs", items: [ [ pairs ] ] } ] };
};
```

Here `left` steps over zero, because the count is odd. `--verbose` reports how
much of the allowance each script used.

## NGA1212

The text after the colon comes from the generator, not from the tool. A script
that is given something it cannot use says so itself with `nga.error`, and
this is how it reaches the reader. The finding stands at the call in the
Project, and the note, [`NGA1214`](#nga1214), points at the line of the script.

```ngp
modules {
  "main.asm"
  script( "tools/rows.js", width = 48 ) as rows
}
```

```asm
.proc entry
        rts
.endp
```

```js tools/rows.js
export default ( args, nga ) =>
{
  if ( args.width > 40 )
  {
    nga.error( `a row of ${ args.width } is wider than the screen` );
  }
  return { sections: [ { name: "row", items: [ { reserve: args.width } ] } ] };
};
```

Fix what the message names, usually an argument of the call or the file the
script reads. A script that throws raises [`NGA1210`](#nga1210) instead.

## NGA1213

A script called `nga.warn`. The text after the colon is the script's own, the
note, [`NGA1214`](#nga1214), points at its line, and the build goes on.

```ngp
modules {
  "main.asm"
  script( "tools/tiles.js", count = 33 ) as tiles
}
```

```asm
.proc entry
        rts
.endp
```

```js tools/tiles.js
export default ( args, nga ) =>
{
  if ( args.count > 32 )
  {
    nga.warn( `${ args.count } tiles, and a screen shows 32 at once` );
  }
  return { sections: [ { name: "tiles", items: [ { reserve: args.count * 8 } ] } ] };
};
```

Every script's warnings share this identifier, so `--deny NGA1213` on the
command line makes all of them errors. The Project's `diagnostics` block does
not reach it, wherever the block stands: a script runs while the Project is
read, and the block takes effect after.

## NGA1214

Follows [`NGA1212`](#nga1212) or [`NGA1213`](#nga1213) and points at the line
and column of the script where `nga.error` or `nga.warn` was called. The
finding itself stands at the call in the Project.

## NGA1215

`--fact` names the sets of facts `--facts` writes: `diagnostics`, `project`,
`map`, `sources`, `tokens`, `symbols` and `bytes`, or `all`, several of them
separated by commas. A name that is none of these is an error, and no empty set
is written for it. Without `--fact`, `--facts` writes `diagnostics` and
`project`.

## NGA1216

Only a cartridge Container takes a quoted board after its name: `car` on the
Atari, and `lnx` and `lyx` on the Lynx. Raised where a board follows any other
Container, such as one left behind when a Project moved from a `.car` to a
`.xex`.

```ngp
container xex "8k"

modules { "main.asm" }
```

```asm
.proc entry
        rts
.endp
```

Delete the quoted word, or name the Container the board is for.

## NGA1217

A cartridge image is the image of one board. On the Atari the board decides
how large the image is, where each bank stands in it, and which mapper number
its header carries. On the Lynx it decides the size of a page. A Container of
either kind without a board is refused rather than guessed at.

```ngp
container car

modules { "main.asm" }
```

```asm
.proc entry
        rts
.endp
```

Write the board after the Container, in quotes: `container car "xegs128"`.
The message lists the boards there are. On the Lynx they are `"64k"`,
`"128k"`, `"256k"` and `"512k"`. See [Cartridges](../14-cartridges.md).

## NGA1218

The quoted board names none the tool writes. The Atari's are `"8k"` and
`"16k"`, which have no banks, and the XEGS boards from `"xegs32"` to
`"xegs1024"`. The Lynx's are `"64k"`, `"128k"`, `"256k"` and `"512k"`.

```ngp
container car "128k"

modules { "main.asm" }
```

```asm
.proc entry
        rts
.endp
```

`"128k"` is a Lynx board. The 128 KB XEGS board is `"xegs128"`. A mapper the
tool does not write, such as a MegaCart, has no name here;
[Cartridges](../14-cartridges.md#what-is-not-written) says why.

## NGA1219

The board in `container car` and the memory the Target declares are checked
against each other. The board's fixed part, the ROM no switch takes away, must
be the Target's `rom` Region, at the same range. `"8k"` and the XEGS boards
are ROM at `$A000-$BFFF`, and `"16k"` at `$8000-$BFFF`. Also raised where the
Target has no `rom` Region at all.

```ngp
target {
  containers car
  region $0080 .. $00FF ram
  region $2000 .. $7FFF ram
  region $A000 .. $BFFF rom
}

container car "16k"

modules { "main.asm" }
```

```asm
.proc entry
        rts
.endp
```

The tool derives neither from the other, so fix the one that is wrong: the
board's name, or the `rom` Region of the variant. The library's
`atari/xegs128.ngp` is the variant for `"xegs128"`.

## NGA1220

An XEGS board switches one bank at a time into `$8000-$9FFF`. The Target must
declare a Window of exactly that one range, which shows the board's banks as a
unit set. Raised where no `window` has that range.

```ngp
target {
  containers car
  region $0080 .. $00FF ram
  region $2000 .. $7FFF ram
  region $A000 .. $BFFF rom
}

container car "xegs32"

modules { "main.asm" }
```

```asm
.proc entry
        rts
.endp
```

Declare the unit set and the Window in the variant, as `atari/xegs128.ngp`
does with `units banks 15` and `window switched $8000 .. $9FFF views banks`.

## NGA1221

The unit set the board's Window shows holds one Bank fewer than the board has
banks. The last bank is the fixed part, which the CPU always sees, so it is
not storage. A 128 KB XEGS board has sixteen banks and a set of fifteen; a
64 KB board has eight and a set of seven.

```ngp
include "atari/xegs128.ngp"

container car "xegs64"

modules { "main.asm" }
```

```asm
.proc entry
        rts
.endp
```

Name the board the variant describes, or change the count in its `units`
line.

## NGA1229

An 8 KB and a 16 KB cartridge have no banks. The image is the fixed part and
nothing else, so there is nowhere for storage to be, and a unit written into
the image would stand over the program. Raised where a Project names such a
board and its machine declares storage.

```ngp
container car "8k"

modules { "main.asm" }

target {
  containers car
  region zp    $0080 .. $00FF  ram
  region ram   $2000 .. $20FF  ram
  region cart  $A000 .. $BFFF  rom
  holder cartinit
}

storage { units 4 size 8192 }
```

```asm
.proc entry
        rts
.endp
```

Remove the `storage` block, or name a XEGS board, whose switched banks are
storage.

## NGA1230

The `cartridge` block fills the header of a `.lnx`. Each entry is a field and
its value, with nothing between them: `name "Asteroids"`, `rotation left`.
Raised at a token that is neither, such as a number, or the `=` in
`name = "Asteroids"`. What is left of the entry, the `"Asteroids"`, is read
past, so the entry costs one finding.

```ngp
include "lynx/lynx.ngp"

container lnx "64k"

cartridge {
  rotation 1
}

modules { "main.asm" }
```

```asm
.proc entry
        rts
.endp
```

The header holds the rotation as a number, but the block takes the word for
it: `none`, `left` or `right`.

## NGA1231

The `cartridge` block sets three fields of the `.lnx` header: `name`,
`manufacturer` and `rotation`. The page size comes from the board in
`container lnx`, and the tool writes every other field itself, with a fixed
value.

```ngp
include "lynx/lynx.ngp"

container lnx "64k"

cartridge {
  name  "Asteroids"
  year  "1990"
}

modules { "main.asm" }
```

```asm
.proc entry
        rts
.endp
```

Delete the entry.

## NGA1232

`name` and `manufacturer` take a quoted string, and `rotation` takes a word.
Raised where the value is of the other kind: a name without quotes, or
`rotation "left"`.

```ngp
include "lynx/lynx.ngp"

container lnx "64k"

cartridge {
  name Asteroids
}

modules { "main.asm" }
```

```asm
.proc entry
        rts
.endp
```

Quote the name, or take the quotes off the rotation.

## NGA1233

The header has 32 bytes for `name` and 16 for `manufacturer`. A reader writes
a terminator over the last byte, so the text may be 31 and 15 bytes long. The
tool refuses a longer one instead of cutting it. The count is of bytes, not
characters.

```ngp
include "lynx/lynx.ngp"

container lnx "64k"

cartridge {
  manufacturer "Atari Corporation, Sunnyvale"
}

modules { "main.asm" }
```

```asm
.proc entry
        rts
.endp
```

Shorten the text.

## NGA1234

`name`, `manufacturer` and `rotation` are each set once in the Project, and an
empty name counts as a name given. `cartridge` blocks sum, so this holds across
several blocks and every file an `include` reaches. A second value would depend
on which file is read first.

```ngp
include "lynx/lynx.ngp"

container lnx "64k"

cartridge {
  name "Asteroids"
  name "Asteroids II"
}

modules { "main.asm" }
```

```asm
.proc entry
        rts
.endp
```

Keep one of them.

## NGA1235

The rotation says how the Lynx is held for this program, so that a reader can
turn the picture: `none`, `left` or `right`. Raised for any other word.

```ngp
include "lynx/lynx.ngp"

container lnx "64k"

cartridge {
  rotation upright
}

modules { "main.asm" }
```

```asm
.proc entry
        rts
.endp
```

`none` is the usual way up, and is what a block without `rotation` writes.

## NGA1236

The `cartridge` block fills the header of a `.lnx`, and is raised with any
other Container. A `.lyx` is the same image without the header, so a name
written for it would never be read.

```ngp
include "lynx/lynx.ngp"

container lyx "64k"

cartridge {
  name "Asteroids"
}

modules { "main.asm" }
```

```asm
.proc entry
        rts
.endp
```

Write `container lnx` to get the header, or delete the block.

## NGA1237

A Lynx cartridge has 256 pages, and the board in `container lnx` or
`container lyx` says how wide a page is: 256, 512, 1024 or 2048 bytes. Page
zero is the bootstrap's, so storage is 255 units of that width. Raised where
the storage the Project includes is something else. A driver handed a unit of
the wrong width would read the wrong bytes.

```ngp
include "lynx/lynx.ngp"
include "lynx/cart256k.ngp"

container lnx "64k"

modules { "main.asm" }
```

```asm
.proc entry
        rts
.endp
```

Include the storage file of the board: `lynx/cart64k.ngp` for `"64k"`, and so
on up to `lynx/cart512k.ngp` for `"512k"`.

## NGA1238

A `d64`, `d80` or `d82` is the surface of one Commodore drive, and its tracks
are the units of storage. A unit's number is its track's, so there is one unit
more than there are tracks, unit zero being no track. A unit is as wide as the
widest track. Raised where the storage the Project includes does not match the
Container.

```ngp
include "pet/3032.ngp"
include "pet/d80.ngp"

container d64

modules { "main.asm" }
```

```asm
.proc entry
        rts
.endp
```

Include the storage file named after the Container: `pet/d64.ngp`,
`pet/d80.ngp` or `pet/d82.ngp`.

## NGA1239

`frames held` makes a Transition read the list of blocks in its Frame once,
instead of going back to the Frame in storage between one block and the next.
It costs code in every Phase, and pays where opening the stream is slow, as on
a Lynx cartridge or a diskette. `held` is the only word `frames` takes.
Leaving the line out is the other choice.

```ngp
frames buffered

modules { "main.asm" }
```

```asm
.proc entry
        rts
.endp
```

## NGA1240

`frames` stands once in the Project, counting every file an `include`
reaches.

```ngp
frames held
frames held

modules { "main.asm" }
```

```asm
.proc entry
        rts
.endp
```

Delete one of them. [`NGA1241`](#nga1241) points at the first.

## NGA1241

Follows [`NGA1240`](#nga1240) and points at the first `frames`.

## NGA1242

`base` named the state a Window showed outside a `.with`, in a `window` line,
a `group` or a `phase`. It is gone: no Window has a base, and what a Window
shows outside a `.with` is undefined. The tool still reads the word, to say
so, and skips what follows it.

```ngp
target {
  units  extension 4
  window ext $4000 .. $7FFF  views extension, main  base main
}

modules { "main.asm" }
```

```asm
.proc entry
        rts
.endp
```

Delete `base` and its name. Code that reads through the Window shows the state
first with `.with`, and a Section that lived in the base belongs in a Pane. See
[`window`](../15-project-file.md#window) and [Panes](../06-panes.md).

## NGA1243

`frames held` applies to Frames, and only a Project with `storage` has them.
Without storage a Transition loads nothing: every Section with bytes is
already in memory, and taking an edge is a jump into the Phase it enters.
Raised in a Project that has a `then` and no storage.

```ngp
frames held

modules { "main.asm" }

phase intro { needs main  then game  entry introStart }
phase game  { needs main             entry gameStart }
entry intro
```

```asm
.proc introStart
        .transition game
.endp

.proc gameStart
        rts
.endp
```

Delete `frames held`, or include the storage of the machine, such as
`atari/disk.ngp`.

## NGA1244

A unit set with `linear` lies in the medium's own memory, as a blitter or a
video chip reads it: Bank 0 at the start of the range, and each next Bank
right after the one before. A Bank is as large as the Window that shows the
set. Raised where the Banks, end to end, run past the end of the range.
`linear` is what gives a Section there a `linearSectionAddress`; see
[Attribute access](../16-assembler.md#attribute-access).

```ngp
target {
  units  vram 4  linear $0 .. $7FFF
  window w $4000 .. $7FFF  views vram
}

modules { "main.asm" }
```

```asm
.proc entry
        rts
.endp
```

Four Banks of 16 KB need 64 KB, and the range is 32 KB. Correct the count, the
range or the Window.

## NGA1245

The `linear` range of a unit set runs from a lower address to a higher one,
both ends inclusive. Raised where the end is below the start, where the start
is negative, or where the end is `$FFFFFFFF` or more.

```ngp
target {
  units  vram 2  linear $7FFF .. $0
  window w $4000 .. $7FFF  views vram
}

modules { "main.asm" }
```

```asm
.proc entry
        rts
.endp
```

Write the lower address first.

## NGA1246

`--overlay PATH FILE` makes the build read FILE wherever it would read PATH.
An editor uses it to build the text it holds and has not saved. Raised where
FILE cannot be read. The run stops there rather than read PATH from the disk,
which is not the text being looked at. Check the path of FILE.

## NGA1247

A run given `--overlay` reads text that is on no disk, so a binary written
from it would match no saved source. Raised where `-o` stands beside
`--overlay`. Use `--overlay` for findings and `--facts`, and save the files
before building the binary.

## NGA1248

Raised where a Project file wants quotes and finds none: the path after
`include`, the processor's name after `cpu`, and a cartridge's board after
`container car`, `lnx` or `lyx`. A processor's name and a board begin with a
digit, which no name of the Project file may, and a path holds `/` and `.`. A
path without its quotes is read past whole, so the finding is one.

```ngp
target { cpu 6502 }

modules { "main.asm" }
```

```asm
.proc entry
        rts
.endp
```

Quote it: `cpu "6502"`, `include "atari/800xl.ngp"`. In `include atari/800xl.ngp`
the lexer also refuses the digits of `800xl`, as [`NGA0011`](#nga0011), before
the Project is read.

## NGA1249

Only a `.xex` can be compressed. Its loader calls code between the segments it
writes, and a compressed segment needs that call to be decoded. No other
Container's loader does this, so `compress` after any other one is refused.

```ngp
container atr compress zx0

modules { "main.asm" }
```

```asm
.proc entry
        rts
.endp
```

Delete `compress zx0`, or write the program as a `.xex`.

## NGA1250

`compress` names the format the segments are carried in, and the tool has to
be able to encode it. The finding lists the formats it encodes. `copy` is a
format but saves nothing, so it is refused too.

```ngp
container xex compress copy

modules { "main.asm" }
```

```asm
.proc entry
        rts
.endp
```

Name one of the formats the finding lists. `zx0` compresses best.

## NGA1251

`except` lists Modules and groups by name, as `resident` does. A Module's name
is the stem of its file, or the name after `as` in `modules`. A quoted path is
not a name.

```ngp
modules { "main.asm" }

container xex compress zx0 except { "main.asm" }
```

```asm
.proc entry
        rts
.endp
```

Write the name without quotes: `except { main }`.

## NGA1252

`choose` names the documents a `.xex` may be changed into while it loads. With
one document there is nothing to choose between.

```ngp
include "atari/800xl.ngp"

choose { "low.ngp" }

modules { "main.asm" }
container xex
```

```ngp low.ngp
target { register PORT $D640 }
```

```asm
.proc entry
        sta PORT
        rts
.endp
```

Name the second document, or `include` the one there is.

## NGA1253

A Project has one `choose` block. Two of them would be two sets of documents
for one byte to choose between.

```ngp
include "atari/800xl.ngp"

choose { "low.ngp" "high.ngp" }
choose { "low.ngp" "high.ngp" }

modules { "main.asm" }
container xex
```

```ngp low.ngp
target { register PORT $D640 }
```

```ngp high.ngp
target { register PORT $D740 }
```

```asm
.proc entry
        sta PORT
        rts
.endp
```

Put every document in one block.

## NGA1254

Follows [`NGA1253`](#nga1253) and points at the first `choose` block.

## NGA1255

A document that `choose` names may declare `register` lines in a `target`
block and `constants` entries, and nothing else. Every document is built over
the first one's layout, so nothing in one may change what is placed or where.
An `include` is refused too, since the included file could declare anything.

```ngp
include "atari/800xl.ngp"

choose { "low.ngp" "high.ngp" }

modules { "main.asm" }
container xex
```

```ngp low.ngp
target { register PORT $D640 }
modules { "extra.asm" }
```

```ngp high.ngp
target { register PORT $D740 }
```

```asm
.proc entry
        sta PORT
        rts
.endp
```

Declare what is not a value in the Project, outside the documents.

## NGA1256

Every document that `choose` names declares the same names. A name that one
declares and another does not would leave a build of the other without it.

```ngp
include "atari/800xl.ngp"

choose { "low.ngp" "high.ngp" }

modules { "main.asm" }
container xex
```

```ngp low.ngp
target { register PORT $D640 }
constants { LEVEL = 1 }
```

```ngp high.ngp
target { register PORT $D740 }
```

```asm
.proc entry
        lda #LEVEL
        sta PORT
        rts
.endp
```

Give the name a value in every document, even where the value is the same.

## NGA1257

Only a `.xex` without storage runs code of the program's while it loads and is
written into memory as it is, so only such a file can be changed into another
document while it loads. The message says what this program is instead.

```ngp
include "atari/800xl.ngp"

choose { "low.ngp" "high.ngp" }

modules { "main.asm" }
container atr
```

```ngp low.ngp
target { register PORT $D640 }
```

```ngp high.ngp
target { register PORT $D740 }
```

```asm
.proc entry
        sta PORT
        rts
.endp
```

Build one file for each document instead: one small Project for each that
includes its document and the common one.

## NGA1258

The program is placed as the first document builds it, so a section may stand
anywhere the first document's registers leave free. A register that a later
document puts in that memory could have a section on top of it.

```ngp
include "atari/800xl.ngp"

choose { "low.ngp" "high.ngp" }

modules { "main.asm" }
container xex
```

```ngp low.ngp
target { register PORT $D640 }
```

```ngp high.ngp
target { register PORT $3000 }
```

```asm
.proc entry
        sta PORT
        rts
.endp
```

Put the register where the machine has registers, in a `register` region.

## NGA2202

Follows a finding about a name defined twice, [`NGA2203`](#nga2203),
[`NGA2205`](#nga2205) or [`NGA2213`](#nga2213), and points at the first
definition. After [`NGA2211`](#nga2211) it points at the first of the local
labels the reference could mean.

## NGA2203

A Module has one set of names for its Symbols, whatever their kind, so a
Constant and a Label cannot share a name. A Section opens no scope: a Label in
one Section collides with the same Label in another. A Proc does open one. A
plain label inside `.proc clear` is `clear.fill`, and two Procs may each have
a `fill`.

```asm
.section
buffer  .res 16
.ends

.section
buffer  .res 16
.ends

.proc entry
        lda buffer
        rts
.endp
```

Rename one, or move it into a Proc or a [Namespace](../16-assembler.md#namespaces).
Inside a Proc, `@` labels may repeat; see
[Local labels](../16-assembler.md#local-labels).

## NGA2204

`.export` names a Symbol of its own Module. The tool resolves it at the end of
the Module, so the definition may come after it. It looks in the block the
`.export` stands in, and a qualified name is [`NGA0145`](#nga0145). Raised
where no Symbol of that name is found there: a misspelling, or a name defined
inside a `.namespace` block while the `.export` stands outside it.

```asm
.export draw

.namespace gfx
.proc draw
        rts
.endp
.endns

.proc entry
        jsr gfx.draw
        rts
.endp
```

Move the `.export` into the `.namespace` block. Another Module then reaches the
Proc as `gfx.draw`.

## NGA2205

Exported names share one table across the Project, whatever Phases their
Modules are in, so two Modules that export one name collide. In C every
function not declared `static` is exported. The finding stands on the second
definition and the note on the first.

```asm
.export draw

.proc draw
        rts
.endp

.proc entry
        jsr draw
        rts
.endp
```

```asm other.asm
.export draw

.proc draw
        rts
.endp
```

Rename one, or keep one private: drop its `.export`, or declare the C function
`static`. Where the two are one routine written for different Phases, the
answer is a [Slot](../07-slots.md).

## NGA2206

Nothing defines the name where the use can see it. The search runs from the
enclosing Namespace outwards, and at each step takes the Module's own Symbols
first and the exported ones of other Modules second. A Symbol of another Module
is visible only once that Module exports it.

```asm
.proc entry
        jsr clearScreen
        rts
.endp
```

```asm other.asm
.proc clearScreen
        rts
.endp
```

Add `.export clearScreen` to `other.asm`. The same finding names an unknown
Charset in a string's prefix, as in `atasci"OOPS"`, and an unknown Slot after
`.implements`. An undefined `a` after `asl` or another instruction with an
accumulator form is [`NGA2208`](#nga2208).

## NGA2207

A Constant whose expression reaches itself, directly or through other
Constants, has no value. The finding stands once, at the reference that closes
the loop.

```asm
WIDTH  = HEIGHT * 2
HEIGHT = WIDTH / 2

.proc entry
        lda #WIDTH
        rts
.endp
```

Give one of them a literal value.

## NGA2208

The accumulator form of `asl`, `lsr`, `rol`, `ror`, and of `inc` and `dec` on a
65SC02, is written without an operand. Other assemblers write `asl a`. Here `a`
would be an operand naming a Symbol, and this is raised where the program
defines no `a`.

```asm
.proc entry
        lda #1
        asl a
        rts
.endp
```

Write `asl`. A program that defines a Label or a Constant `a` gets no finding:
`asl a` shifts the byte at that address.

## NGA2210

A local label is reached only inside the Proc or macro body that defines it.
`@-name` takes the nearest definition before the reference in the source,
`@+name` the nearest after, and `@name` the only one. Raised where none
matches: the label is on the other side, in another Proc, or of the other
kind, since `@-` passes over every `@name` and `@-name` over every `@`.

```asm
.proc entry
        lda $F0
        beq @-done
        sta $F1
@done   rts
.endp
```

`@done` comes after the branch, so the reference is `@+done`.

## NGA2211

A bare `@name` stands for the one definition of `name` in its Proc or macro
body. Where there are several, the reference does not choose. A bare `@wait`
that worked stops working once a second `@wait` appears anywhere in the Proc,
before it or after it.

```asm
.proc entry
        ldx #8
@wait   dex
        bne @wait
        ldy #8
@wait   dey
        bne @-wait
        rts
.endp
```

Write `@-wait` for the nearest definition before the reference, or `@+wait`
for the nearest after.

## NGA2212

`@` alone defines an anonymous local label. A reference to one says which way
to look: `@-` for the nearest before, `@+` for the nearest after.

```asm
.proc entry
        ldx #8
@       dex
        bne @
        rts
.endp
```

The loop is behind the branch, so the reference is `bne @-`.

## NGA2213

A plain label of a macro body belongs to the body and is made again at each
use. It is unique in the body. `@` labels may repeat, and are another kind of
name: `wait` never reaches `@wait`, so the two may share a spelling.

```asm
.macro delay
        ldx #0
wait    dex
        bne wait
        ldx #0
wait    dex
        bne wait
.endm

.proc entry
        delay
        rts
.endp
```

Rename one, or write both as `@wait` and reach each with `@-wait`.

## NGA2214

A macro parameter stands for what the use wrote. A label of the body with the
parameter's name would mean one thing where it is defined and another where it
is read. The same holds for a name a `.case` binds.

```asm
.macro delay count
        ldx #count
count   dex
        bne count
.endm

.proc entry
        delay 8
        rts
.endp
```

Rename the label or the parameter.

## NGA2215

A local label defined in a branch of an `.if` is visible in that branch and the
branches inside it, and nowhere else. The branch may not be assembled, so a
reference outside it cannot count on the label being there. Raised where the
only definitions of the name stand in other branches.

```asm
FAST = 1

.proc entry
        ldx #8
.if FAST
@loop   dex
.else
@loop   nop
        dex
.endif
        bne @-loop
        rts
.endp
```

Define the label once, on a line of its own before the `.if`, so that both
branches start at it. See
[Conditional assembly](../16-assembler.md#conditional-assembly).

## NGA2220

An operator or a directive wants an Integer and was given an Address, a String
or a Pane. `*`, `/`, `<<`, `>>`, `-x`, `~x` and `!x` take Integers only;
[Operators](../16-assembler.md#operators) has the rest. The usual case is the high
byte of an address written as a division.

```asm
.section
buffer  .res 256
.ends

.proc entry
        lda #buffer / 256
        sta $F1
        rts
.endp
```

Take the byte with `>buffer`, or `<buffer` for the low one. Both are
Integers.

## NGA2221

An Address plus an Integer is an Address, and one Address minus another is the
Integer distance between them. The sum of two Addresses has no meaning, so it
is refused. A midpoint is the usual case.

```asm
.section
table   .byte 1, 2, 3, 4, 5, 6
tableEnd
.ends

.proc entry
        lda (table + tableEnd) / 2
        rts
.endp
```

Write the midpoint as a position plus half the distance:
`table + (tableEnd - table) / 2`.

## NGA2222

An Integer minus an Address is refused, as `-buffer` is. Where an Address
stands in a subtraction it stands on the left: `buffer - 1` is an Address, and
`tableEnd - table` is an Integer.

```asm
.section
buffer  .res 16
.ends

.proc entry
        ldx #$100 - buffer
        rts
.endp
```

Where the arithmetic needs a byte of the address, take it first: `<buffer`
and `>buffer` are Integers.

## NGA2223

Each Section is placed on its own. The distance between Labels of two
Sections, and which of them comes first, is what the solver chose, and another
build may choose differently. A difference of two Addresses, and a `<`, `<=`,
`>` or `>=` between them, needs both in one Section and one space. `==` and
`!=` do not.

```asm
.section
header  .byte 1, 2
.ends

.section
body    .byte 3, 4, 5
.ends

.proc entry
        ldx #body - header
        rts
.endp
```

Put both Labels in one Section where the distance is meant to be fixed. The
size of a whole Section is `runtimeSectionSize`, and needs no second Label. See
[Subtracting addresses](../16-assembler.md#subtracting-addresses).

## NGA2224

The attributes after a dot, such as `runtimeSectionAddress`,
`runtimeSectionSize` and `resident`, belong to a Section, and only a Label
reaches them. A Constant is a number even where it holds an address, and has
no Section. The same holds for a register, a Region's name and any expression.

```asm
buffer = $0600

.proc entry
        ldx #buffer.runtimeSectionSize
        rts
.endp
```

Declare the memory as a Section with a Label and `.res`, and the Section has
the attributes. Where the address must be fixed, pin it with `absolute at`.
See [Attribute access](../16-assembler.md#attribute-access).

## NGA2225

A Label is an Address, and `.byte` has room for one byte of it.

```asm
.section
pointers
        .byte message
.ends

.section
message
        .byte "HELLO"
.ends
```

Write `<message` or `>message` for one byte of the address, or `.word message`
for both. A String in `.word` is refused the same way, and so is a number in
`.dispatch`, which takes Labels of its own Section.

## NGA2226

A string literal is a String even when it holds one character, and no
instruction takes one. The character literal `'A'` is an Integer.

```asm
.proc entry
        lda #"A"
        rts
.endp
```

Write `lda #'A'`. With a Charset prefix, as in `lda #screen'A'`, the value is
the byte that Charset maps the character to.

## NGA2227

A Label reaches five attributes of its Section: `runtimeSectionAddress`,
`runtimeSectionSize`, `resident`, `linearSectionAddress` and
`linearFamilyAddress`. Any other name after the dot is refused, and so are
`storagePayloadAddress` and `storagePayloadSize`, which are reserved and not
built.

```asm
.section
buffer  .res 64
.ends

.proc entry
        ldx #buffer.size
        rts
.endp
```

The size is `buffer.runtimeSectionSize`. After the name of a Proc, a name the
Proc does not define is read as an attribute, so a misspelt `worker.count` gets
this finding too. See [Attribute access](../16-assembler.md#attribute-access).

## NGA2228

The conditional value `c ? a : b` has the type of its answers. A Constant that
holds a register's address is an Integer, and a Label is an Address.

```asm
SHADOW = 1
COLBK  = $D01A

.section
colorShadow
        .res 1
.ends

.proc entry
        lda #$34
        sta SHADOW ? colorShadow : COLBK
        rts
.endp
```

Choose the statement instead of the value: put `sta colorShadow` and
`sta COLBK` in the two branches of an `.if SHADOW`. See
[The conditional value](../16-assembler.md#the-conditional-value).

## NGA2229

An Address carries its Section, and both subtracting it from another Address
and deciding the width of an instruction need that Section. A choice between
Labels of two Sections has none to carry.

```asm
PAL = 1

.section
palSpeeds
        .byte 5, 6, 7
.ends

.section
ntscSpeeds
        .byte 6, 7, 8
.ends

.proc entry
        lda PAL ? palSpeeds : ntscSpeeds
        rts
.endp
```

Put both tables in one Section, with a Label at each, and the conditional is
accepted. Or keep one Label and choose its bytes with `.if PAL` inside the
Section.

## NGA2230

The prefix of a literal, and the base after `:` in a derived `.charset`, must
name a Charset. Here `font` is the Label of the font's bytes, which is not the
mapping of characters to them.

```asm
.section
font    .res 1024
.ends

.section
title
        .byte font"HELLO"
.ends
```

Declare the mapping with `.charset` under a name of its own, and use that name
as the prefix. A prefix that names nothing at all is
[`NGA2206`](#nga2206).

## NGA2231

Every character of a prefixed literal must be one its Charset maps. An escape
is asked of the Charset like any other character, so `\n` needs an entry too.
The finding names the first character that is missing, once per literal.

```asm
.charset font
  " " = $00
  "ABCDEFGHIJKLMNOPQRSTUVWXYZ" = $01
.endch

.section
title
        .byte font"Game Over"
.ends
```

This font has no lower case. Add a run to the `.charset` for the missing
characters, or write the text in characters it maps.

## NGA2232

A derived Charset is built from the table of its base, so the base must be
built first. Here each is the other's base, and neither can be built. The
finding stands at the first declaration of the cycle.

```asm
.charset bright : dim ^ $80
.endch

.charset dim : bright ^ $80
.endch
```

Give one of them entries of its own and no base, and derive the other from it.

## NGA2233

Inside one `.charset` block a character is mapped once, since a literal holding
it would otherwise have two bytes to choose from. Two characters may share one
byte, which is what a font with only upper case wants.

```asm
.charset font
  "ABCDEFGHIJKLMNOPQRSTUVWXYZ" = $01
  "0123456789"                 = $1B
  " .,:!?O"                    = $25
.endch
```

`O` is in the first run and again in the last. Remove one of the two. An entry
of a derived Charset overrides its base, and that is not a duplicate, but two
entries of one derived block for one character are.

## NGA2234

A run takes consecutive bytes from the value on its right, one per character,
and every one of them must be between `$00` and `$FF`. The finding gives the
first and last byte the run would cover, in decimal. A mask after `^` wider
than a byte is refused the same way.

```asm
.charset font
  "0123456789"                 = $00
  "ABCDEFGHIJKLMNOPQRSTUVWXYZ" = $F0
.endch
```

Start the run lower, or split it into runs that fit.

## NGA2235

The value on the right of a `.charset` entry, and the mask after `^`, must be
[declared values](../16-assembler.md#declared-values): literals, and Constants
made of literals. A Label, a size, or the distance between two Labels is known
only later.

```asm
.section
glyphs
        .res 8
letters .res 208
.ends

.charset font
  "ABCDEFGHIJKLMNOPQRSTUVWXYZ" = letters - glyphs
.endch
```

Write the number, or a Constant that holds it.

## NGA2236

A Charset is named only as the prefix of a literal, as in `font"HELLO"` or
`font'A'`. Its name alone has no value.

```asm
.charset font
  "ABCDEFGHIJKLMNOPQRSTUVWXYZ" = $01
.endch

.proc entry
        lda #font
        rts
.endp
```

For the byte of one character, write `lda #font'A'`.

## NGA2237

A literal without a prefix is the source's own bytes, so it must be ASCII.
Nothing says which byte a character above 127 would be, because the machine's
own character set is neither Unicode nor ASCII.

```asm
.section
credits
        .byte "© 1984 NGA"
.ends
```

Put the name of a Charset that maps the character in front of the literal, as
in `atascii"..."`. The runs inside a `.charset` are not checked, since they
list the characters a font draws. See [Character sets](../11-character-sets.md).

## NGA2240

`.transition NAME` names a Phase that a `phase` block declares. Here the name
is misspelt.

```ngp
modules { "intro.asm" "game.asm" }

phase intro { needs intro  then game  entry introStart }
phase game  { needs game              entry gameStart }
entry intro
```

```asm intro.asm
.proc introStart
        .transition gmae
.endp
```

```asm game.asm
.proc gameStart
@loop   jmp @loop
.endp
```

The statement may have meant the edge `then game`, so that edge is not also
reported as taken by nothing, which would be [`NGA2246`](#nga2246). A Project
that declares no `then` at all is checked the same way.

## NGA2241

The code of a `.transition` does not know at run time which Phase it runs in.
So every Phase that needs its Module must declare `then` to the Phase it
names, and the finding names each Phase that does not.

```ngp
modules { "intro.asm" "game.asm" }

phase intro { needs intro  then game  entry introStart }
phase game  { needs game              entry gameStart }
entry intro
```

```asm intro.asm
.proc introStart
        .transition game
.endp
```

```asm game.asm
.proc gameStart
        .transition intro
.endp
```

Add `then intro` to the `game` Phase. A `.transition` in a Module that
`resident` names needs the edge from every Phase. In a Project where no Phase
declares `then`, every `.transition` that names a Phase is refused this way.

## NGA2242

Raised where a Project with Transitions declares more than 254 Phases. A
Phase's number is one byte in every Frame and in every `.transition`, and `$FF`
ends a table. Reduce the number of Phases: where several Phases run the same
code over different data, one Phase can run it.

## NGA2243

Raised with storage, for an edge that would load more than 255 Sections. The
Frame of an edge lists each Section it loads as one block and counts the blocks
in a byte. An edge loads every Section with bytes of a Module the entered Phase
needs and the Phase it leaves does not, whether or not a `.transition` takes
it. A Section in a Pane or in a unit set is not counted: the Container writes
it into its Bank once, and no edge loads it.

Use fewer, larger Sections in the Modules that Phase brings. A Module both
Phases need is kept and not loaded.

## NGA2244

The `transform` block names a format the tool encodes, since the tool writes
the Payload in that form when it builds the program. The formats are `copy`,
`rle` and `zx0`.

```ngp
modules { "main.asm" }

transform lz4 { main.title }
```

```asm
.section
title   .byte "TITLE SCREEN"
.ends

.proc entry
        lda title
        rts
.endp
```

A `.transform` in a Module that names an unknown format is
[`NGA2276`](#nga2276).

## NGA2245

A Section has no name of its own, so the `transform` block names it by one of
its Labels, as `module.label`, or `module.space.label` for a Label in a
Namespace. Raised where the Module has no Label of that name. Whether the Label
is exported does not matter.

```ngp
include "atari/800xl.ngp"
include "atari/disk.ngp"

modules { "main.asm" }

transform zx0 { main.titel }

container atr
```

```asm
.section
title   .byte "TITLE SCREEN"
.ends

.proc entry
        lda title
        rts
.endp
```

## NGA2246

`then` in a `phase` block declares an edge, and a `.transition` in a Module of
that Phase takes it. Raised where no Module the Phase needs holds a
`.transition` to the Phase named.

```ngp
modules { "intro.asm" "game.asm" }

phase intro { needs intro  then game  entry introStart }
phase game  { needs game              entry gameStart }
entry intro
```

```asm intro.asm
.proc introStart
        rts
.endp
```

```asm game.asm
.proc gameStart
        rts
.endp
```

Nothing enters `game`, and what only `game` needs is left out of the build.
Write the `.transition game` where `intro` ends, or remove `then game`.

## NGA2247

Raised with storage, where `transform` names a Section that no Transition
loads, so there is no Payload to decode. On an `.atr` such Sections are those
of a `resident` Module, which every edge keeps, and those of the entry Phase
when no edge leads back to it. A Section in a Pane is one too, wherever there
is storage, since the Container writes it into its Bank. The Container writes
them as they are.

```ngp
include "atari/800xl.ngp"
include "atari/disk.ngp"

modules { "main.asm" }

transform zx0 { main.title }

container atr
```

```asm
.section
title   .byte "TITLE SCREEN"
.ends

.proc entry
        lda title
        rts
.endp
```

Take the Section out of the `transform` block, or move it to a Module of a
Phase that a Transition enters.

## NGA2248

A Section is transformed once. Any Label of a Section names the whole Section,
so two Labels of one Section in `transform` blocks name it twice.

```ngp
include "atari/800xl.ngp"
include "atari/disk.ngp"

modules { "intro.asm" "level.asm" }

phase intro { needs intro  then level  entry introStart }
phase level { needs level              entry levelStart }
entry intro

transform zx0 { level.map, level.mapEnd }

container atr
```

```asm intro.asm
.proc introStart
        .transition level
.endp
```

```asm level.asm
.section
map     .byte "##########"
mapEnd
.ends

.proc levelStart
        lda map
@stop   jmp @stop
.endp
```

`mapEnd` stands at the end of the Section that `map` starts. List one Label
per Section.

## NGA2249

Without storage no edge loads anything: every Section is written once, when the
program loads. A `.xex` can run the decoder while the DOS loads it. No other
Container runs code while it is loaded, so nothing would decode the Section.
`copy` is accepted in any Container.

```ngp
modules { "main.asm" }

transform zx0 { main.title }
```

```asm
.section
title   .byte "TITLE SCREEN"
.ends

.proc entry
        lda title
        rts
.endp
```

Include the storage document beside the variant, such as `atari/disk.ngp` for
an `.atr`, build a `.xex`, or take the Section out of the block.

## NGA2250

`.implements SLOT, SYMBOL` names the Slot first. Raised where the first name is
defined and is not a Slot, as here, where the two names are the wrong way
round.

```asm
.slot draw, vector

.implements levelDraw, draw

.proc levelDraw
        rts
.endp
```

Write `.implements draw, levelDraw`. A first name that is not defined at all is
[`NGA2206`](#nga2206).

## NGA2251

The second name of `.implements` is a Label or a Section of the Module the
directive stands in. Raised where that Module defines no Label of that name. A
Label exported by another Module does not count.

```asm
.slot draw, vector
.export draw

.implements draw, levelDraw
```

```asm level.asm
.export levelDraw

.proc levelDraw
        rts
.endp
```

Move the `.implements` line into `level.asm`, beside the Proc.

## NGA2252

A Slot has one live Implementation in each Phase. Two Modules that implement it
in the same Phase leave the tool no single address to write into the Cell.

```ngp
modules { "main.asm" "level.asm" }

resident { main }

phase level { needs level  entry levelStart }
entry level
```

```asm
.slot draw, vector
.export draw

.implements draw, drawNothing

.proc drawNothing
        rts
.endp
```

```asm level.asm
.implements draw, drawMap

.proc drawMap
        rts
.endp

.proc levelStart
        jsr draw
        rts
.endp
```

A `resident` Module is live in every Phase, so a default there collides with
every Phase that fills the Slot. There is no order of precedence. Remove one
`.implements`, or fill the Slot in the Module of each Phase. The note
[`NGA2253`](#nga2253) points at the other Implementation.

## NGA2253

Follows [`NGA2252`](#nga2252) and points at the `.implements` of the other
Implementation live in that Phase.

## NGA2254

Raised where the program declares more than 255 Slots, counting every `.slot`
in every Module. Every Slot has a Cell, and a Frame counts the Cells it writes
in a byte. Use fewer Slots: a Phase can fill one `pointer` Slot with the
address of a table of its own, and the code reads the other addresses from that
table.

## NGA2255

An Implementation stands in no Pane. A call through the Cell does not show the
Pane, and would run whatever the Window shows at that moment.

```ngp
include "atari/130xe.ngp"

modules { "main.asm" }

panes in ext { level }
```

```asm
.slot draw, vector

.implements draw, levelDraw

.proc levelDraw, in level
        rts
.endp
```

Fill the Slot with a Proc outside the Pane that puts `.with level` over its
`jsr`, as `levelDraw` does in [Slots](../07-slots.md).

## NGA2260

`.endp then NAME` names a Proc of the same Module, by the Label at its start. A
Label in a `.section`, a Label inside a Proc, or a Proc of another Module is
refused.

```asm
.proc entry
        ldx #0
.endp then clear

.section
clear
        sta $0600,x
        rts
.ends
```

Make `clear` a `.proc`, or drop the `then` and end `entry` with `jmp clear`.
See [`.endp then NAME`](../16-assembler.md#endp-then-name).

## NGA2261

The Proc that `then` names is placed right after the Proc that names it, and
only one Proc can stand right before it.

```asm
.proc entry
        jsr fillScreen
        ldx #0
.endp then clear

.proc fillScreen
        ldx #$FF
.endp then clear

.proc clear
        rts
.endp
```

Keep one `then`, and end the other Proc with `jmp clear`.

## NGA2262

The Proc after `then` starts where the one before it ends, so its address
comes from the chain. An `at` or an `align` of its own would conflict with
that.

```asm
.proc entry
        ldx #0
.endp then clear

.proc clear, absolute align 256
        rts
.endp
```

Pin or align the first Proc of the chain instead.

## NGA2264

A chain of `then` that comes back to where it started has no first Proc to
place. It is reported once, at the `then` of the Proc of the cycle declared
first.

```asm
.proc entry
        jmp first
.endp

.proc first
        inx
.endp then second

.proc second
        iny
.endp then first
```

Drop one `then` and end that Proc with a `jmp` to the next.

## NGA2265

The Proc that `then` names stands in the bytes right after the first one. A
Pane's bytes are there only while the Pane is shown, so a chain cannot run from
a Pane into memory outside it, or the other way.

```ngp
include "atari/130xe.ngp"

modules { "main.asm" }

panes in ext { level }
```

```asm
.proc drawLevel, in level
        ldx #0
.endp then finish

.proc finish
        rts
.endp
```

Put both in the Pane, or drop the `then` and end `drawLevel` with
`jmp finish`.

## NGA2266

Where `.declare arg` or `.declare ret` writes a type, the `.ztemp` or `.temp`
under it reserves exactly that type's bytes: one for `u8`, `i8` and `bool`, two
for `u16` and `i16`, N for `u8[N]`.

```asm
.proc double
        .declare arg u16
value   .ztemp 1
        asl value
        rts
.endp
```

Reserve two bytes, or drop the type and let the reservation answer: one byte is
a `u8`. A place such as `ax` is held to its bytes by
[`NGA2334`](#nga2334).

## NGA2270

A Proc holds code, and a table the code reads may stand in it with `.byte`. A
variable may not.

```asm
.proc entry
        inc counter
        rts
counter .res 1
.endp
```

Declare it as `counter .ztemp 1` or `.temp`, which takes the Proc's scope, or
put it in a `.section`, which may be opened inside the Proc.

## NGA2271

A Temporary shares its addresses with other Temporaries while it is not active,
so a byte written into it when the program loads would not be there when it
runs. A `.section temporary` holds `.res` and nothing else: no data and no
instructions.

```asm
.section temporary
scratch
        .byte 0, 0
.ends
```

Set the values in code when the Section is entered, or put initialised data in
an ordinary `.section`.

## NGA2272

The names of the Target's Regions, registers, unit sets and Windows, and of
the Project's Panes, are visible in every Module, and no Module may define one
again, exported or not.
This catches a local copy of a register's address, which could drift from the
Target's.

```ngp
target {
  region $0080 .. $00FF ram
  region $2000 .. $BFFF ram
  register PORTB $D301
}

modules { "main.asm" }
```

```asm
PORTB = $D301

.proc entry
        lda #$FE
        sta PORTB
        rts
.endp
```

Delete the definition and use the Target's name. The note
[`NGA1163`](#nga1163) points at its declaration.

## NGA2273

A Section has no name of its own. Its Labels name it, in source, in the Project
and in the memory map, so a `.section` with no Label anywhere in it cannot be
named at all.

```asm
.section
        .byte 1, 2, 4, 8, 16, 32, 64, 128
.ends
```

Put a Label at its start.

## NGA2274

`.transform FORMAT LABEL` stands in the decoder's own Module. The declaration
exports the Label, so it needs no `.export` of its own.

```asm
.transform rle unpackRle

.proc unpack
        rts
.endp
```

Correct the name, or move the `.transform` into the decoder's Module.

## NGA2275

The Transition routine jumps to a decoder, so `.transform` names the Label it
starts at. A Constant, a macro or any other kind of Symbol is refused, and the
message says which kind it is.

```asm
.transform rle unpack

.macro unpack
        rts
.endm
```

Write the decoder as a `.proc` and name that.

## NGA2276

`.transform` declares a decoder for a format the tool encodes: `copy`, `rle`
or `zx0`. The tool would never write a Payload in any other format, so a
decoder for one would have nothing to decode.

```asm
.transform lz4 unpack

.proc unpack
        rts
.endp
```

The library's decoders are `stream/rle.asm` and `stream/zx0.asm`. The same name
in the Project's `transform` block is [`NGA2244`](#nga2244).

## NGA2277

Raised at a second `.transform` for one format, in the same Module or in
another. A Frame names a block's decoder by a number, one per format, so a
format has one decoder. The usual cause is a decoder of the program's own beside the one a
storage document lists, as `atari/disk.ngp` lists `stream/zx0.asm`.

```asm
.transform rle unpack
.transform rle unpackFast

.proc unpack
        rts
.endp

.proc unpackFast
        rts
.endp
```

Keep one of the two. The note, [`NGA2278`](#nga2278), points at the one the
tool kept.

## NGA2278

Follows [`NGA2277`](#nga2277) and points at the first `.transform` for the
format, which is the decoder the tool keeps.

## NGA2279

Raised where the program has storage, takes a Transition, and no Module
declares `.transform copy`. A Payload that no `transform` block names is moved
into place by the `copy` decoder. The driver declares it, because the driver
knows how its medium is best copied.

```ngp
storage { units 1  size 4096 }

modules { "main.asm" "drive.asm" }
resident { drive }

phase level { needs main  then level  entry start }
entry level
```

```asm
.proc start
        .transition level
.endp
```

```asm drive.asm
.driver open   driveOpen
.driver read   driveRead
.driver unit   driveUnit
.driver offset driveAt

.macro driveOpen
        nop
.endm

.macro driveRead
        lda #0
.endm

.section zeropage
driveAt   .res 2
driveUnit .res 1
.ends
```

Add `.transform copy` to the driver, with a Label of its own that does the
copying, as every driver in the library does.

## NGA2280

Raised for a `transform` block of the Project whose format no Module decodes.
The tool encodes the format, and a Module of the program decodes it: a
`.transform` line binds the format to a Label of that Module. The library
holds `stream/rle.asm` and `stream/zx0.asm`.

```ngp
storage { units 1  size 4096 }

modules { "main.asm" }

transform rle { main.tiles }
```

```asm
.section
tiles   .byte 0, 0, 0, 0, 1, 1, 1, 1
.ends
```

List the decoder in `modules` and in `resident`, as `"stream/rle.asm"` and
`rle`. A storage document of the library lists `stream/zx0.asm` already. A
`.xex` without storage needs neither, because the tool adds the library's
decoder to the code that runs while the file loads.

## NGA2281

Raised when a `.transition` is taken in a program with storage and no Module
declares `.driver`. A Transition reads its Frame and its Payloads from storage,
and only the driver knows how to read it. Without a `storage` block a
Transition loads nothing and needs no driver.

```ngp
storage { units 1  size 4096 }

modules { "main.asm" }

phase level { needs main  then level  entry start }
entry level
```

```asm
.proc start
        .transition level
.endp
```

Include the machine's storage document, which lists its driver:
`atari/disk.ngp` beside `atari/800xl.ngp`, for one. A Project that declares its
own `storage` lists a driver Module of its own too, in `modules` and in
`resident`.

## NGA2282

Raised at the second Module that declares `.driver`. A program has one driver,
which the Transition routine, the decoders and every `.with` use. Usually two
documents each list one, such as `atari/roms.ngp` beside `atari/disk.ngp`.

```asm
.driver init setup

.macro setup
        cld
.endm
```

```asm roms.asm
.driver init begin

.macro begin
        sei
.endm
```

Keep one driver, and give it every role the program needs.
[`NGA2283`](#nga2283) points at the one the tool kept.

## NGA2283

Follows [`NGA2282`](#nga2282) and points at the first `.driver` line of the
driver the tool kept.

## NGA2284

Raised at a `.driver` line whose first word is not a role. The roles are the
fixed set the tool calls, so any other word would declare something nothing
uses. They are case-sensitive: `showat` is not `showAt`.

```asm
.driver write driveWrite

.macro driveWrite
        sta $D301
.endm
```

## NGA2285

Raised where the driver lacks a role the tool calls. A driver of storage needs
`open`, `read`, `unit` and `offset`: the routine writes a position into the
two cells, then opens the stream and reads it. A driver that declares a `rom`
needs `select`, which switches the ROMs on every edge. `init`, `finalize`,
`mask` and `unmask` are optional.

```ngp
storage { units 1  size 4096 }

modules { "main.asm" }
```

```asm
.driver open   driveOpen
.driver unit   driveUnit
.driver offset driveAt

.macro driveOpen
        nop
.endm

.section zeropage
driveAt   .res 2
driveUnit .res 1
.ends
```

Declare the missing role. A driver that only switches ROMs, in a program
without storage, declares none of the stream's roles.

## NGA2286

Raised where a `.driver` line names something its own Module does not define.
A role belongs to the driver: the macro, or the Label of a cell, stands in the
Module that says `.driver`. One that another Module exports does not count.

```asm
.driver init driveInit
```

```asm setup.asm
.export driveInit

.macro driveInit
        cld
.endm
```

Move the definition into the driver's Module.

## NGA2287

`as` names the Proc whose Temporaries hold the arguments of every Proc that
joins it. Raised where the name is something else, such as the table that
holds the handlers' addresses, and where `as` stands on a `.section`, which
takes no arguments.

```asm
.section
handlers .word 0
.ends

.proc onFire, as handlers
        rts
.endp
```

Name the Proc that is the type. A type declared in C as
`typedef u8 chooser(u8 x);` is joined with `as chooser`; see
[Function types](../17-c-dialect.md#function-types).

## NGA2288

Raised where a Proc that says `as` also declares an argument or a result with
`.declare`. A member reads its arguments from the type's Temporaries, so that
a direct call and a call through a pointer write the same bytes.

```asm
.proc handler
ptr     .ztemp 2
key     .ztemp 1
        jmp (ptr)
.endp

.proc onKey, as handler
        .declare arg u8
pressed .ztemp 1
        lda pressed
        rts
.endp
```

Drop the `.declare` and the `.ztemp` under it, and read `handler.key`.

## NGA2289

Raised where the driver's Module is not in memory in every Phase. Every
Transition calls the driver, whichever Phase it leaves.

```ngp
modules { "main.asm" "game.asm" "drive.asm" }

phase intro { needs main, drive  then game  entry start }
phase game  { needs game                    entry play }
entry intro
```

```asm
.proc start
        .transition game
.endp
```

```asm game.asm
.proc play
        rts
.endp
```

```asm drive.asm
.driver init driveInit

.macro driveInit
        cld
.endm
```

List the Module in `resident`, as the library's variants and storage documents
list their drivers.

## NGA2290

Raised where a Module that declares `.transform` is not in memory in every
Phase. Any Transition may load a Payload in that format, so its decoder has to
be there whichever Phase the Transition leaves.

```ngp
modules { "main.asm" "game.asm" "unpack.asm" }

phase intro { needs main, unpack  then game  entry start }
phase game  { needs game                     entry play }
entry intro
```

```asm
.proc start
        .transition game
.endp
```

```asm game.asm
.proc play
        rts
.endp
```

```asm unpack.asm
.transform rle unpack

.proc unpack
        rts
.endp
```

List the Module in `resident`.

## NGA2291

Raised at the second `.driver` line for one role. A role is declared once:
`show` and `showAt` once per Window, and `rom` once per Module it names.

```asm
.driver init driveInit
.driver init driveReset

.macro driveInit
        cld
.endm

.macro driveReset
        sei
.endm
```

Delete one. Where both bodies are needed, put them in one macro.

## NGA2292

Raised for a `.xex` without storage, where a Section named in `transform`
stands in a Pane or in a unit set. Such a Section is written into its Bank
while the file loads, and no decoder runs on it there.

```ngp
include "atari/130xe.ngp"

modules { "main.asm" }

panes in ext { level }

transform zx0 { main.levelData }

container xex
```

```asm
.section in level
levelData
        .byte 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
.ends

.proc entry
        .with level
        lda levelData
        rts
.endp
```

Take the Section out of the `transform` block.

## NGA2293

Raised where a `.driver` line has the wrong number of names after the role.
`show` and `showAt` take a Window and a macro, and `showAt` may add `x` or
`xy`, the registers it takes the state in. `rom` takes a Module and may add a
second whose ranges it shows too. Every other role takes one name.

```asm
.driver show ext

.macro driveShow state
        ldx #state
        stx $D301
.endm
```

## NGA2294

Raised where a `.driver` line names a Window the Target does not declare. A
driver is written for one machine and names that machine's Windows, so this
usually means a driver of another machine, or a misspelled name.

```asm
.driver show ext driveShow

.macro driveShow state
        ldx #state
        stx $D301
.endm
```

Name a Window that a `window` line of the Target declares; see
[`window`](../15-project-file.md#window).

## NGA2295

Raised where the Target declares a Window and the driver has no `show` or no
`showAt` for it. A driver shows every Window the Target declares: a `.with`
expands `show` where the state is known when the program is built, and
`showAt` where it is in `X`.

```ngp
target {
  units  extra 4
  window ext $4000 .. $7FFF  views extra
}

modules { "main.asm" }
```

```asm
.driver show ext driveShow

.macro driveShow state
        ldx #state
        stx $D301
.endm
```

Declare the missing role for that Window.

## NGA2296

Raised where storage is the Banks of a unit set, `storage { units NAME }`, and
the driver declares no `stream`. Those Banks are read through a Window that
shows them, and `.driver stream WINDOW` says which.

```ngp
target {
  units  extra 4
  window ext $4000 .. $7FFF  views extra
}

storage { units extra }

modules { "main.asm" }
```

```asm
.driver open   driveOpen
.driver read   driveRead
.driver unit   driveUnit
.driver offset driveAt
.driver show   ext driveShow
.driver showAt ext driveShowAt

.macro driveOpen
        nop
.endm

.macro driveRead
        lda #0
.endm

.macro driveShow state
        ldx #state
        stx $D301
.endm

.macro driveShowAt
        stx $D301
.endm

.section zeropage
driveAt   .res 2
driveUnit .res 1
.ends
```

Add `.driver stream` with the Window whose `views` lists the unit set, here
`.driver stream ext`. See [`storage`](../15-project-file.md#storage).

## NGA2297

Raised where `.driver stream` names a Window that does not show storage.
Storage that is a unit set is read through a Window whose `views` lists that
set. Storage declared by count, `units N  size S`, is read without a Window,
and its driver declares no `stream`, as `atari/disk.asm` does.

```ngp
target {
  units  extra 4
  window ext $4000 .. $7FFF  views extra
}

storage { units 16  size 4096 }

modules { "main.asm" }
```

```asm
.driver open   driveOpen
.driver read   driveRead
.driver unit   driveUnit
.driver offset driveAt
.driver stream ext
.driver show   ext driveShow
.driver showAt ext driveShowAt

.macro driveOpen
        nop
.endm

.macro driveRead
        lda #0
.endm

.macro driveShow state
        ldx #state
        stx $D301
.endm

.macro driveShowAt
        stx $D301
.endm

.section zeropage
driveAt   .res 2
driveUnit .res 1
.ends
```

Name the Window that shows storage's unit set. For storage by count, delete
the `stream` line.

## NGA2298

Raised where `nga.show` or `nga.showAt` does not begin with a Window. The
driver declares these two roles once per Window, so the Window picks the macro,
and the arguments after it are the macro's.

```ngp
include "atari/130xe.ngp"

modules { "main.asm" }

container xex
```

```asm
.proc entry
        nga.show 2
        rts
.endp
```

Write the Window first: `nga.show ext, 2`. In a program, a
[`.with`](../16-assembler.md#with) is usually what is wanted, since it also
checks what the statement under it reaches.

## NGA2299

Raised where a Window's name stands as a value. The name is for `.with` and
for the driver's roles, and stands for neither the Window's address nor a
state of it.

```ngp
include "atari/130xe.ngp"

modules { "main.asm" }

container xex
```

```asm
.proc entry
        lda #<ext
        rts
.endp
```

For the address, declare a Constant, `extWindow = $4000`. For a state, write a
Pane's name. For the number of Banks, write the unit set's name.

## NGA2300

Raised at a `.macro` whose name is a mnemonic. A use would be read as the
instruction, so the macro could never be reached. Every mnemonic the tool
knows counts, so a 65SC02 instruction such as `phx` cannot be a macro on a
6502 Target either.

```asm
.macro phx
        txa
        pha
.endm
```

Give the macro another name, such as `pushX`.

## NGA2301

Raised where the header of a macro, or the pattern of a `.case`, lists one name
twice. The body could not tell which argument the name means.

```asm
.macro copy from, from
        lda from
        sta to
.endm
```

## NGA2302

Raised where a macro parameter has the name of a Symbol its Module defines.
Inside the body the name would mean the argument and outside it the Symbol,
and a reader of the body could not tell which was meant. A name a `.case`
binds is held to the same rule.

```asm
count = 8

.macro clear at, count
        lda #0
        ldx #count - 1
@loop   sta at,x
        dex
        bpl @loop
.endm
```

Rename the parameter.

## NGA2303

Raised where the first word of a statement is no instruction and no macro the
Module can see. Mnemonics are lowercase, so `LDA` is a name like any other. A
macro of another Module is seen only where that Module exports it with
`.export`. For a qualified name, the Namespace exists and holds no such macro.

```asm
.proc entry
        lad #0
        rts
.endp
```

## NGA2304

Raised where an instruction's operand has a comma followed by something other
than `x` or `y`. That makes a list, and only a macro takes one. Index registers
are lowercase, so `table,X` is a list of two values.

```asm
.section
table   .byte 1, 2, 3, 4
.ends

.proc entry
        ldx #0
        lda table,X
        rts
.endp
```

## NGA2305

Raised where a macro is used with an argument shaped like an operand: `#value`,
`at,x`, or `(ptr),y`. An argument is an expression, and the body decides how it
is used.

```asm
.macro background color
        lda #color
        sta $D01A
.endm

.proc entry
        background #$94
        rts
.endp
```

Drop the `#`, since the body writes `lda #color` already. For an indexed
address, pass the address and write the index in the body.

## NGA2306

Raised where a macro use gives more or fewer arguments than the macro has
parameters. A macro whose last parameter is a pack takes at least the names
before it, and is [`NGA2331`](#nga2331) instead.

```asm
.macro poke at, value
        lda #value
        sta at
.endm

.proc entry
        poke $D01A
        rts
.endp
```

## NGA2308

Raised where a macro's name stands as an operand, usually after `jsr` or
`jmp`. A macro has no address, because its body is copied into each use. The
driver's roles, such as `nga.read`, are macros too.

```asm
.macro clear at
        lda #0
        sta at
.endm

.proc entry
        jsr clear
        rts
.endp
```

Write the name where an instruction stands, `clear $D01A`, or make it a
`.proc` where a call is wanted.

## NGA2309

Raised where a `.driver` role names a Label or a Constant instead of a macro.
The tool expands a role at each place it uses it, so a role is a macro even
when its body is one `jsr`. The cells `unit` and `offset` are the exception:
they name Labels.

```asm
.driver init driveInit

.proc driveInit
        cld
        rts
.endp
```

Keep the Proc, and name in `.driver` a macro whose body is a `jsr` to it, as
the library's drivers do.

## NGA2310

Raised where a Module defines `nga`, as a Symbol or by opening
`.namespace nga`. The name is the tool's: the driver's roles are reached
through it, as `nga.read` and `nga.show`.

```asm
.namespace nga
width = 40
.endns
```

## NGA2311

Raised where a macro use is qualified by a name that is no Namespace. A
Module's name is not a Namespace: a macro that `print.asm` exports is used as
`line`, not `print.line`. A Namespace is opened with `.namespace`.

```asm
.proc entry
        print.line 3
        rts
.endp
```

```asm print.asm
.export line

.macro line n
        ldx #n
        stx $0600
.endm
```

## NGA2312

Raised for `nga.NAME` where the tool defines no such name. The tool's
Namespace holds the driver's roles, `nga.open`, `nga.read`, `nga.init`,
`nga.select`, `nga.mask`, `nga.unmask`, `nga.finalize`, `nga.show` and
`nga.showAt`, and the two cells of its stream, `nga.unit` and `nga.offset`.

```asm
.proc entry
        nga.close
        rts
.endp
```

## NGA2313

Raised where code uses a role or a cell of the driver, such as `nga.read` or
`nga.unit`, or a `.with`, and the program has no driver. For the roles it is
reported once, at the first use. It also follows a driver the tool refused,
such as one missing a role, [`NGA2285`](#nga2285). `nga.select`, `nga.mask`,
`nga.unmask` and `nga.finalize` are never reported: without a driver they
expand to nothing.

```asm
.proc entry
        nga.read
        sta $0600
        rts
.endp
```

Include the machine's storage document, which lists its driver, or list a
driver Module of your own.

## NGA2314

Raised where a Module defines a name the tool already gives every Module, such
as a Constant of the Project's `constants` block. In that Module its own
definition would win, and every other Module would see the Project's value.

```ngp
constants { LEVELS = 8 }

modules { "main.asm" }
```

```asm
LEVELS = 3

.proc entry
        lda #LEVELS
        rts
.endp
```

Delete one of the two definitions, or rename one. [`NGA2315`](#nga2315) points
at the Project's.

## NGA2315

Follows [`NGA2314`](#nga2314) and points at the line of the Project file that
defines the name.

## NGA2316

Raised where the name of a Namespace stands as a value. A Namespace is a prefix
on the names inside it and has no value of its own.

```asm
.namespace sound
volume = 8
.endns

.proc entry
        lda #sound
        rts
.endp
```

Name the member: `lda #sound.volume`.

## NGA2317

Raised at a Symbol whose name is also a Namespace. A dotted name after a Label
reads an attribute of its Section, so `player.x` would mean two things. The
Label of a `.proc` is the exception, since a Proc is a scope of that name
itself.

```asm
.section
player  .res 4
.ends

.namespace player
x = 0
y = 1
.endns
```

Rename one of the two.

## NGA2319

Raised where a name inside a Proc is one of a Section's attributes:
`runtimeSectionAddress`, `runtimeSectionSize`, `resident`,
`linearSectionAddress`, `linearFamilyAddress`, `storagePayloadAddress` or
`storagePayloadSize`. From outside, `loader.resident` is the Proc's attribute,
and a name inside it would take the same spelling.

```asm
.proc loader
resident .ztemp 1
        lda resident
        rts
.endp
```

Rename it. See [Attribute access](../16-assembler.md#attribute-access).

## NGA2321

Raised where an `.if` condition names a position or a size: a Label, or an
attribute such as `runtimeSectionSize`. In a macro body this includes a
parameter whose argument is one. The condition must be a
[declared value](../16-assembler.md#declared-values), made of literals and of
Constants made of literals.

```asm
.section
table   .byte 1, 2, 3
.ends

.proc entry
.if table.runtimeSectionSize > 2
        ldx #2
.endif
        rts
.endp
```

To check a size or an address, write `.assert`, which is evaluated after Place.

## NGA2322

Raised in the Expand Step when a macro uses itself, directly or through other
macros, more than 1024 uses deep. The tool takes that as a recursion whose
condition never turns false. Usually the argument that should bring it to an
end is passed on unchanged: here `zeros n` was meant to be `zeros n - 1`.

```asm
.macro zeros n
.if n > 0
        .byte 0
        zeros n
.endif
.endm

.section
table   zeros 4
.ends
```

The finding stands at the use inside the body, and the note
[`NGA2323`](#nga2323) at the use where the expansion began. Nothing more of
that use is expanded. A recursion that ends but produces too much is
[`NGA2332`](#nga2332).

## NGA2323

Follows [`NGA2322`](#nga2322), [`NGA2332`](#nga2332) and
[`NGA2333`](#nga2333), and a finding about a `.with` written in a macro body,
such as [`NGA3008`](#nga3008). The finding stands in the body, which may be used
in many places. The note points at the use in a Section or a Proc where this
expansion began.

## NGA2324

A name where a mnemonic stands is a macro use, and the name is looked up as any
name is, this Module's own Symbols first. Raised when what it finds is a
Label, a Constant or any other Symbol that is not a macro. The usual cause is a
call written without its `jsr`.

```asm
.proc clear
        lda #0
        sta $0600
        rts
.endp

.proc entry
        clear
        rts
.endp
```

A Symbol of another kind is not skipped in favour of a macro of the same name
further out, so `lda fill` and `fill 1, 2` in one Module never mean two
different `fill`s. Rename one of them. A name that is nothing at all is
[`NGA2303`](#nga2303).

## NGA2325

`NAME...` as an item of a list spreads a pack: its elements stand in its place.
Raised where `NAME` is not a pack: a fixed parameter of the macro, a fixed name
of a `.case`, or any name outside a macro body. Usually the parameter was
declared without its dots.

```asm
.macro table values
        .byte values, values...
.endm
```

Write the last parameter as `values...` on the `.macro` line. Only the last
parameter may be a pack. See [Packs](../16-assembler.md#packs).

## NGA2326

`.match NAME` chooses a `.case` by how many elements the pack `NAME` has.
Raised where `NAME` is not a pack the body binds: a fixed parameter, a fixed
name of an enclosing `.case`, or a name the body does not bind at all.

```asm
.macro poke at, values
.match values
.case first, rest...
        lda #first
        sta at
        poke at + 1, rest...
.case
.endmatch
.endm
```

Declare the parameter as a pack, `values...`. To choose by a value rather than
by a count, use `.if`.

## NGA2327

The names of a `.case` are new names for the pack's elements. Raised where one
of them is already bound around the `.case`: a parameter of the macro, or a
name of the `.case` of an enclosing `.match`. Inside the case the name would
mean one thing and just outside it another. Another `.case` of the same
`.match` is not around it, and may bind the same names.

```asm
.macro store at, value, rest...
        lda #value
        sta at
.match rest
.case value, more...
        store at + 1, value, more...
.case
.endmatch
.endm
```

Give the case a name of its own, such as `next`.

## NGA2328

Raised at a use where the pack has a number of elements that no `.case` of a
`.match` fits. It is reported once per use, and the note
[`NGA2329`](#nga2329) points at the `.match`. In a recursion that takes an
element off each turn, the usual cause is a missing `.case` with no names,
which fits the empty pack and is where the recursion stops.

```asm
.macro words values...
.match values
.case first, rest...
        .word first
        words rest...
.endmatch
.endm

.section
limits  words 100, 2000
.ends
```

Here the third use gets an empty pack. Add an empty `.case`, or a case for the
count the use gives. How a case fits is in [Packs](../16-assembler.md#packs).

## NGA2329

Follows [`NGA2328`](#nga2328) and points at the `.match` whose cases the pack
did not fit.

## NGA2330

`.match` keeps the first `.case` that fits. Raised at a `.case` that an earlier
case always fits first, so it is never taken: after `.case first, rest...`,
which fits one element or more, `.case last` cannot be reached. Raised where the
macro is defined, whether it is used or not.

```asm
.macro words values...
.match values
.case first, rest...
        .word first
        words rest...
.case last
        .word last
.case
.endmatch
.endm
```

Put the narrower case first, or remove it.

## NGA2331

A macro whose last parameter is a pack takes at least as many arguments as it
has names before the pack. The pack takes the rest, none included. Raised at a
use that gives fewer.

```asm
.macro poke at, first, rest...
        lda #first
        sta at
.match rest
.case next, more...
        poke at + 1, next, more...
.case
.endmatch
.endm

.proc entry
        poke $0600
        rts
.endp
```

A macro without a pack takes exactly its parameters, and a use that gives
another count is [`NGA2306`](#nga2306).

## NGA2332

Raised when one use expands to more than 65536 statements, the number of bytes
in the address space: an expansion that size could not be placed if each
statement emitted a single byte. The depth limit of [`NGA2322`](#nga2322) does
not catch it. A macro that uses itself twice per level ends at a small depth,
after a number of uses that doubles with each level.

```asm
.macro tree n
.if n > 0
        .byte n
        tree n - 1
        tree n - 1
.endif
.endm

.section
nodes   tree 20
.ends
```

The note [`NGA2323`](#nga2323) points at the use where the expansion began.
Write the recursion with one use per level.

## NGA2333

An argument with a declared value is put in as that value, and one that adds or
subtracts a declared value is carried as one sum. Any other argument is carried
as its expression, so a recursion that passes on `at + step`, where `step` is
not a declared value, nests one level deeper at every use. Raised when the
expression passes the 64 levels it may nest.

```asm
.section
screen  .res 4000
.ends

.section
line    .res 40
.ends

.macro clear n, at
.if n > 0
        sta at
        clear n - 1, at + line.runtimeSectionSize
.endif
.endm

.proc entry
        lda #0
        clear 100, screen
        rts
.endp
```

Make the step a declared value, such as `LINE = 40`, and `at + LINE` stays one
sum however deep the recursion goes. See
[Declared values](../16-assembler.md#declared-values).

## NGA2334

A place on `.declare` puts some bytes of an argument or a result in registers,
and the bytes written `m` in the reservation below it. Raised where the
reservation does not hold exactly the `m` bytes. `am` is one byte in `A` and one
in memory, so its `.ztemp` is one byte.

```asm
.proc plot
        .declare arg am
column  .ztemp 2
        lda column
        rts
.endp
```

Reserve only what the place keeps in memory. The same rule for a type, such as
`u16` over a reservation of one byte, is [`NGA2266`](#nga2266). See
[`.declare`](../16-assembler.md#declare).

## NGA2335

`.driver seek` is a word about the medium and not a role. `forward` is the only
one: it says the driver reaches an offset only by reading up to it, as the
Lynx's cartridge does. Raised for any other word.

```asm
.driver seek random
```

A driver that reaches any offset at no cost, such as a sector in a buffer or a
Bank in a Window, declares no `seek`. With `forward`, the images a
`.transition NAME, fast` opens begin where a unit does. The other word of this
kind is `span`, [`NGA2426`](#nga2426).

## NGA2412

Raised where code names a Label of a Pane's Section and does not run with that
Pane shown: it is outside the Pane, has no `.with` over the statement, and is
not in a Proc declared `under` the Pane. Outside a `.with`, what a Window shows
is undefined. The address is refused as well as the access, so `#<levelMap` is
refused as `lda levelMap` is.

```ngp
include "atari/130xe.ngp"

modules { "main.asm" }

panes in ext { level }
```

```asm
.section in level
levelMap
        .byte 1, 2, 3
.ends

.proc entry
        lda levelMap
        sta $0600
        rts
.endp
```

Put `.with level` on the line above the statement, or put the code in a Proc
declared `.proc NAME, under level`. In C, write `[[with(level)]]` over the
block or `[[under(level)]]` on the function. Under a `.with` that shows
another Pane the finding is [`NGA3008`](#nga3008), and for a Pane pinned to a
named state it is [`NGA3011`](#nga3011). See
[Panes](../06-panes.md#who-may-name-what).

## NGA2414

A Section is in memory only in the Phases that need its Module. Raised where
code present in a Phase names a Section that is not in memory in that Phase:
there the address holds whatever that Phase put in it. It is reported for the
first such Phase only.

```ngp
modules { "title.asm" "game.asm" }

phase title { needs title  then game }
phase game  { needs game   entry play }
entry title
```

```asm title.asm
.proc entry
        lda #0
        sta score
        .transition game
.endp
```

```asm game.asm
.export score

.section
score   .res 1
.ends

.proc play
        inc score
        rts
.endp
```

Put what both Phases use in a Module both need, or in `resident`. Where each
Phase has its own version of a routine, the answer is a [Slot](../07-slots.md).
In a program without storage the bytes a Section emits stay where they are, and
may be read and written in every Phase from which one that needs them can be
reached. A reservation has no bytes, and a call or a jump is always held to the
Phases that need its Module.

## NGA2415

A Slot is reached through a Cell that each Transition writes. Raised at a use
of a Slot by code that is present in a Phase where no Module implements it.
The Cell there holds what the last edge wrote, which points at memory this
Phase does not hold. It is reported for the first such Phase, once per use.

```ngp
modules { "hud.asm" "title.asm" "game.asm" }
resident { hud }

phase title { needs title  then game }
phase game  { needs game   entry play }
entry title
```

```asm hud.asm
.export draw, frame

.slot draw, vector

.proc frame
        jsr draw
        rts
.endp
```

```asm title.asm
.proc entry
        jsr frame
        .transition game
.endp
```

```asm game.asm
.implements draw, gameDraw

.proc gameDraw
        rts
.endp

.proc play
        jsr frame
        rts
.endp
```

Implement the Slot in that Phase too, with a Proc that only returns where there
is nothing to do, or keep the use out of the Phase. A Phase that neither uses
nor implements a Slot needs nothing.

## NGA2416

From outside, a Proc is entered at its start, so the tool knows every place
control comes in. Raised where a Reference from another Section goes further in
than that allows: a call or a jump past the start, a write anywhere but into
the operand of an instruction that does not jump or into an opcode with `.alt`,
or the address of any other byte taken. A read may go anywhere.

```asm
.proc clear
        lda #0
fill    sta $0600,x
        dex
        bne fill
        rts
.endp

.proc entry
        lda #$20
        ldx #40
        jsr clear.fill
        rts
.endp
```

Make the second way in a Proc of its own and chain the first to it with
`.endp then fill`, so that `clear` falls through into it. Code that is jumped
into the middle of belongs in a `.section`. See
[A Proc has one entry](../16-assembler.md#a-proc-has-one-entry).

## NGA2417

A branch reaches 128 bytes back and 127 forward, so its target must be at a
distance the program fixes. Raised for a Bcc or a Jcc to a Label in another
Section, which the solver may place anywhere, unless the target is the start of
a Proc chained to this one by `then`.

```asm
.proc entry
        lda $0600
        beq idle
        sta $0601
        rts
.endp

.proc idle
        rts
.endp
```

Use `jmp`, under a branch of the opposite sense where the jump is conditional,
or chain the two Procs with `.endp then idle` so that the solver places them
together. See [Branch reach](../16-assembler.md#branch-reach).

## NGA2418

`in NAME` on a `.section` or a `.proc` names a Pane of the Project's `panes`
block, a Region of RAM or ROM, or a unit set. `under NAME` on a Proc names a
Pane. Raised where `NAME` is none of those, usually because the Pane is not
declared in the Project yet, or is spelt differently there.

```asm
.section in level
levelMap
        .byte 1, 2, 3
.ends
```

Declare the Pane in the Project, as in `panes in ext { level }`. A Region of
registers or reserved memory is [`NGA2442`](#nga2442). See
[`panes`](../15-project-file.md#panes).

## NGA2420

A Section in a Pane stands in its Window, and no Window covers the zero page.
Raised for a Section declared both `in` a Pane and `zeropage`.

```ngp
include "atari/130xe.ngp"

modules { "main.asm" }

panes in ext { level }
```

```asm
.section in level, zeropage
levelPtr
        .res 2
.ends
```

Keep the zero-page variable out of the Pane: a `.ztemp` of the code that uses
it, or a `zeropage` Section of its own. A Section in a Pane is `absolute`
without saying so.

## NGA2421

A Pane's value is the index of the state the solver gave it, known only after
Place. It may stand as an immediate operand, an item of `.byte` or `.word`, or
the argument of a driver's role, and `<` and `>` may take it apart. The one
arithmetic allowed on it is `FAMILY + n`. Raised for any other operator, a
comparison in an `.assert` included. A common case is computing the hardware's
value for a state, which is the driver's work.

```ngp
include "atari/130xe.ngp"

modules { "main.asm" }

panes in ext { level }
```

```asm
.proc entry
        lda #level << 2 | $E3
        sta PORTB
        rts
.endp
```

Show the Pane with `.with level`, or load its state with `ldx #level` and hand
it to the driver with `.with ext, x`. See
[Where a state number comes from](../16-assembler.md#where-a-state-number-comes-from).

## NGA2422

`FAMILY + n` names member `n` of a family declared `NAME[N]`, and `n` is a
declared value from zero to `N`. `N` itself is allowed as the end of a range,
one past the last member. Raised for anything beyond that, or for an `n` that
is not a declared value. A single Pane counts as a family of one. Under
`.with`, which shows a member, `N` is refused too, since no Bank holds it.

```ngp
include "atari/130xe.ngp"

modules { "main.asm" }

panes in ext { rooms[2] }
```

```asm
.section
roomStates
        .byte rooms, rooms + 1, rooms + 2, rooms + 3
.ends
```

Here `rooms + 2` is the end of the range and passes, and `rooms + 3` is
refused. Other arithmetic on a Pane is [`NGA2421`](#nga2421).

## NGA2423

A Pane's name is a number, the state of its Window, and not the address of
anything. Raised where it stands as an operand that reaches memory, usually
`lda level` where `lda #level` was meant.

```ngp
include "atari/130xe.ngp"

modules { "main.asm" }

panes in ext { level }
```

```asm
.section
shown   .res 1
.ends

.proc entry
        lda level
        sta shown
        rts
.endp
```

Write the operand as an immediate. To read what the Pane holds, name one of its
Labels under `.with level`.

## NGA2424

`.dispatch` goes to positions of the Section it stands in, which is what keeps
that Section's flow known to the tool. Raised for a target that is not one: a
Label of another Section or Proc, or a Slot.

```asm
.proc entry
        lda $0600
        .dispatch @walk, idle
@walk   inc $0601
        rts
.endp

.proc idle
        rts
.endp
```

Name a position of this Proc instead, one that jumps on: `@idle jmp idle`. A
table that reaches other Sections is a table of addresses under `.own`, and the
Section that jumps through it then has no known flow. See
[One of its own positions](../12-instructions.md#one-of-its-own-positions).

## NGA2425

A `.dispatch` chooses by the value in `A`, which is one byte, so no value
reaches a target past the 256th. Raised for a statement of more than 256
targets, counting every row of it. The tool writes `jmp (abs,x)` where the
processor has it and the targets are 128 or fewer, and the `rts` trick, which
reaches 256, otherwise. Where the choice is wider than a byte, choose on the
high byte with one `.dispatch`, and on the low byte with another at each of its
targets.

## NGA2426

`.driver span` is a word about the medium and not a role. `none` is the only
one: it promises the driver that no image and no Frame runs from one unit of
storage into the next, so the driver need not know which unit follows which.
`pet/disk.asm` says it, since a diskette's tracks hold different numbers of
sectors. Raised for any other word.

```asm
.driver span sectors
```

A driver that carries an offset into the next unit, as the others the tool
ships do, declares no `span`. With `none`, every image has to fit in one unit,
or the placement refuses it with [`NGA5255`](#nga5255). The other word of this
kind is `seek`, [`NGA2335`](#nga2335).

## NGA2427

`.driver unit` and `.driver offset` name the two cells the Transition routine
writes a position into before it expands `open`. Each is a Label of the
driver's Module over a reservation, usually in the zero page. Raised where the
name is a Constant, a macro or any other kind of Symbol.

```asm
.driver open    myOpen
.driver read    myRead
.driver unit    myUnit
.driver offset  myAt

myUnit = $80

.macro myOpen
        nop
.endm

.macro myRead
        lda #0
.endm

.section zeropage
myAt    .res 2
.ends
```

Reserve the cell in a `.section zeropage` of the driver, as `myAt` is. How big
each must be is [`NGA2428`](#nga2428).

## NGA2428

The routine writes a unit's number into the `unit` cell, and an offset of two
bytes, the low one first, into the `offset` cell. Raised where the reservation
under either Label is another size, or the Label stands over no reservation. A
unit's number is one byte for up to 256 units of storage and two above that.
Without storage nothing writes the `unit` cell, and its size is not checked.

```asm
.driver open    myOpen
.driver read    myRead
.driver unit    myUnit
.driver offset  myAt

.macro myOpen
        nop
.endm

.macro myRead
        lda #0
.endm

.section zeropage
myUnit  .res 1
myAt    .res 1
.ends
```

Reserve two bytes for the offset. A driver for machines with different counts
sizes the unit's cell from the unit set, as `atari/mcel.asm` does with
`celUnitBytes = 1 + ( sdram > 256 )`.

## NGA2429

`.driver showAt WINDOW NAME` may end with a word saying where the macro takes
the state: `x`, which is what saying nothing means, for one byte in `X`, or
`xy` for two bytes, the high one in `Y`. Raised for any other word.

```ngp
target {
  window extra $4000 .. $7FFF  views off, on
}

modules { "main.asm" }
```

```asm
.driver show    extra extraShow
.driver showAt  extra extraShowAt a

.macro extraShow state
        lda #state
        sta $D500
.endm

.macro extraShowAt
        sta $D500
.endm
```

The tool hands the state over in `X`, or in `X` and `Y`, and nowhere else. Write
the macro to read `X`, and drop the word or write `x`.

## NGA2430

A Window has a state for each of its named states and one for each Bank of the
unit sets it shows. Raised where a Window has more than 256 states and the
driver's `showAt` for it takes one byte, which cannot hold every state.

```ngp
target {
  units  sdram 512
  window extra $4000 .. $7FFF  views sdram
}

modules { "main.asm" }
```

```asm
.driver show    extra extraShow
.driver showAt  extra extraShowAt

.macro extraShow state
        ldx #<state
        ldy #>state
        extraShowAt
.endm

.macro extraShowAt
        stx $D500
        sty $D501
.endm
```

End the declaration with `xy`, and the macro takes the high byte in `Y`. A
`.with` that hands the state in registers then writes `, xy`,
[`NGA2431`](#nga2431). A driver may say `xy` for a smaller Window too, and is
then handed a zero in `Y`.

## NGA2431

`.with WINDOW, x` and `.with FAMILY, x` hand the state to the driver in `X`, and
`, xy` hands it in `X` and `Y`, the high byte in `Y`. The form must be the one
the driver's `showAt` for that Window declares. Raised where they differ. On
MariaCEL, `cel8000` shows 2047 Banks, so its driver takes `xy`.

```ngp
include "atari/mariacel.ngp"

modules { "main.asm" }
```

```asm
.proc entry
        ldx #3
        .with cel8000, x
        lda $8000
        rts
.endp
```

Load the high byte as well, `ldy #0`, and write `.with cel8000, xy`. Where the
driver takes `x`, write `, x`.

## NGA2433

`.driver rom` names a Module the driver can switch out. Each is one bit of the
set that `select` is handed in `A`, in the order the lines are written. Raised
where a driver declares more than eight.

```asm
.driver rom     osrom
.driver rom     basic
.driver rom     cart1
.driver rom     cart2
.driver rom     cart3
.driver rom     cart4
.driver rom     cart5
.driver rom     cart6
.driver rom     cart7
.driver select  romsSelect

.macro romsSelect
        sta $D500
.endm
```

## NGA2434

`without NAME` in a `phase` block takes out of that Phase a Module that
`resident` put in. It works only for a ROM the driver switches out, one it
names with `.driver rom`. Raised for any other Module, and for every `without`
where no driver switches anything. `atari/800xl.ngp` alone has no such driver.

```ngp
include "atari/800xl.ngp"

modules { "title.asm" "game.asm" }

phase title { needs title  then game }
phase game  { needs game   without osrom }
entry title
```

```asm title.asm
.proc entry
        .transition game
.endp
```

```asm game.asm
.proc entry
        rts
.endp
```

On an 800XL, include `atari/roms.ngp` beside the machine. The 130XE's own
driver switches the ROMs. A program on a diskette keeps the OS in every Phase,
because the diskette's driver reads through it. See
[`phase`](../15-project-file.md#phase).

## NGA2435

A `.driver rom` line may name a second Module, whose ranges showing the first
shows too. On an XL one bit of PORTB switches the OS ROM and the floating-point
pack together, and the driver says so with `.driver rom mathpack osrom`. Raised
where a Phase is without the second Module and needs the first. `mathpack` is
resident in the variant, so a Phase that leaves out only `osrom` still needs
it.

```ngp
include "atari/800xl.ngp"
include "atari/roms.ngp"

modules { "title.asm" "game.asm" }

phase title { needs title  then game }
phase game  { needs game   without osrom }
entry title
```

```asm title.asm
.proc entry
        .transition game
.endp
```

```asm game.asm
.proc entry
        rts
.endp
```

Leave both out: `without osrom, mathpack`.

## NGA2436

Raised for a `without` in the entry Phase of a Project whose Container is
`xex`, once for each Module it names.

```ngp
include "atari/800xl.ngp"
include "atari/roms.ngp"

modules { "game.asm" "ending.asm" }

phase game   { needs game    then ending  without osrom, mathpack }
phase ending { needs ending }
entry game

container xex
```

```asm game.asm
.proc entry
        .transition ending
.endp
```

```asm ending.asm
.proc entry
        rts
.endp
```

Start in a Phase that keeps the ROMs, and take a Transition to the one that
does without them.

## NGA2437

`.alt` lists the opcodes the instruction on the next line may be given. Raised
where that line is not an instruction: data, a Constant, a macro use, the end
of the Section, or a Label on a line of its own. The last is the usual case,
because the Label that names the opcode stands above the `.alt` and not
between it and the instruction.

```asm
.proc walk
.alt iny, dey
direction
        nop
        rts
.endp
```

Move the Label above the `.alt`, or onto the instruction's own line. See
[`.alt`](../16-assembler.md#alt).

## NGA2438

The tool knows an instruction by its length, where control goes after it and
what memory it touches. An Alternative may change none of these, except that
it may return. So the instruction under `.alt` reaches no memory: it has no
operand or an immediate one. Raised where its operand is an address, as when
`inc` and `dec` of a variable are to be swapped.

```asm
.section
position .res 1
.ends

.proc walk
.alt dec
        inc position
        rts
.endp
```

Keep the value in a register and swap instructions that take no operand, such
as `.alt dey` over `iny`.

## NGA2439

Each Alternative takes the operand the instruction is written with, at the
instruction's length. Raised where the processor has no such form of it: `nop`
has no immediate form, so it cannot replace `lda #$80`, and `jmp` has no form
without an operand.

```asm
.proc fade
.alt nop
        lda #$80
        sta $D01A
        rts
.endp
```

Over an immediate, list only instructions that have an immediate form. To turn
code off, put the `.alt` over a one-byte instruction, such as `.alt rts` over a
`nop` at the start.

## NGA2440

A write into an instruction's opcode from outside its Proc is allowed at the
Label of an instruction that has `.alt`. Where the store writes an immediate
loaded just before it, the value must be the instruction's own opcode or one
its `.alt` lists. Raised when it is neither. A value read from a table is taken
on trust.

```asm
.proc walk
direction
.alt iny, dey
        nop
        rts
.endp

.proc entry
        lda #$E8
        sta walk.direction
        jsr walk
        rts
.endp
```

`$E8` is `inx`, and the declaration lists `iny` and `dey`. Write one of the
declared opcodes, or add the mnemonic to the `.alt`. A write into an opcode
with no `.alt` is [`NGA2416`](#nga2416).

## NGA2441

A driver that only switches ROMs, such as `atari/roms.asm`, has no stream: no
`open`, no `read` and no cells. It can be the driver only of a program without
storage. Raised for `nga.open` or `nga.read` where such a driver is the
program's.

```ngp
include "atari/800xl.ngp"
include "atari/roms.ngp"

modules { "main.asm" }
```

```asm
.proc entry
        nga.read
        sta $0600
        rts
.endp
```

A program that reads storage needs the driver of that storage, which streams.
Where no driver is declared at all, the finding is [`NGA2313`](#nga2313).

## NGA2442

`in NAME` with a Region's name puts the Section inside that Region, as
`in underOsLow` does on the XL. The solver places Sections only in `ram` and
`rom` Regions, so a `register` or `reserved` one has no room to give.

```ngp
include "atari/130xe.ngp"

modules { "main.asm" }

container xex
```

```asm
.section in dos
buffer  .res 64
.ends

.proc entry
        lda buffer
        rts
.endp
```

Name a `ram` Region instead. Where the Section has to stand at an address
inside a `reserved` Region, pin it there with `at`, which is a warning,
[`NGA5218`](#nga5218).

## NGA2443

`in` may be repeated to give the solver a choice of Regions:
`.section in underOsLow, in underOsHigh`. A Pane is no such choice. Its
Sections stand in the Bank the Pane is given and nowhere else, so a Pane is
named alone.

```ngp
include "atari/130xe.ngp"

modules { "main.asm" }

panes in ext { level }

container xex
```

```asm
.section in level, in underOsLow
map     .byte 1
.ends

.proc entry
        rts
.endp
```

Keep the Pane or the Regions, not both. A unit set is named alone too,
[`NGA2451`](#nga2451).

## NGA2444

`linearSectionAddress` and `linearFamilyAddress` are where a Section stands in
the medium's own memory, which a blitter or a video chip reads as one. Only a
Section in a Bank has one: a Section in a Pane of a Window that shows a
`linear` unit set, or a Section in such a unit set itself. This Section is in
neither.

```ngp
include "atari/mariacel.ngp"

modules { "main.asm" }

container xex
```

```asm
.section
ship    .byte 1, 2, 3, 4
.ends

.proc entry
        lda #<ship.linearSectionAddress
        rts
.endp
```

Put the Section `in` a Pane of the Window that shows the unit set, here
`panes in cel8000 { textures }`, or `in` the unit set, `.section in sdram`.

## NGA2445

A Pane pinned to a named state, `panes in WINDOW = STATE { ... }`, stands in a
state of the hardware and not in a Bank of a unit set, so it has no linear
address. On MariaCEL the blitter's tables are such Panes, and `cel.draw` is
one of their Sections.

```ngp
include "atari/mariacel.ngp"

modules { "main.asm" }

container xex
```

```asm
.proc entry
        lda #<cel.draw.linearSectionAddress
        rts
.endp
```

Ask for `linearSectionAddress` only of a Section in a Pane the solver gives a
Bank.

## NGA2446

A unit set has linear addresses when its `units` line in the variant ends in
`linear FROM .. TO`, which says where its Banks lie in the medium's memory.
`atari/mariacel.ngp` declares one for `sdram`. `atari/130xe.ngp` declares none
for `extension`.

```ngp
include "atari/130xe.ngp"

modules { "main.asm" }

panes in ext { level }

container xex
```

```asm
.section in level
map     .byte 1
.ends

.proc entry
        lda #<map.linearSectionAddress
        rts
.endp
```

Where only the CPU reads the Banks, there is no linear address to ask for:
read the Section through the Window, under a `.with`. Where other hardware
reads them as one memory, the variant's `units` line is where that is
declared.

## NGA2447

Every member of a family holds the Section at the same address in its own
Bank, so the Section has one linear address per member. Ask for the first
member's with `linearFamilyAddress`. Member `n` is `n` Banks further on, each
Bank as long as the Window.

```ngp
include "atari/mariacel.ngp"

modules { "main.asm" }

panes in cel8000 { frames[2] }

container xex
```

```asm
.section in frames
pixels  .res 256
.ends

.proc entry
        lda #<pixels.linearSectionAddress
        rts
.endp
```

The reverse, `linearFamilyAddress` of a Section in a single Pane, is
[`NGA2448`](#nga2448).

## NGA2448

`linearFamilyAddress` is for a Section in a family, whose members each hold
it. Raised for a Section in a single Pane, and for one that stands in a unit
set itself, whose name the message gives in place of a Pane's.

```ngp
include "atari/mariacel.ngp"

modules { "main.asm" }

panes in cel8000 { textures }

container xex
```

```asm
.section in textures
ship    .byte 1, 2, 3, 4
.ends

.proc entry
        lda #<ship.linearFamilyAddress
        rts
.endp
```

Write `linearSectionAddress`. The reverse is [`NGA2447`](#nga2447).

## NGA2449

A linear address may be wider than 16 bits, and the CPU cannot reach memory
through it in any case. So it is refused as an instruction's operand and as an
item of `.word`. The Labels of a Section `in` a unit set are linear addresses
themselves, and the same holds for them.

```ngp
include "atari/mariacel.ngp"

modules { "main.asm" }

panes in cel8000 { textures }

container xex
```

```asm
.section in textures
ship    .byte 1, 2, 3, 4
.ends

.section
list    .word ship.linearSectionAddress
.ends

.proc entry
        lda list
        rts
.endp
```

Take its bytes and hand them to the hardware that reads the medium:
`<ship.linearSectionAddress`, `>ship.linearSectionAddress` and
`<(ship.linearSectionAddress >> 16)`, as immediates or items of `.byte`.

## NGA2450

A Section `in` a unit set stands in the medium's memory at a linear address,
and has no other. A set whose `units` line declares no `linear` range gives it
none.

```ngp
include "atari/130xe.ngp"

modules { "main.asm" }

container xex
```

```asm
.section in extension
map     .byte 1
.ends

.proc entry
        rts
.endp
```

For memory the CPU reads through a Window, declare a Pane in that Window,
`panes in ext { level }`, and write `.section in level`.

## NGA2451

`in` repeated gives the solver a choice of Regions. A unit set is not one of
them: a Section in it has a linear address and no address the CPU uses, so the
set is named alone.

```ngp
include "atari/mariacel.ngp"

modules { "main.asm" }

container xex
```

```asm
.section in sdram, in ram
ship    .byte 1, 2, 3, 4
.ends

.proc entry
        rts
.endp
```

Keep the unit set or the Regions, not both. A Pane is named alone too,
[`NGA2443`](#nga2443).

## NGA2452

A unit set holds data the medium reads, such as a blitter's textures. The CPU
has no address in it, so it runs no code there, and zero page is the CPU's own
memory. Raised for a Proc and for a `zeropage` Section.

```ngp
include "atari/mariacel.ngp"

modules { "main.asm" }

container xex
```

```asm
.proc draw, in sdram
        rts
.endp

.proc entry
        rts
.endp
```

Code that runs in the Window that shows the set stands in a Pane of that
Window: `panes in cel8000 { code }`, and `.proc draw, in code`.

## NGA2453

Raised for `runtimeSectionAddress` of a Section that stands `in` a unit set.

```ngp
include "atari/mariacel.ngp"

modules { "main.asm" }

container xex
```

```asm
.section in sdram
ship    .byte 1, 2, 3, 4
.ends

.proc entry
        lda #<ship.runtimeSectionAddress
        rts
.endp
```

Its Labels are its linear address: take their bytes, `<ship`, `>ship` and
`<(ship >> 16)`. For memory the CPU reads, put the Section in a Pane instead.

## NGA2454

An expression divides by something that comes to zero. The division has no
value, so it is refused where it is written, whatever the expression is for: an
operand, a `.byte` item, the size of a `.res`, a pin, an alignment or an
assertion. The divisor is usually a constant that one configuration sets to
zero.

```asm
columns = 0

.section
row     .res 40 / columns
.ends

.proc entry
        rts
.endp
```

Give the divisor a value that is not zero, or write the expression so that it
does not divide where that value is zero, as `columns ? 40 / columns : 0`.

## NGA2455

An expression comes to a number that 64 signed bits do not hold, past
-9,223,372,036,854,775,808 or 9,223,372,036,854,775,807. Arithmetic is done in
those bits and the answer has no value, so it is refused where it is written.
In practice the operands are themselves near the limit, often a constant
negated or multiplied once too often.

```asm
least = 0 - $7FFFFFFFFFFFFFFF - 1

.proc entry
        ldx #-least
        rts
.endp
```

Keep the arithmetic within 64 bits. A value meant for a byte or a word is
usually wanted reduced first, as `least & $FF`.

## NGA2456

Raised where `.declare ret` names another Proc's result and the name is not
one. It has to be the reservation another Proc declares its result over, with
every byte in memory, as this Module sees it. A name that resolves to nothing
raises this, and so do an argument's reservation and a result a register
carries part of.

```asm
.proc low
        .declare arg u8
value   .ztemp 1
        .declare ret u16
result  .ztemp 2
        rts
.endp

.proc either
        jsr low
        rts
        .declare ret low.value
.endp

.proc entry
        jsr either
        rts
.endp
```

Name the reservation under the other Proc's `.declare ret`, here
`low.result`. If that result is partly in a register, declare this result over
a reservation of its own and copy the value into it.

## NGA3001

`.with` covers the one statement on the next line, and that statement has to be
an instruction or a macro use. A `.byte`, a directive, or a Label on a line of
its own is refused, and so is a `.with` with nothing after it. An `.off`,
`.own`, `.root`, `.alt` or another `.with` may stand between.

```ngp
include "atari/130xe.ngp"

modules { "main.asm" }

panes in ext { level }

container xex
```

```asm
.section in level
map     .byte 1
.ends

.proc entry
        .with level
@first
        lda map
        rts
.endp
```

Write the Label on the line of the instruction, `@first lda map`, or above the
`.with`.

## NGA3002

Each form of `.with` takes one kind of name. `.with PANE` takes a Pane or a
member of a family, `FAMILY + n`. The `, x` form takes a family or a Window,
and `= STATE` a Window. Raised for any other pairing, such as a Window alone or
a family alone.

```ngp
include "atari/130xe.ngp"

modules { "main.asm" }

panes in ext { slots[3] }

container xex
```

```asm
.section in slots
row     .res 40
.ends

.proc entry
        .with slots
        lda row
        rts
.endp
```

Name a member, `.with slots + 0`, or show the member whose state is in `X`
with `.with slots, x`.

## NGA3003

`WINDOW = STATE` names one of the states the variant lists after `views`. A
name the Window does not list names nothing. Neither does the name of a unit
set it lists, which gives one state per Bank and has no name of its own.

```ngp
include "atari/130xe.ngp"

modules { "main.asm" }

container xex
```

```asm
.proc entry
        .with ext = extension
        nop
        rts
.endp
```

Show a Bank through the Pane that stands in it, `.with PANE`, or by number
with `.with ext, x`.

## NGA3005

The `.with` shows what is shown already. That is the case in a Proc declared
`under` a Pane, for a `.with` of that Pane, and in a Pane's own code, for a
`.with` of the same Pane. The switch is still written, and costs bytes and
time.

```ngp
include "atari/130xe.ngp"

modules { "main.asm" }

panes in ext { level }

container xex
```

```asm
.section in level
map     .byte 1
.ends

.proc readMap, under level
        .with level
        lda map
        rts
.endp

.proc entry
        .with level
        jsr readMap
        rts
.endp
```

Remove the `.with`.

## NGA3006

Several `.with` lines over one statement show one state each, on different
Windows. Two on the same Window would need it to show two states at once.
Raised also where a `.with` in a macro body names the Window that the `.with`
over the use shows already.

```ngp
include "atari/130xe.ngp"

modules { "main.asm" }

panes in ext { level, tables }

container xex
```

```asm
.section in level
map     .byte 1
.ends

.section in tables
table   .byte 2
.ends

.proc copy
        lda table
        sta map
        rts
.endp

.proc entry
        .with level
        .with tables
        jsr copy
        rts
.endp
```

Split the work into two statements, each under its own `.with`, and carry the
data between them in memory outside the Window.

## NGA3007

In a Proc declared `under` a Pane, a `.with` on that Pane's Window is followed
by code the tool adds, which shows the Pane again, since the Proc returns to
code that expects it shown. An `rts` or `rti` as the statement, or in a macro
used there, returns before that code runs. A `jsr` is allowed, because it
returns to where the added code stands.

```ngp
include "atari/130xe.ngp"

modules { "main.asm" }

panes in ext { level, tables }

container xex
```

```asm
.section in level
map     .byte 1
.ends

.section in tables
table   .byte 2
.ends

.macro fetch
        lda table
        rts
.endm

.proc readTable, under level
        .with tables
        fetch
.endp

.proc entry
        .with level
        jsr readTable
        rts
.endp
```

Return on a line of its own, after the statement. Outside such a Proc nothing
is shown again, and a return under `.with` is allowed.

## NGA3008

A `.with` shows one state of its Window. A statement under it that reaches a
Section of another Pane is reaching memory that is not there. The tool follows
every `jsr` and `jmp` out of the statement, so a routine two calls away that
names such a Section is caught too, and [`NGA3009`](#nga3009) points at the
name.

```ngp
include "atari/130xe.ngp"

modules { "main.asm" }

panes in ext { level, tables }

container xex
```

```asm
.section in level
drawLevel
        rts
.ends

.proc entry
        .with tables
        jsr drawLevel
        rts
.endp
```

Show the Pane the statement reaches. The rule holds over Symbols: memory
reached through a pointer is not checked.

## NGA3009

Follows [`NGA3008`](#nga3008), and [`NGA3017`](#nga3017) where that is raised
at a `.with`. It points at the place the Symbol is named, which may be in a
routine the statement calls rather than in the statement itself.

## NGA3011

A Pane pinned to a named state, as `atari/mariacel.ngp` pins `sprlist` with
`panes in cel4000 = sprlist { sprlist }`, is shown by that state and no other.
Its Sections are named only where the state is shown: under a `.with` that
shows it, in a Proc declared `under` the Pane, or in the Pane's own code.

```ngp
include "atari/mariacel.ngp"

modules { "main.asm" }

container xex
```

```asm
.proc entry
        lda #0
        sta cel.draw
        rts
.endp
```

Put `.with cel4000 = sprlist`, or `.with sprlist`, over the statement. For a
Pane the solver gives a Bank, the same rule is [`NGA2412`](#nga2412).

## NGA3012

`.own` and `.root` cover the statement on the next line, which takes an
address: an instruction, a `.byte` or a `.word`. Raised where that line is
anything else: a `.res`, a `.dispatch`, a Label on a line of its own, or
nothing at the end of the file.

```asm
.section
        .own entry
table
        .word handler
.ends

.proc handler
        rts
.endp

.proc entry
        jmp (table)
.endp
```

Put the directive directly above the statement. A Label goes on the
statement's own line, `table .word handler`, or above the directive.

## NGA3013

An address under `.root` goes to the hardware: what it names becomes a Root,
and no jump of the program's reaches it through this statement. An address
under `.own` is followed by the program. One statement says one or the other.

```asm
.proc entry
        .own
        .root vbd
        ldx #>handler
        rts
.endp

.proc handler
        rts
.endp
```

Keep the one that is true of the address.

## NGA3014

`.root` names the Holder that takes the address, one of the `holder` names the
Target declares, such as `vbd` or `dlist`. A statement hands one address to
one Holder, so a `.root` with no name or with two is refused.

```asm
.proc entry
        .root
        ldx #>handler
        rts
.endp

.proc handler
        rts
.endp
```

Memory that something outside the program uses for as long as its Module is
present has no statement that hands it over. Mark that Section `root` instead.

## NGA3015

Code in a Pane stands in the Window's memory, so a `.with` on the same Window
would switch away the code that is running. Raised for code in a Pane and in a
family of the Window. A `.with` of the code's own Pane is the warning
[`NGA3005`](#nga3005) instead.

```ngp
include "atari/130xe.ngp"

modules { "main.asm" }

panes in ext { level, tables }

container xex
```

```asm
.section in tables
table   .byte 2
.ends

.section in level
drawLevel
        .with tables
        lda table
        rts
.ends

.proc entry
        .with level
        jsr drawLevel
        rts
.endp
```

Do the switch in a Trampoline: a Proc outside the Window, declared
`.proc NAME, under level`, which holds the `.with` and is called from the
Pane's code. See [Panes](../06-panes.md).

## NGA3016

`under` declares the Pane a Proc runs with shown. It is refused on a family,
whose member is known only at run time; on a `.section`, since data does not
run; and beside `in`, since a Proc under a Pane stands outside the Window and
not in a Pane of it.

```ngp
include "atari/130xe.ngp"

modules { "main.asm" }

panes in ext { slots[3] }

container xex
```

```asm
.section in slots
row     .res 40
.ends

.proc clearRow, under slots
        lda #0
        sta row
        rts
.endp

.proc entry
        rts
.endp
```

Code that works on a family's members stands `in` the family and is called
under `.with slots, x`, as in
[Panes](../06-panes.md#more-than-one-of-the-same-thing).

## NGA3017

A Proc declared `under` a Pane names that Pane's Sections on the promise that
the Pane is shown whenever it runs. Every call is held to it. The caller is
the Pane's own code, another Proc under the same Pane, or a statement under a
`.with` that shows the Pane.

```ngp
include "atari/130xe.ngp"

modules { "main.asm" }

panes in ext { level }

container xex
```

```asm
.section in level
map     .byte 1
.ends

.proc readMap, under level
        lda map
        rts
.endp

.proc entry
        jsr readMap
        rts
.endp
```

Put `.with level` over the call.

## NGA4501

A Phase that no chain of `then` from the entry Phase reaches. Nothing enters
it, yet it is built, placed and carried in the Container. It is usually a
`then` that was renamed or removed.

```ngp
modules { "main.asm" }

phase game   { needs main }
phase scores { needs main  entry showScores }
entry game
```

```asm
.proc entry
@stop   jmp @stop
.endp

.proc showScores
@stop   jmp @stop
.endp
```

Add the `then` that leads to the Phase, or remove the Phase. Raise the warning
to an error with `deny NGA4501` in the
[`diagnostics`](../15-project-file.md#diagnostics) block of a Project where this
must not happen.

## NGA4505

Prune drops a Section that nothing reaches from a Root, pinned or not. A pin
usually means something expects the memory at that address, often the
hardware or an OS that no instruction of the program names. Once the Section
is dropped, the solver may give that memory to another Section.

```asm
.section absolute at $9F00
mailbox .res 16
.ends

.proc entry
        rts
.endp
```

Mark the Section `root` if something outside the program reads or writes it.
Otherwise remove the pin, or the Section. See
[Roots](../13-roots.md#what-nothing-reaches).

## NGA4601

A Temporary shares its bytes with Temporaries that are never active at the
same time. An address of it, once taken, could be used while another
Temporary holds those bytes. Raised at the statement that takes the address,
where no `.own` stands over it.

```asm
.proc entry
buffer  .temp 8
        lda #<buffer
        rts
.endp
```

Where a routine reads or writes through the pointer, name it with `.own NAME`
on each taking. It then becomes an Owner of the Temporary, as a routine that
names it is. Otherwise reserve the memory with `.res`, which never shares.

## NGA4602

A call from an Owner of the Temporary can come back into that Owner, through
another Section, before the Owner returns. The second Activation uses the same
bytes and destroys the value of the first.

```asm
.proc walk
depth   .ztemp 1
        sta depth
        jsr step
        lda depth
        rts
.endp

.proc step
        jsr walk
        rts
.endp

.proc entry
        jsr walk
        rts
.endp
```

Keep such a value in a plain `.res`, and save it on the stack around the call
where it has to survive the call, or break the cycle. A path that comes back
through a `.transition` is not such a cycle.

## NGA4603

`.own` says who follows an address of code, or who owns a Temporary whose
address is taken, and the statement under it takes neither. An address of data
needs no `.own`. Over the `jmp (ptr)` itself it does nothing either, since the
edge belongs to the statement that took the address.

```asm
.section
colors  .byte $28, $CA, $94
.ends

.proc entry
        .own
        lda #<colors
        rts
.endp
```

Remove the `.own`, or move it to the statement that takes the address of the
code.

## NGA4606

Taking the address of code tells the tool nothing about who will jump to it,
so the statement has to say: `.own` for this Section, `.own NAME` for another,
`.root HOLDER` for the hardware, naming which. An address of data needs none
of this.

```asm
.section
actions .word fire
.ends

.proc fire
        rts
.endp

.proc entry
        lda actions
        rts
.endp
```

Trace needs the jump the address stands for to know which Sections can be
active at once. See [Roots](../13-roots.md#naming-the-follower).

## NGA4607

A jump through a pointer goes wherever the pointer holds. The tool follows the
addresses of code the program owns, so a Section that jumps that way has to
own the code it can reach.

```asm
.section zeropage
vector  .res 2
.ends

.proc entry
        jmp (vector)
.endp
```

Put `.own` on the statements that take the addresses the pointer is loaded
with, or `.own NAME` naming this Section where another Section takes them. A
table of positions of the jumping Section itself is a `.dispatch`. This often
comes together with [`NGA4606`](#nga4606), at the other end of the same
pointer.

## NGA4608

`.root` hands what the statement names to the hardware, and the statement
names no Section: only a number or a Constant. The `.root` usually stands one
line off the statement that takes the address.

```ngp
target { holder vbd }

modules { "main.asm" }
```

```asm
.proc entry
        .root vbd
        lda #7
        rts
.endp
```

Move it down to the statements that take the address. Before a call of
`SETVBV` on the XL, that is `.root vbd` over `ldx #>handler` and again over
`ldy #<handler`.

## NGA4609

`.own NAME` names the Section that follows the address, by any of its Labels;
a Proc's name is one. A name that is something else, such as a Constant, or
that is not defined, is refused.

```asm
VDSLST = $0200

.proc entry
        .own VDSLST
        lda #<handler
        sta VDSLST
        rts
.endp

.proc handler
        rti
.endp
```

Name the code that jumps through the pointer, not the pointer. Where the
hardware does the jumping, write `.root HOLDER` instead, here `.root dli` on
the XL.

## NGA4611

`.own NAME` hands an address of code to another Section, which jumps to it
later. Residency is per Module, so this is raised where the follower's Module
is present in a Phase and the code's Module is not: here a resident dispatcher
and a routine of `game`. Reported once per taking and follower, for the first
such Phase.

```ngp
modules { "main.asm" "game.asm" }

resident { main }

phase game { needs game  then over  entry gameStart }
phase over { entry gameOver }
entry game
```

```asm
.export dispatch, ptr

.section zeropage
ptr     .res 2
.ends

.proc dispatch
        jmp (ptr)
.endp

.proc gameOver
@stop   jmp @stop
.endp
```

```asm game.asm
.proc gameStart
        lda handler
        sta ptr
        lda handler + 1
        sta ptr + 1
        jsr dispatch
        .transition over
        .own dispatch
handler .word step
.endp

.proc step
        rts
.endp
```

Hand the address to a follower in the code's own Module, which is present
exactly where the code is, or make the code's Module present wherever the
follower is. Under a bare `.own` the follower is the Section that takes the
address, and the same mistake is [`NGA2414`](#nga2414).

## NGA4612

Sections are placed independently, so control that runs past the last
statement of one goes into whatever the solver put after it. Raised at the
statement some run leaves from: an instruction that goes on, a call of a
routine that may return, a branch to the end of the Section, or code that runs
into data.

```asm
.proc entry
        jsr init
.endp

.proc init
        rts
.endp
```

A `brk` ends a Section as a return or a jump does. To fall into the next Proc
on purpose, chain it with
[`.endp then NAME`](../16-assembler.md#endp-then-name), which the solver places
right after. Where a branch at the end is always taken, `.off NGA4612` on it
says so.

## NGA4613

A `final` Phase is one the program never leaves. The tool keeps out of it the
code that runs only when the program ends or returns. A return from the
Phase's entry would land in that code, which is not there.

```ngp
modules { "main.asm" }

phase game { needs main  final }
entry game
```

```asm
CONSOL = $D01F

.proc entry
@frame  lda CONSOL
        cmp #6
        bne @frame
        rts
.endp
```

End every path of the entry in a loop that does not return, or drop `final`.

## NGA4614

`.root` and `.release` name a Holder: hardware that holds one Section at a
time, which the Target declares with `holder`. `atari/800xl.ngp` and
`atari/130xe.ngp` declare nine, from `dlist` to `dosini`. The stand-in Target
declares none.

```asm
.proc entry
        .root dlist
        lda #<list
        rts
.endp

.section
list    .byte $70
.ends
```

Include the variant of the machine, or declare the Holder in a `target` block
of the Project, `target { holder dlist }`. See
[Roots](../13-roots.md#what-the-hardware-follows).

## NGA5102

The processor has no instruction for this mnemonic and this operand. Either
the form does not exist at all, as `sta #1`, or it exists for one
PlacementClass only: `stx` takes `,y` and `sty` takes `,x` only with a
zero-page operand. The message names the form that is missing: `absolute,y`
where only the zero-page form exists, as here, and `direct,y` where neither
width has one.

```asm
.section
buffer  .res 16
.ends

.proc entry
        ldy #0
        stx buffer,y
        rts
.endp
```

`buffer` is `absolute`. Put it in a `.section zeropage`, or go through `A`
with `txa` and `sta buffer,y`.

## NGA5103

An operand without `#` that is an `Integer` has no PlacementClass, so its
value decides whether the instruction is zero page or absolute. That value has
to be a [declared value](../16-assembler.md#declared-values), known before any
Section has an address. `tableEnd - table` is an `Integer` with Labels at its
leaves, and has a value only after Size.

```asm
.section
table   .byte 1, 2, 3
tableEnd
.ends

.proc entry
        ldx #0
@loop   lda table,x
        inx
        cpx tableEnd - table
        bne @loop
        rts
.endp
```

Here the `#` is missing: `cpx #tableEnd - table` compares with the count.
Where an address is meant, write an `Address`, such as a Label plus an offset,
which takes its Section's PlacementClass.

## NGA5104

The count of a `.res` may be written in terms of other sizes, such as
`work.runtimeSectionSize` or the distance between two Labels. Raised where
following those sizes leads back to the reservation itself.

```asm
.section
buffer  .res buffer.runtimeSectionSize
.ends
```

A cycle through several Sections is reported once, at one of its
reservations. Break it by writing one of the sizes as a Constant.

## NGA5105

`.res` takes a count from 0 to 65,536. A negative count usually comes from a
subtraction of two Constants in the wrong order, or after one of them changed.

```asm
ROWS = 24
VISIBLE = 25

.section
spare   .res ROWS - VISIBLE
.ends
```

A Section that stands in a unit set declaring `linear` may reserve up to the
size of the set's Banks end to end, which can exceed 64 KB.

## NGA5106

An `Integer` operand without `#` is an address given by its value, so the
value must lie between `$0000` and `$FFFF`. It is usually a Constant with an
offset added past the end of memory, or subtracted below zero.

```asm
SCREEN = $F000

.proc entry
        lda SCREEN + $1000
        rts
.endp
```

Check the Constant and the arithmetic on it.

## NGA5107

The Target says which processor the program is for, and a Target that names
none is a 6502. The 65SC02's additions, such as `stz`, `bra`, `phx` and the
`(zp)` modes, are refused on a 6502. So is an opcode of the 65SC02 that an
`.alt` names.

```asm
.section
counter .res 1
.ends

.proc entry
        stz counter
        rts
.endp
```

Where the machine has a 65SC02, write `cpu "65sc02"` in its `target` block;
see [the Project file](../15-project-file.md#cpu). Otherwise write the 6502 form,
here `lda #0` and `sta counter`. See
[Instructions](../12-instructions.md#what-the-processor-has).

## NGA5201

Two pinned Sections claim the same bytes in a Phase both are present in. A pin
is never moved, so the tool has no way to resolve it. The message names the
Section reported and the pinned one it lands on.

```asm
.section absolute at $3000
screen  .res 960
.ends

.section absolute at $3300
font    .res 1024
.ends
```

`screen` reaches `$33BF`. Move one of the pins, or drop one and let the solver
place that Section. Sections that are never in memory at the same time, such
as reservations in Modules no Phase needs together, may be pinned at the same
address.

## NGA5202

Raised in two cases. A pinned Section runs past `$FFFF`. Or a Section that is
not pinned is larger than every range of free memory it could start in, once
its `in`, `align` and `within` are counted.

```asm
.section
buffer  .res 40000
.ends
```

The findings after it say the same thing for the whole program: no layout
exists ([`NGA5206`](#nga5206)), and the Phase needs more memory than it has
([`NGA5631`](#nga5631)). With no `region` declared, the stand-in Target has
32 KB, at `$2000-$9FFF`. Make the Section smaller or split it. A zero-page
Section that does not fit is [`NGA5204`](#nga5204) instead.

## NGA5203

A pin is a [declared value](../16-assembler.md#declared-values) from `$0000` to
`$FFFF`, so no Label and no size may appear in it: a pin is part of the layout,
and a Label's address is what the layout decides. In a Section that stands in a
unit set declaring `linear`, the pin is a linear address and may be larger.

```asm
.section absolute at $10000
table   .byte 1
.ends
```

## NGA5204

A `zeropage` Section lies wholly below `$100`. Raised for a pin that makes it
run past `$FF`, and for a Section that is not pinned and is larger than every
free range of zero page the Target has.

```asm
.section zeropage at $F0
ptrs    .res 32
.ends
```

Pin it lower, split it, or move what does not need zero-page addressing into
an `absolute` Section. With no `region` declared, the stand-in Target has 16
bytes of zero page, `$80-$8F`.

## NGA5205

The Window the driver's stream reads through is where storage arrives. A
Section that waits in storage cannot also live at the address it arrives
through, because the arrival would overwrite it while it is being read.

```ngp
include "atari/130xe.ngp"
include "atari/extension.ngp"

modules { "main.asm" "level.asm" }

phase intro { needs main  then level }
phase level { needs level  entry run }
entry intro
```

```asm
.proc entry
        .transition level
.endp
```

```asm level.asm
.section absolute at $4000
map     .byte 1, 2, 3
.ends

.proc run
        lda map
@       jmp @-
.endp
```

Only a pin reaches this, since the solver keeps such a Section out of the
Window by itself. Drop the pin, or put the Section in a [Pane](../06-panes.md),
which has no Payload and is read where it stands. A Section with no Payload
pinned in a Window is [`NGA5235`](#nga5235). See
[Banks and Windows](../05-banks-and-windows.md#what-may-not-live-in-the-window).

## NGA5206

The solver showed that no layout exists. Usually a finding beside it says why:
a Phase over capacity ([`NGA5631`](#nga5631), [`NGA5632`](#nga5632)), a Pane
over its Bank ([`NGA5225`](#nga5225)), too little ROM
([`NGA5234`](#nga5234)), or a Section with no room at all
([`NGA5202`](#nga5202)). Alone, it means that every Section fits on its own and
every sum holds, and the constraints together still cannot be met.

```asm
.section absolute align $4000
font    .res $3000
.ends

.section absolute align $4000
screen  .res $3000
.ends
```

Both can start only at `$4000`, since `$8000` plus their size runs past the
stand-in's memory. Run the build with `--explain` to have the tool name the
pairs no layout keeps apart ([`NGA5207`](#nga5207)).

## NGA5207

Raised only when the build is run with `--explain`, after
[`NGA5206`](#nga5206), and only where no sum already said why. The tool then
looks for the layout that lets the fewest pairs of Sections overlap, and names
each pair it had to let overlap. Where several choices are equally small, it
names one of them. Each Section of a pair gets an [`NGA5208`](#nga5208) note.

Change one Section of each pair: its size, its `align`, `within` or pin, or
the Phases its Module is needed in.

## NGA5208

Follows [`NGA5207`](#nga5207), once for each Section of the pair, and points at
its declaration.

## NGA5209

Follows the [`NGA5207`](#nga5207) findings under `--explain` when the search
for the fewest overlapping pairs ran out of its budget. A layout exists if the
pairs named may overlap, but fewer pairs might be enough. Start from them all
the same.

## NGA5210

`.assert` states a condition checked after Place, when every address and size
is known. Raised where the condition is zero.

```asm
.section
colors  .byte $00, $0E, $34, $86, $C8
.ends

.proc entry
        lda colors
        rts
.endp

.assert colors.runtimeSectionSize == 4
```

An assertion guards something the code relies on and the solver does not
know. Either the program changed under it, or the assertion is out of date.

## NGA5211

Raised where an `.assert` has no value even after Place. The usual cause is a
Section that Prune dropped, which has no address: an `.assert` is not a
Reference, so it does not keep the Section it names. A division whose divisor
comes out zero is the other cause.

```asm
.section
table   .byte 1, 2, 3
.ends

.proc entry
        rts
.endp

.assert table >= $2000
```

Nothing reaches `table`. Name it from code that is kept, mark it `root`, or
remove the assertion.

## NGA5212

`align N` takes a [declared value](../16-assembler.md#declared-values) that is a
power of two from 1 to 65,536: the start of the Section is a multiple of it.
Raised for zero, for a larger value, for one that is not a power of two, such as
`align 3`, and for one that is not declared.

```asm
.section absolute align 0
font    .res 1024
.ends
```

## NGA5213

`within N` takes a declared value from 1 to 65,536, as `align` does
([`NGA5212`](#nga5212)), but need not be a power of two: the Section does not
cross a multiple of it. `within 256` keeps a Section in one page.

```asm
.section absolute within 0
list    .res 32
.ends
```

## NGA5214

`within N` keeps a Section from crossing a multiple of N, so the Section can
be at most N bytes. Raised before the solver is asked, since no start would
do.

```asm
.section absolute within 256
sines   .res 320
.ends
```

Split the Section into parts that each fit, each with its own `within`, or
widen the boundary.

## NGA5215

A pin has to satisfy the rest of the Section's placement. Here the address
after `at` is not a multiple of the `align`.

```asm
.section absolute at $3200 align 1024
font    .res 1024
.ends
```

Move the pin to a multiple, or drop the `at` and let the solver choose an
aligned address.

## NGA5216

A pinned Section with `within N` must not cross a multiple of N. This pin
starts close enough to one that the Section's last byte lies past it.

```asm
.section absolute at $33F0 within 1024
dlist   .res 32
.ends
```

Move the pin, or drop the `at` and let the solver find a start that keeps the
Section inside the boundary.

## NGA5217

A `register` Region is hardware, and nothing the program places can stand
there. A pin into one is an error, where a pin into a `reserved` Region is a
warning ([`NGA5218`](#nga5218)).

```ngp
target {
  region $0080 .. $00FF ram
  region $2000 .. $BFFF ram
  region gtia $D000 .. $D01F register
}

modules { "main.asm" }
```

```asm
.section absolute at $D000
hposp0  .res 1
hposp1  .res 1
.ends
```

Name a register in the `target` block instead, as `register HPOSP0 $D000`,
and every Module sees the name; see
[the Project file](../15-project-file.md#register). Memory that another chip
reads, such as a Lynx display buffer over Mikey's registers, is declared
`foreign` and `root`, and may lie there.

## NGA5218

A `reserved` Region belongs to something else, usually the machine or its DOS.
A pin puts a Section there anyway, which the tool allows and reports. Read it
as a question: is this Region really reserved, or is this pin really wanted.

```ngp
target {
  region $0080 .. $00FF ram
  region dos $0700 .. $1FFF reserved
  region $2000 .. $BFFF ram
}

modules { "main.asm" }
```

```asm
.section absolute at $0700
buffer  .res 16
.ends

.proc entry
        lda buffer
        rts
.endp
```

Not raised for a `foreign` Section, nor for a Module the Container's load
needs, whose memory the variant reserves for it. A pin into a `register` Region
is an error instead, [`NGA5217`](#nga5217), and so is a pin into a `rom` Region
of a Section that is not read-only, [`NGA5233`](#nga5233).

## NGA5219

The solver's search has a budget. Raised where it ran out before finding a
layout or showing that none exists, and no sum over a Phase refused the
program first. It does not mean that no layout exists. The
[`NGA5220`](#nga5220) note names the pool with the least room left, which is
where to look first.

Many pins that cut memory into small holes, with many Sections about the size
of those holes, make the search hard. Free some room in the pool the note
names, or drop pins and alignments the program does not need. `--explain`
does not run here, since nothing was shown to explain.

## NGA5220

Follows [`NGA5219`](#nga5219). It names the Phase and the pool, `view fixed`
or the zero page, whose sum came closest to refusing the program, and how many
of its bytes are free.

## NGA5222

A Section with a Payload waits in storage until a Transition loads it, and so
does every Frame. Raised where storage has no free run long enough for this
one after what was placed before it. The size is that of the stored form, so a
`transform` changes it. A Frame is named by its edge, as
`frame intro -> level`.

```ngp
target {
  region $0080 .. $00FF ram
  region $2000 .. $BFFF ram
  register PORTB $D301
  units extension 1
  window ext $4000 .. $5FFF views extension
}
storage { units extension }

modules { "atari/portb.asm" "main.asm" "level.asm" }
resident { portb }

phase intro { needs main  then level }
phase level { needs level  entry run }
entry intro
```

```asm
.proc entry
        .transition level
.endp
```

```asm level.asm
.section
map     .byte 1
        .res $2000
        .byte 1
.ends

.proc run
        lda map
@       jmp @-
.endp
```

Storage here is one Bank of 8 KB. Reserved space between two emitted bytes
travels with them, so the `.res` costs storage; at the end of the Section it
would cost nothing. Otherwise compress the Section with a `transform` in the
Project, or use a Target with more storage. See
[the Project file](../15-project-file.md#transform).

## NGA5224

A `root` Section is read by the hardware with no Reference in the code. One
that only reserves space has no bytes to carry, so where a Phase that does not
need its Module comes between two that do, nothing puts its contents back. The
later Phase finds what the Phase in between left there.

```ngp
modules { "main.asm" "game.asm" }

phase intro { needs main  then game }
phase game  { needs game  then outro  entry play }
phase outro { needs main  then game  entry finish }
entry intro
```

```asm
.section root
screen  .res 960
.ends

.proc entry
        .transition game
.endp

.proc finish
@       jmp @-
.endp
```

```asm game.asm
.proc play
        .transition outro
.endp
```

List the Module in `game` too, or in `resident`, so that it stays. Where the
program sets the memory up again on every entry, `.off NGA5224` above the
Section says so. A `root` Section that holds bytes is not reported: its bytes
are brought back, or kept in place where there is no storage.

## NGA5225

A Pane is one Bank, so its Sections present in one Phase have to fit in it
together. The sum is checked before the solver is asked, and the finding
points at the Pane. A Project with no `phase` block has one unnamed Phase,
which the message calls `(implicit)`.

```ngp
include "atari/130xe.ngp"

modules { "main.asm" }

panes in ext { level }
```

```asm
.section in level, root
map     .res $3000
.ends

.section in level, root
tiles   .res $1800
.ends
```

Declare a second Pane, `panes in ext { level, tiles }`, and move one Section
to it, or put the two in Modules no Phase needs together.
[`NGA5206`](#nga5206) follows, as it follows every sum that refuses.

## NGA5226

A Pane's Section is reached through its Pane's Window, which is the only place
its Bank appears. A pin must put the whole Section inside the Window's ranges.

```ngp
include "atari/130xe.ngp"

modules { "main.asm" }

panes in ext { level }
```

```asm
.section in level, absolute at $8000, root
map     .res 1024
.ends
```

`ext` is `$4000-$7FFF` on the 130XE. Pin inside it, or drop the pin and let
the solver choose where in the Window the Section stands.

## NGA5227

A named state is not a Bank: storage holds nothing for it, and outside the
case the message names, no Container writes bytes into it. A Pane pinned to
one holds reservations, which the program fills itself.

```ngp
include "atari/mariacel.ngp"

modules { "main.asm" }

panes in cel4000 = derived { shapes }

container xex
```

```asm
.section in shapes, root
shape   .byte $0F, $3C, $F0
.ends

.proc entry
        rts
.endp
```

On MariaCEL the driver's stream reads through `cel8000`, and `cel4000` shows the
blitter's memory. Reserve the space with `.res` and fill it from code under a
`.with`, as `atari/mceltables.asm` does.

## NGA5229

A family `NAME[N]` is N Panes on N consecutive Banks of the unit set its
Window shows. Raised where N is larger than the number of Banks in the set.

```ngp
include "atari/130xe.ngp"

modules { "main.asm" }

panes in ext { rooms[8] }
```

```asm
.section in rooms, root
room    .res 4096
.ends
```

The 130XE has five Banks. Declare fewer members. [`NGA5206`](#nga5206)
follows. See [Panes](../06-panes.md#more-than-one-of-the-same-thing).

## NGA5230

A Section nothing writes is read-only without being told. `readonly` is for one
whose address the program takes, where the tool cannot see what is done
through the pointer, and it says the pointer is only read through. A write the
tool can see contradicts the word and is refused: a store, an increment or a
shift on it.

```asm
.section readonly
lives   .byte 3
.ends

.proc entry
        dec lives
        rts
.endp
```

Drop `readonly` if the Section is meant to change, or the write if it is not.
[`NGA5231`](#nga5231) points at the declaration.

## NGA5231

Follows [`NGA5230`](#nga5230) and points at the `.section readonly` that the
write contradicts.

## NGA5232

`readonly` is about bytes that could stand in ROM. A Section of reservations
alone emits none, so the word has nothing to say about it.

```asm
.section readonly
buffer  .res 64
.ends

.proc entry
        lda buffer
        rts
.endp
```

Remove `readonly`. A Section meant to hold constant data needs `.byte` or
`.word` rather than `.res`.

## NGA5233

A `rom` Region holds what nothing writes. A pin is the author's word, so a
Section pinned there stands there. Raised where that Section is not read-only:
the program writes it, it emits no bytes, or its address is taken and it does
not say `readonly`.

```ngp
target {
  region $0080 .. $00FF ram
  region $2000 .. $7FFF ram
  region cart $8000 .. $9FFF rom
}

modules { "main.asm" }
```

```asm
.section absolute at $8000
score   .byte 0
.ends

.proc entry
        inc score
        rts
.endp
```

A write to ROM changes nothing, and the next read returns what the ROM holds.
Pin a variable into RAM, or drop the pin: the solver puts what nothing writes
into ROM by itself. A table read through a pointer needs `readonly`.

## NGA5234

Where the Target has a `rom` Region, every Section nothing writes stands
there, code included. ROM is summed once for the whole run and not per Phase,
because nothing loads it and no Phase gives its addresses to another. The
notes name the largest Sections.

```ngp
target {
  region $0080 .. $00FF ram
  region $2000 .. $2FFF ram
  region $8000 .. $800F rom
}

modules { "main.asm" }
```

```asm
.section
message .byte "HELLO, WORLD!"
.ends

.proc entry
        lda message
        rts
.endp
```

On a cartridge, move what one Phase needs into a Pane, which stands in a Bank
of the board rather than in its fixed part; see
[Cartridges](../14-cartridges.md#the-machine). [`NGA5206`](#nga5206) follows.

## NGA5235

No Window has a base: what it shows outside a `.with` is whatever was shown
last. So the solver gives a Window's addresses only to Panes and to storage,
and a pin of any other Section there is refused.

```ngp
include "atari/130xe.ngp"

modules { "main.asm" }
```

```asm
.section absolute at $4000
buffer  .res 256
.ends

.proc entry
        lda buffer
        rts
.endp
```

Pin it outside `$4000-$7FFF`, or put it in a Pane with `in NAME` and read it
under a `.with`. A Section with a Payload pinned there is
[`NGA5205`](#nga5205) instead. See
[Panes](../06-panes.md#nothing-else-in-the-window).

## NGA5240

Raised only when the tool runs with `--verify-layout`. The option reads the
Layout again after Place and checks every Constraint against it, with code that
shares nothing with the solver. A finding of it means the tool placed something
wrong. The program is not at fault, and there is nothing in it to change.
Report the finding with the Project that raised it. The option changes nothing
in the build, so a build without it writes the same Layout with the same
fault.

This one finds two Sections on a common address in a Phase that holds both. Two
Temporaries that are never live at once may share an address, and so may two
Sections in different states of one Window. This pair is neither.

## NGA5241

A finding of `--verify-layout`, and a fault of the tool, not of the program;
see [`NGA5240`](#nga5240). The Section is pinned with `at`, and the Layout put
it somewhere else.

## NGA5242

A finding of `--verify-layout`, and a fault of the tool, not of the program;
see [`NGA5240`](#nga5240). A `zeropage` Section ends below `$0100`, and the
Layout let this one run past it.

## NGA5243

A finding of `--verify-layout`, and a fault of the tool, not of the program;
see [`NGA5240`](#nga5240). A Section that waits in storage may not stand in the
Window the driver's stream reads through, because the Window shows a Bank there
while a Transition reads. The Layout put this one there. The same rule for a
pin is [`NGA5205`](#nga5205).

## NGA5244

A finding of `--verify-layout`, and a fault of the tool, not of the program;
see [`NGA5240`](#nga5240). The solver placed the Section outside the memory it
may stand in: the `ram` Regions of its placement class, the `rom` Regions for a
Section nothing writes, its Pane's Window, or the Regions its `in` names. For a
Section in a unit set, that memory is the set's Banks. A pinned Section is not
held to this.

## NGA5245

A finding of `--verify-layout`, and a fault of the tool, not of the program;
see [`NGA5240`](#nga5240). Prune kept the Section, so it needs an address, and
the Layout has none for it.

## NGA5246

A finding of `--verify-layout`, and a fault of the tool, not of the program;
see [`NGA5240`](#nga5240). The Layout put the Section where it runs past
`$FFFF`.

## NGA5247

A finding of `--verify-layout`, and a fault of the tool, not of the program;
see [`NGA5240`](#nga5240). The Section says `align N`, and the Layout put it at
an address that is not a multiple of `N`.

## NGA5248

A finding of `--verify-layout`, and a fault of the tool, not of the program;
see [`NGA5240`](#nga5240). The Section says `within N`, and the Layout put it
across a multiple of `N`.

## NGA5250

A finding of `--verify-layout`, and a fault of the tool, not of the program;
see [`NGA5240`](#nga5240). Prune dropped the Section because nothing reaches
it, and the Layout gave it an address anyway.

## NGA5251

A finding of `--verify-layout`, and a fault of the tool, not of the program;
see [`NGA5240`](#nga5240). The Proc ends with `.endp then NAME`, so `NAME` must
start at the byte after it in every Phase that holds both. The Layout put `NAME`
somewhere else.

## NGA5253

`foreign` says that another master of the address space reads the Section and
the CPU does not, as a Lynx's display reads its buffer. It lets the Section lie
over a `register` or `reserved` Region. Raised when such a Section holds bytes
and reaches one of those Regions. The bytes would be written by the CPU, through
a loader or a Transition's copy, and at those addresses the CPU sees a chip's
registers or a ROM, not the memory.

```ngp
target {
  region     $0080 .. $00FF  ram
  region     $0200 .. $FBFF  ram
  region io  $FC00 .. $FFFF  register
}

modules { "main.asm" }
```

```asm
.section absolute at $E000, root, foreign
screen  .byte $11
        .res 8159
.ends

.proc entry
        rts
.endp
```

Make the Section a reservation, with `.res` only, and let the program or the
hardware fill it at run time. A `foreign` Section with bytes is allowed where
all of it lies in `ram`.

## NGA5254

An instruction reads or writes a `foreign` Section at an address where a
`register` or `reserved` Region stands. The access goes to what the CPU sees
there, such as a chip's registers, and not to the Section's memory. The check is made on the address
the operand names, once the Section has its address. What an index register
adds at run time is not checked, nor is an access through a pointer, and taking
the address with `#<` is not an access.

```ngp
target {
  region     $0080 .. $00FF  ram
  region     $0200 .. $FBFF  ram
  region io  $FC00 .. $FFFF  register
}

modules { "main.asm" }
```

```asm
.section absolute at $E000, root, foreign
screen  .res 8160
.ends

.proc entry
        lda #0
        sta screen + 8000
        rts
.endp
```

`screen + 8000` is `$FF40`, inside `io`. The CPU may use the part of the
Section below `$FC00`, and the rest is the other master's. The same rule for
the Section's own bytes is [`NGA5253`](#nga5253).

## NGA5255

Raised on a Commodore diskette, a `d64`, `d80` or `d82`, for a Section that
waits in storage and whose stored form is larger than the widest track. A unit
there is a track, tracks hold different numbers of sectors, and nothing may run
from one track into the next.

A driver that declares `.driver span none` has such a form cut into pieces that
each fit in a track. The shipped `pet/disk.asm` declares it. A driver of your
own for a diskette that does not declare it gets the form whole, and no track
holds it. Add `.driver span none` to the driver.

## NGA5256

A finding of `--verify-layout`, and a fault of the tool, not of the program;
see [`NGA5240`](#nga5240). The Layout put a Section that is in no Pane inside a
Window, where only a Pane's Sections and storage stand. A pin there is refused
as [`NGA5235`](#nga5235).

## NGA5258

A `.xex` with no storage carries a Section named in a `transform` in its
encoded form, and decodes it in place while the file loads. Raised when the
encoded form is no shorter than the Section's bytes. The `.xex` then carries
the bytes as they are. The program is the same; only the compression is lost.
Also raised for a Section the `.xex` moves under a ROM.

```ngp
include "atari/800xl.ngp"

modules { "main.asm" }

transform zx0 { main.table }

container xex
```

```asm
.proc entry
        lda table
        rts
.endp

.section
table   .byte 1, 2, 3, 4
.ends
```

Short data, and data with nothing repeated, rarely shrink. Take the Section out
of the `transform` block.

## NGA5259

The setting of [`NGA5258`](#nga5258). To decode in place, the loader writes the
encoded form so that it ends a few bytes past the end of the Section, and the
decoder writes the Section from its start without overtaking what it has still
to read. Those bytes past the end must be memory the loader may write while the
file loads: in a `ram` Region, and not held by a `root` reservation of the entry
Phase, such as the OS ROM, or by a Module of the `load` block. Raised when they
are not. The `.xex` then carries the Section's bytes as they are.

```ngp
include "atari/800xl.ngp"

modules { "main.asm" }

transform zx0 { main.table }

container xex
```

```asm
.proc entry
        lda table
        rts
.endp

.section absolute at $BFE0
table   .byte 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
        .byte 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
.ends
```

The table ends at `$BFFF`, and the OS ROM starts at `$C000`. Pin the Section
lower, or drop the `at` and let the solver place it.

## NGA5260

A `.xex` with no storage runs code while it loads: the decoder of each format a
`transform` names, the move of bytes kept under a ROM, and the code of the
`load` block's Modules. The tool places that code in memory that is free while
the file loads: clear of every Section being decoded, of every `root`
reservation of the entry Phase, and of what the `load` block says the machine
holds. The rest of the program is no obstacle, since it is written after this
code has run. Raised when no free stretch holds what the message names.

```ngp
target {
  region $0080 .. $00FF  ram
  region $2000 .. $2FFF  ram
}

modules { "main.asm" }

transform zx0 { main.table }

container xex
```

```asm
.proc entry
        lda table
        rts
.endp

.section
table   .byte 1
        .res 4000
        .byte 2
.ends
```

The table takes 4002 of the 4096 bytes of RAM, and the zx0 decoder does not
fit in the rest. Leave memory free while the program loads: decode less, or
reserve less in the entry Phase.

## NGA5261

A `.xex` with storage brings the Sections of its entry Phase that a `transform`
names by a cold start: an edge into the entry Phase from nowhere, which reads
them from storage and decodes them before the entry Label runs. The driver and
the decoders run during the cold start, so they are in memory before the first
block is read, and nothing could decode one of their own Sections. Raised for a
Section of the driver's Module, or of a Module that declares a decoder, named
in a `transform`.

```ngp
include "atari/130xe.ngp"
include "atari/extension.ngp"

modules { "main.asm"  "unpack.asm" }

transform rle { main.table, unpack.unpack }

container xex
```

```asm
.proc entry
        lda table
        rts
.endp

.section
table   .byte 0, 0, 0, 0, 0, 0, 0, 0
.ends
```

```asm unpack.asm
.transform rle unpack

.proc unpack
        rts
.endp
```

Take the Section out of the `transform` block, and the `.xex` writes it as it
is. A cartridge's cold start is described in
[Cartridges](../14-cartridges.md#the-cold-start).

## NGA5262

A `.xex` with no storage can keep bytes for a later Phase in the RAM under a
ROM, while the entry Phase has that ROM switched in. The file loads with every
ROM in, so the `.xex` first writes those bytes into memory that is free while
it loads, and then moves them under the ROM with every ROM switched out. Raised
when no free stretch is large enough to hold any of them. Free means clear of
every `root` reservation of the entry Phase, of what the `load` block says the
machine holds, and of the driver and the tool's own load code. Reserve less in
the entry Phase, so that some memory is free while the program loads.

## NGA5263

The setting of [`NGA5262`](#nga5262), for a Section a `transform` names. Its
encoded form is written whole into one free stretch and decoded from there
under the ROM. Raised when the form is larger than the largest free stretch.
The Section is then moved as it is, in pieces, and the `.xex` is larger by what
the encoding would have saved. Reserve less in the entry Phase, or take the
Section out of the `transform` block.

## NGA5264

`in NAME` with a Region puts the Section inside that Region, and `at` pins it.
Raised where a Section has both and its pinned bytes do not lie wholly inside
any Region its `in` names. The tool does not choose between the two.

```ngp
include "atari/800xl.ngp"

modules { "main.asm" }
```

```asm
.section absolute at $B000, in underOsLow
table   .byte 1, 2, 3, 4
.ends

.proc entry
        lda table
        rts
.endp
```

`underOsLow` is `$C000-$CFFF`. Drop the `at` and the solver places the Section
in the Region, or pin it inside the Region. See
[`in NAME`](../16-assembler.md#in-name).

## NGA5265

A Module the `load` block `needs` that holds anything but pinned reservations
is code of the program's own, which runs while the `.xex` loads. The tool
places it in memory that is free during the load and writes the rest of the
file over it afterwards, so it belongs to no Phase. Raised when a Phase also `needs` the
Module, or `resident` names it.

```ngp
modules { "main.asm"  "early.asm" }

load { needs early  init early }

phase main { needs main, early }
entry main

container xex
```

```asm
.proc entry
        rts
.endp
```

```asm early.asm
.proc early
        rts
.endp
```

Take the Module out of the Phase's `needs`. See
[`load`](../15-project-file.md#load).

## NGA5266

A Module the `load` block `needs` that holds only pinned reservations says what
memory the machine uses while the `.xex` loads, such as the DOS's. Place keeps
every Section with bytes off it. Raised for a Section pinned there whose bytes
the `.xex` writes during the load, onto memory the DOS may still be using. It
is a warning because a pin is the author's word: writing `INITAD`, in the OS's
memory, is how a program talks to the DOS.

```ngp
modules { "main.asm"  "dos.asm" }

load { needs dos }

phase main { needs main }
entry main

container xex
```

```asm
.section absolute at $2400
flag    .byte 1
.ends

.proc entry
        lda flag
        rts
.endp
```

```asm dos.asm
.section absolute at $2000
dos     .res $1000
.ends
```

Where the pin is not meant, move it or drop it. A Section with no bytes may
stand there without a warning, since the DOS is done with the memory by the
time the program writes it.

## NGA5267

A finding of `--verify-layout`, and a fault of the tool, not of the program;
see [`NGA5240`](#nga5240). The `.xex` writes this Section's bytes while it
loads, and the Layout put it on memory that a Module of the `load` block says
the machine holds then. A pinned Section there is a warning instead,
[`NGA5266`](#nga5266).

## NGA5268

`init NAME` in the `load` block names a Label that the `.xex` calls through
`INITAD` while it loads. The name is looked up among the Modules the same block
`needs`, as `entry` is among a Phase's. Raised when none of them defines it,
when more than one does, or when it is not a Label; the message says which.

```ngp
modules { "main.asm"  "early.asm" }

load { needs early  init fadeIn }

phase main { needs main }
entry main

container xex
```

```asm
.proc entry
        rts
.endp
```

```asm early.asm
.proc fade
        rts
.endp
```

Correct the name, or add the Module that defines it to the block's `needs`.

## NGA5269

A Section `in` a unit set that declares `linear` stands in the medium's own
memory, at a linear address, and not at an address the CPU uses. Its `at` is a
linear address too, and is held to the set's Banks: from the start of the
`linear` range to the end of the last Bank. Raised when the pinned bytes fall
outside. An `at` written as a CPU address, such as the Window's, is the usual
cause.

```ngp
target {
  region $0080 .. $00FF  ram
  region $2000 .. $3FFF  ram
  units  vram 4  linear $10000 .. $1FFFF
  window vwin $4000 .. $7FFF  views vram
}

modules { "main.asm" }
```

```asm
.section absolute at $4000, in vram
sprites .res 256
.ends

.proc entry
        lda #<sprites
        rts
.endp
```

Pin the Section at a linear address in the range, such as `$10000`, or drop
the `at`.

## NGA5270

Before the solver runs, the tool adds up, for each Phase, the Sections that
stand in a unit set and the Sections of Panes in that set's Banks, a family's
once per member. Raised when the sum is more than the set's Banks hold end to
end. The solver is asked all the same, so [`NGA5206`](#nga5206) comes with it.

```ngp
target {
  region $0080 .. $00FF  ram
  region $2000 .. $3FFF  ram
  units  vram 4  linear $10000 .. $1FFFF
  window vwin $4000 .. $7FFF  views vram
}

modules { "main.asm" }
```

```asm
.section in vram
screen  .res $C000
.ends

.section in vram
copy    .res $5000
.ends

.proc entry
        lda #<screen
        lda #<copy
        rts
.endp
```

Four Banks of `$4000` hold `$10000` bytes. Make the Sections smaller, or put
them in Modules of different Phases, since the sum is taken per Phase.

## NGA5271

A Section `in` a unit set that holds bytes is written into the set by the
Container while the program loads. A Container fills one set: the one storage
is made of, or, with no storage, the one the driver's stream reads through. Any
other set holds only reservations. Raised for a Section with bytes in such a
set.

```ngp
target {
  region $0080 .. $00FF  ram
  region $2000 .. $3FFF  ram
  units  vram 4  linear $10000 .. $1FFFF
  window vwin $4000 .. $7FFF  views vram
}

modules { "main.asm" }
```

```asm
.section in vram
palette .byte $00, $0F, $F0, $FF
.ends

.proc entry
        lda #<palette
        rts
.endp
```

Make the Section a reservation, with `.res`, and have the program fill it at
run time.

## NGA5272

A Pane's Section with bytes is written into the Pane's Bank by the Container
while the program loads. The Container fills one set of units: the one storage
is made of, or, with no storage, the one the driver's stream reads through.
Raised for a Section with bytes in a Pane of any other set. With no storage
and no driver, no set is filled at all.

```ngp
target {
  units  extension 4
  window ext  $4000 .. $7FFF  views extension
}

modules { "main.asm" }

panes in ext { level }

container xex
```

```asm
.section in level, root
map     .byte 1, 2, 3
.ends

.proc entry
        rts
.endp
```

A `target` written by hand needs a Module that declares a driver, with `.driver
stream ext`, as `atari/130xe.ngp` lists `atari/portb.asm`. Or make the Section a
reservation and have the program fill it at run time.

## NGA5273

A `.xex` built from `choose` writes, before each call that changes the bytes,
the list of what to change: an address and a byte for each later document. The
list goes into memory the program's own bytes will fill later, into memory
nothing writes, or into the program's bytes where nothing changes and which
are written again after the call. Here every byte of the table changes and
little else of memory is there.

```ngp
include "small.ngp"

choose { "low.ngp" "high.ngp" }

modules { "detect.asm"  "main.asm" }
load { needs detect  init detect }

phase main { needs main }
entry main

container xex
```

```ngp small.ngp
target {
  region   $0080 .. $00FF  ram
  region   $2000 .. $27FF  ram
  region   $D000 .. $D7FF  register
}
```

```ngp low.ngp
constants { LEVEL = 1 }
```

```ngp high.ngp
constants { LEVEL = 2 }
```

```asm detect.asm
.proc detect
        lda #1
        sta ngaChoice
        rts
.endp
```

```asm
.macro sites n
.if n > 0
        .byte LEVEL, LEVEL, LEVEL, LEVEL, LEVEL, LEVEL, LEVEL, LEVEL
        sites n - 1
.endif
.endm

.proc entry
        rts
.endp

.section root
table
        sites 80
.ends
```

Make the bytes that differ fewer, for example by keeping the value in one
place and reading it from there.

## NGA5631

Before the solver runs, the tool adds up, for each Phase, the `absolute`
Sections the Phase holds, and compares the sum with the memory outside every
Window, which is the View `fixed`. Raised for a Phase whose sum is larger. The
notes name the three Sections that weigh most. The solver is asked all the
same, so [`NGA5206`](#nga5206) comes with it.

```asm
.section
level   .res 30000
.ends

.section
tiles   .res 20000
.ends

.proc entry
        lda level
        lda tiles
        rts
.endp
```

Make the Phase hold less: split the program into Phases whose Modules arrive
from storage by a Transition ([Phases](../04-phases.md)), or put data in a Pane
behind a Window ([Panes](../06-panes.md)). With no storage, a Section stays in memory from the
load until the last Phase that needs it, and counts in every Phase on the way;
its note is then [`NGA5634`](#nga5634).

## NGA5632

The same sum as [`NGA5631`](#nga5631), for the `zeropage` Sections of a Phase,
against the zero page the Target's `ram` Regions leave. The stand-in Target has
16 bytes of it, `$0080-$008F`. A machine variant declares its own.

```asm
.section zeropage
pointers .res 12
.ends

.section zeropage
counters .res 8
.ends

.proc entry
        lda pointers
        lda counters
        rts
.endp
```

Take `zeropage` off Sections that do not need it. Scratch bytes can be
Temporaries, declared with `.ztemp`, which share an address with others that
are never live at the same time ([Temporaries](../02-temporaries-and-liveness.md)).

## NGA5633

Follows [`NGA5631`](#nga5631), [`NGA5632`](#nga5632) or
[`NGA5234`](#nga5234), once for each of the three Sections that weigh most in
the sum, largest first, and points at the Section.

## NGA5634

Follows [`NGA5631`](#nga5631) or [`NGA5632`](#nga5632) in place of
[`NGA5633`](#nga5633), for a Section the Phase does not need. With no storage,
bytes are written when the program loads and stay in memory while a Phase still
to come needs them, so this Phase holds them for the Phase the note names. With
a [`storage`](../15-project-file.md#storage) block they would wait there and
arrive by a Transition.

## NGA6101

Raised when the tool writes the bytes: an operand of an instruction, or an item
of `.byte` or `.word`, has a value the place does not hold. A byte holds -128 to
255 and a word -32768 to 65535. A negative value is written as its two's
complement, so `.byte -1` is `$FF`.

Here it is an address where a byte is meant: `table` is at `$2000`, and
`#table` asks for all of it in one byte.

```asm
.section
table   .byte 1, 2, 3
.ends

.proc entry
        lda #table
        rts
.endp
```

Write `#<table` or `#>table` for one half of it. See
[the operators](../16-assembler.md#operators).

## NGA6102

A branch reaches 128 bytes back and 127 forward. The tool says how far the
target actually is, and names the conditional jump that reaches anywhere. It is
five bytes where a branch is two.

```asm
.macro zeros n
.if n > 0
        .byte 0
        zeros n - 1
.endif
.endm

.proc entry
        bne far
        zeros 200
far     rts
.endp
```

`jne` is two bytes where the target is close enough and five where it is not,
and the tool picks; see [Instructions](../12-instructions.md).

## NGA6201

Raised when the Container is the raw image, the default where a Project names
none, and two Sections with bytes stand at one address. Place allows that where
no Phase needs both, since the two are never in memory together. A raw image is
the memory at one instant, and it has room for one of them. Here the solver put
the two entries at one address by itself.

```ngp
include "atari/130xe.ngp"
include "atari/extension.ngp"

modules { "intro.asm" "game.asm" }

phase intro { needs intro  then game  entry introStart }
phase game  { needs game              entry gameStart }
entry intro
```

```asm intro.asm
.proc introStart
        .transition game
.endp
```

```asm game.asm
.proc gameStart
@stop   jmp @stop
.endp
```

Name the Container the machine loads, here `container xex`. The `.xex` keeps
`gameStart` in storage and the Transition brings it in. See
[`container`](../15-project-file.md#container).

## NGA6204

Raised when a `.xex` has bytes to put into Banks, a Pane's Sections or what waits
in storage, and no Module declares a driver with `.driver`. The `.xex` fills a
Bank while it loads: it calls the driver's `showAt` to show the Bank in the
Window and writes the bytes through it. See
[Banks and Windows](../05-banks-and-windows.md#what-the-tool-did-differently).

```ngp
target {
  units  extension 4
  window ext  $4000 .. $7FFF  views extension
}
storage { units extension }

modules { "main.asm" }

panes in ext { level }

container xex
```

```asm
.section in level, root
map     .byte 1, 2, 3
.ends

.proc entry
        rts
.endp
```

The Pane's Section is kept by `root`, and no code reads it. A program that takes
a `.transition` or writes a `.with` is told earlier, by [`NGA2281`](#nga2281) or
[`NGA2313`](#nga2313). This one is seen where no code uses the driver yet.
Without storage, the same Pane is [`NGA5272`](#nga5272): with no driver there
is no stream, so no set of units is filled at all. The
library's variants list their driver, as `atari/130xe.ngp` lists
`atari/portb.asm`. A `target` written by hand needs a Module that declares one.

## NGA6205

Raised when a `.xex` has bytes to put into Banks or storage and the driver
declares no `.driver stream WINDOW`. The `.xex` fills a unit by showing it in
that Window and writing into it. A driver that reads its medium some other way
leaves it nothing to write into. `atari/disk.asm` is one: it reads sectors
through the OS.

```ngp
include "atari/800xl.ngp"
include "atari/disk.ngp"

modules { "intro.asm" "level.asm" }

phase intro { needs intro  then level }
phase level { needs level }
entry intro

container xex
```

```asm intro.asm
.proc entry
        .transition level
.endp
```

```asm level.asm
.section
map     .byte 1, 2, 3
.ends

.proc entry
        lda map
@stop   jmp @stop
.endp
```

Storage on a diskette goes with `container atr`, as in
[Phases](../04-phases.md#the-project-says-which-part-is-which). A `.xex` takes
storage in Banks, such as `atari/extension.ngp` beside `atari/130xe.ngp`.

## NGA6207

Raised when the driver declares `.driver init` and the Container is neither a
`.xex` nor a raw image, which nothing of the tool starts.
`init` is what the hardware needs before the driver can use it, such as a board
on the parallel bus that answers at none of its addresses until it is selected.
The `.xex` calls it through `INITAD` before it fills a unit and before the
entry. The other Containers have no place that calls it, and the driver's writes
would then go to hardware that does not answer.

```ngp
include "atari/800xl.ngp"

modules { "main.asm" }

container atr
```

```asm
; A board that answers on the bus only once it is selected.
.driver init wake

.macro wake
        lda #1
        sta $D1FF
.endm

.proc entry
@stop   jmp @stop
.endp
```

Build a `.xex` where the machine takes one.

## NGA6208

The `.xex` calls the driver's `init` at `ngaInit`, a Proc the tool writes for
that call, as a Root. Raised when that Proc has no address once the program is
placed. It is a check of the tool's own work, and nothing a Project declares is
meant to reach it.

## NGA6211

Raised when the Container has to say where the program starts and the entry
Phase has no Label of that name in the Modules it needs. The name is the one
`entry NAME` gives in the Phase's `phase` block, and `entry` where the block
gives none. A Project with no `phase` block has one Phase, shown as
`(implicit)`, and its entry is a Label named `entry`.

```ngp
modules { "main.asm" }

container xex
```

```asm
.proc start
        rts
.endp
```

Name the Label `entry`, or add a `phase` block that names it,
`phase main { needs main  entry start }`, with `entry main` beside it. The Label
may be private to its Module. The raw image is not entered by anything, so it
does not ask. See [`phase`](../15-project-file.md#phase).

## NGA6212

Raised when more than one Module the entry Phase needs defines a Label of the
entry's name. Each Module may have a private `entry` of its own, and then
nothing says which one the Phase starts at. The finding points at the first.

```ngp
modules { "main.asm" "other.asm" }

container xex
```

```asm
.proc entry
        rts
.endp
```

```asm other.asm
.proc entry
        rts
.endp
```

Rename all but one, or give the Phase an `entry NAME` that one Module alone
defines.

## NGA6213

Raised when the name the entry Phase starts at is defined once, and as something
other than a Label, such as a Constant. An entry is a place in the program, and a
Label is what names one. A Constant does not, even when its value is an address,
as here.

```ngp
modules { "main.asm" }

container xex
```

```asm
entry = $2000
```

Define the entry as a Label, or rename the other definition and say which Label
the Phase starts at with `entry NAME`.

## NGA6214

Raised when the driver's stream reads through a Window of more than one range
and a `.xex` has to fill a unit through it. The `.xex` writes a unit as one run
of memory, and a Window of two ranges is not one.

```ngp
target {
  units  pages 4
  window split  $4000 .. $4FFF, $6000 .. $6FFF  views pages
}

modules { "main.asm" }

panes in split { level }

container xex
```

```asm
.driver open    boardOpen
.driver read    boardRead
.driver unit    boardUnit
.driver offset  boardAt
.driver stream  split
.driver show    split boardShow
.driver showAt  split boardShowAt

.macro boardOpen
.endm
.macro boardRead
.endm
.macro boardShow state
.endm
.macro boardShowAt
.endm

.section zeropage
boardAt   .res 2
boardUnit .res 1
.ends

.section in level
map     .byte 1, 2, 3
.ends

.proc entry
        .with level
        lda map
        rts
.endp
```

The driver's macros are empty here because only the Window matters. Stream
through a Window of one range.

## NGA6221

Raised when the Container is `atr` and the `storage` block names a unit set,
`units NAME`. Storage on a diskette is its sectors, which the driver reads one
at a time through the OS. A unit set is Banks shown through a Window, and the
boot record that loads the program shows no Window to fill them through.

```ngp
include "atari/800xl.ngp"

target {
  units  extension 4
  window ext  $4000 .. $7FFF  views extension
}
storage { units extension }

modules { "main.asm" }

container atr
```

```asm
.proc entry
@stop   jmp @stop
.endp
```

Storage on a diskette is declared by count: include `atari/disk.ngp`, which
declares it and lists the driver that reads it. Banks go with `container xex`.
A driver that streams through the Window is refused as well,
[`NGA6223`](#nga6223).

## NGA6222

Raised when the Container is `atr` and a unit of storage is not 65536 bytes. A
unit is 256 sectors of 256 bytes. The unit's number is then the high byte of the
sector number and the offset is the rest, so the driver does no arithmetic.

```ngp
include "atari/800xl.ngp"

storage { units 720  size 256 }

modules { "main.asm" }

container atr
```

```asm
.proc entry
@stop   jmp @stop
.endp
```

Include `atari/disk.ngp` rather than writing the block. It declares three units
of 65536 bytes from sector 4 on, the first sector after the boot record.

## NGA6223

Raised when the Container is `atr` and the driver declares
`.driver stream WINDOW`. A driver that streams through a Window is one for
memory the CPU sees, such as Banks. The boot record of an `.atr` reads sectors
and shows no Window.

```ngp
include "atari/800xl.ngp"

target {
  units  pages 4
  window ext  $4000 .. $7FFF  views pages
}

modules { "main.asm" }

container atr
```

```asm
.driver open    boardOpen
.driver read    boardRead
.driver unit    boardUnit
.driver offset  boardAt
.driver stream  ext
.driver show    ext boardShow
.driver showAt  ext boardShowAt

.macro boardOpen
.endm
.macro boardRead
.endm
.macro boardShow state
.endm
.macro boardShowAt
.endm

.section zeropage
boardAt   .res 2
boardUnit .res 1
.ends

.proc entry
@stop   jmp @stop
.endp
```

A diskette's driver is `atari/disk.asm`, which `atari/disk.ngp` lists. A driver
that streams through a Window goes with `container xex`. Storage in Banks on an
`.atr` is [`NGA6221`](#nga6221), raised beside this one when the driver streams
through those Banks.

## NGA6224

Raised when an `.atr` would need more than 65535 sectors: the boot record, the
storage the program uses, and the load image after it. The OS's disk call takes
a sector number in two bytes, so no image holds more. The tool makes an image of
720 sectors, a diskette, and grows it past that as far as this limit.

Make the Payloads smaller with a `transform`, such as `zx0`, or keep less of the
program in storage.

## NGA6225

Raised for a Section the `.atr` would load into `$0700`-`$087F`. The OS reads
the boot record there, and the loader in it then reads the rest of the program.
A byte loaded there would overwrite the loader while it runs.

```ngp
include "atari/800xl.ngp"

modules { "main.asm" }

container atr
```

```asm
.section absolute at $0800
table   .byte 1, 2, 3
.ends

.proc entry
        lda table
@stop   jmp @stop
.endp
```

`atari/800xl.ngp` leaves those addresses out of every Region, so the solver
never puts a Section there, and the cause is a pin. Move it above `$087F`, or
drop it. A `target` written by hand for an `.atr` has to leave the range out of
its `ram` Regions too, or the solver itself puts Sections there.

## NGA6226

The tool writes the boot record of an `.atr` itself, as a Module of its own, and
fills in the sector the load image starts at. Raised when that Module, or the
place in it the sector goes, is missing or outside the record. It is a check of
the tool's own work, and nothing a Project declares is meant to reach it.

## NGA6227

The boot record is the first three sectors of the diskette, 384 bytes the OS
reads to `$0700`. Raised when the loader the tool writes there comes to more.
The loader is the tool's own and fits with room to spare, so this is a check of
the tool's work rather than of the program's.

## NGA6231

Raised when what waits in storage reaches past the end of the cartridge image.
On a board with Banks, storage is the Banks the board switches, and the
`container car` line holds the Target's unit set to the board's count
([`NGA1221`](#nga1221)). The library's variants therefore never get here. A
`storage` block written by hand can declare more than the board holds; declare
the storage the board has.

## NGA6232

Raised for a Section with bytes that stands outside the cartridge's ROM and that
nothing copies there. Nothing loads a cartridge: the CPU reads it where it
lies. On a board with Banks, the bytes of a Section in RAM wait in a Bank and the
cold start copies them, as in
[Cartridges](../14-cartridges.md#the-cold-start). A board without Banks, `8k` or
`16k`, has nowhere for them to wait.

```ngp
target {
  containers car
  region $0080 .. $00FF  ram
  region $2000 .. $2FFF  ram
  region $A000 .. $BFFF  rom
  holder cartinit
}

modules { "main.asm" }

container car "8k"
```

```asm
.section
lives   .byte 3
.ends

.proc entry
        dec lives
@stop   jmp @stop
.endp
```

Reserve the byte with `.res 1` and set it from code, or use a board with Banks.
A Section that nothing writes goes into ROM by itself. A Proc whose bytes the
program writes is kept out of ROM, and is refused here the same way.

## NGA6233

Raised when the tool cannot complete the six bytes at the top of a cartridge,
`$BFFA`-`$BFFF`, which the OS reads to start it. The first two hold where the
program starts. Where the entry Phase has no entry Label, the tool has nothing
to write there, and this comes with [`NGA6211`](#nga6211),
[`NGA6212`](#nga6212) or [`NGA6213`](#nga6213).

```ngp
target {
  containers car
  region $0080 .. $00FF  ram
  region $2000 .. $7FFF  ram
  region $A000 .. $BFFF  rom
  holder cartinit
}

modules { "main.asm" }

container car "8k"
```

```asm
.proc start
        rts
.endp
```

Fix the entry and both findings go.

## NGA6234

Raised when the Container is `bs93` and the entry Label is not the image's
lowest byte. The header of a `bs93` holds one address, where the block is loaded
and where it is entered, so the program has to start at its first byte. The tool
puts no `jmp` in front, because that would move the address the header holds.

```ngp
include "lynx/lynx.ngp"

modules { "main.asm" }

container bs93
```

```asm
.section
colors  .byte 1, 2, 3
.ends

.proc entry
        lda colors
@stop   bra @stop
.endp
```

The solver put `colors` first. Pin the entry below everything else, as
`.proc entry, absolute at $0200`.

## NGA6235

Raised when the Container is `bs93` and a Section has a Payload, which waits in
storage for a Transition to load it. An emulator copies a `bs93` block into
memory and enters it, and nothing ever reads storage.

```ngp
include "lynx/lynx.ngp"
include "lynx/cart64k.ngp"

modules { "intro.asm" "level.asm" }

phase intro { needs intro  then level  entry introStart }
phase level { needs level              entry levelStart }
entry intro

container bs93
```

```asm intro.asm
.proc introStart
        .transition level
.endp
```

```asm level.asm
.section
map     .byte 1, 2, 3
.ends

.proc levelStart
        lda map
@stop   bra @stop
.endp
```

Leave out the storage document, here `lynx/cart64k.ngp`. Without storage every
Section with bytes is in the block, and a Transition loads nothing. Or build a
cartridge, `container lnx "64k"`, which has a loader.

## NGA6236

A Lynx cartridge boots in two parts. The boot ROM decrypts fifty bytes from the
start of the cartridge and runs them, and they read the rest of the loader,
which reads the program. The tool writes both parts itself, as a Module of its
own, and fills in two operands of the second part. Raised when that Module or
those operands are missing. It is a check of the tool's own work, and nothing a
Project declares is meant to reach it.

## NGA6237

The first part of a Lynx cartridge's loader is the one block the boot ROM
decrypts, fifty bytes run at `$0200`. Raised when the tool's own first part
comes to more. It is a check of the tool's work rather than of the program's.

## NGA6238

The second part of a Lynx cartridge's loader stands in the rest of page zero,
after the first part. The first part reads it from where the boot ROM's counter
stopped, and the counter does not run on into the next page, so the loader has
to fit in that page: 204 bytes on a `64k` board. Raised when it does not. The
loader is the tool's own and is written to fit the smallest board, so this is a
check of the tool's work.

## NGA6239

Raised when a Lynx cartridge would need more bytes than the board holds. Page
zero holds the loader, storage follows from page one, and the load image, which
is everything the entry Phase needs, begins on the page after the storage the
program uses. The message counts all three. Here the entry Phase carries 40000
bytes and the other Phase's 30000 wait in storage.

```ngp
include "lynx/lynx.ngp"
include "lynx/cart64k.ngp"

modules { "intro.asm" "level.asm"
  script( "fill.js", name = "title", size = 40000 ) as titleArt
  script( "fill.js", name = "map", size = 30000 ) as levelArt
}

phase intro { needs intro, titleArt  then level  entry introStart }
phase level { needs level, levelArt              entry levelStart }
entry intro

container lnx "64k"
```

```js fill.js
export default ( args ) => ( {
  sections: [ { name: args.name, items: [ new Uint8Array( args.size ) ] } ]
} );
```

```asm intro.asm
.proc introStart
        lda title
        .transition level
.endp
```

```asm level.asm
.proc levelStart
        lda map
@stop   bra @stop
.endp
```

Name a larger board, `"128k"`, `"256k"` or `"512k"`, and include its storage
document in place of `lynx/cart64k.ngp`. Or make the Payloads smaller with a
`transform`.

## NGA6240

Raised when the Container is `prg` and the image begins below `$040E`. `LOAD`
puts a `.prg` at BASIC's start, `$0401`, and the tool puts a line of BASIC there
that enters the program with `SYS`. The program's own bytes begin behind that
line. A Section with bytes further down would have to be loaded over BASIC's
pointers by the load itself.

```ngp
include "pet/3032.ngp"

modules { "main.asm" }

container prg
```

```asm
.section zeropage
speed   .byte 4
.ends

.proc entry
        lda speed
        rts
.endp
```

`pet/3032.ngp` leaves `$0200`-`$040D` out of every Region, so the solver keeps
out of it. The cause is a pin or, as here, a `zeropage` Section with bytes.
Reserve the byte with `.res 1` and set it from code.

## NGA6241

Raised when the Container is `prg` and a Section has a Payload, which waits in
storage for a Transition to load it. A `.prg` is one block a PET is given with
`LOAD`, and it has no loader to read storage.

```ngp
include "pet/3032.ngp"
include "pet/d64.ngp"

modules { "intro.asm" "level.asm" }

phase intro { needs intro  then level  entry introStart }
phase level { needs level              entry levelStart }
entry intro

container prg
```

```asm intro.asm
.proc introStart
        .transition level
.endp
```

```asm level.asm
.section
map     .byte 1, 2, 3
.ends

.proc levelStart
        lda map
@stop   jmp @stop
.endp
```

Build the same program as `container d64`, the diskette whose other sectors a
Transition reads. Or leave out the storage document, here `pet/d64.ngp`: every
Section then stays in memory, and a Transition loads nothing.

## NGA6242

Raised when a Commodore diskette, `d64`, `d80` or `d82`, has too few free
sectors left for the program's own file. The Phases' storage is written first.
The program, the file `LOAD` reads, then goes into what is left as a chain of
sectors, 254 bytes of it in each. The message counts the sectors the program
needs and those left.

Make the Payloads smaller with a `transform`, or use a larger surface, `d80` or
`d82`, with the storage document of that surface.

## NGA6243

Raised when the Container is `prg`, `d64` or `bs93` and the entry Phase has no
Section that writes a byte. Each of them is one block that a machine loads and
then enters, and a block of nothing has nothing to enter.

```ngp
modules { "main.asm" }

container prg
```

```asm
.proc entry
.endp
```

Give the entry Phase the code it runs. A raw image of the same program is an
empty file, which is all such a program is, so a raw image is not refused.

## NGA6244

Raised when a Container came out with no bytes and nothing the tool checked
said why. Every other refusal of a Container names its reason. This one stands
for the places where writing stops on something an earlier Step should have
ruled out, such as a Label the tool declared itself and then cannot find. It is
a check of the tool's own work, and nothing a Project declares is meant to reach
it. Report it with the Project that raised it. Without this check the tool would
write an empty file and report success.

## NGA6245

A document that `choose` names may change bytes and nothing else, because the
program is placed once for all of them. Here a `.if` takes a branch of another
length under the second document, so `entry` would be one byte longer.

```ngp
include "atari/800xl.ngp"

choose { "low.ngp" "high.ngp" }

modules { "detect.asm"  "main.asm" }
load { needs detect  init detect }

phase main { needs main }
entry main

container xex
```

```ngp low.ngp
constants { LEVEL = 1 }
```

```ngp high.ngp
constants { LEVEL = 2 }
```

```asm detect.asm
.proc detect
        lda #1
        sta ngaChoice
        rts
.endp
```

```asm
.proc entry
.if LEVEL == 1
        lda #1
.else
        lda #1
        ora #2
.endif
        sta level
        rts
.endp

.section
level   .res 1
.ends
```

Make both branches the same length, or decide at run time what the `.if`
decided.

## NGA6246

Nothing is chosen until the first `init` of the `load` block returns, so the
code it runs still holds the first document's bytes. Code that finds the
hardware names the places it looks at by numbers or by registers no document
declares.

```ngp
include "atari/800xl.ngp"

choose { "low.ngp" "high.ngp" }

modules { "detect.asm"  "main.asm" }
load { needs detect  init detect }

phase main { needs main }
entry main

container xex
```

```ngp low.ngp
target { register PORT $D640 }
```

```ngp high.ngp
target { register PORT $D740 }
```

```asm detect.asm
.proc detect
        lda PORT
        and #1
        sta ngaChoice
        rts
.endp
```

```asm
.proc entry
        sta PORT
        rts
.endp
```

Read `$D640` and `$D740` by their numbers in the code that chooses. Code that
uses the chosen registers while the file loads can be a second `init`, which
runs after the bytes are changed.

## NGA6247

The bytes a document changes are changed where the load has them in memory as
they are. A section carried encoded and decoded under a ROM is never there, so
its bytes cannot be changed.

```ngp
include "atari/800xl.ngp"
include "atari/roms.ngp"

choose { "low.ngp" "high.ngp" }

modules { "detect.asm"  "intro.asm"  "game.asm"  "art.asm" }
load { needs detect  init detect }

phase intro { needs intro                                then game  entry introStart }
phase game  { needs game, art  without osrom, mathpack              entry gameStart }
entry intro

transform zx0 { art.art }

container xex
```

```ngp low.ngp
constants { LEVEL = 1 }
```

```ngp high.ngp
constants { LEVEL = 2 }
```

```asm detect.asm
.proc detect
        lda #1
        sta ngaChoice
        rts
.endp
```

```asm intro.asm
.proc introStart
        .transition game
.endp
```

```asm game.asm
.proc gameStart
        lda art
        sta seen
        rts
.endp

.section
seen    .res 1
.ends
```

```asm art.asm
.export art

.macro rows n
.if n > 0
        .byte 1, 2, 3, 4, 1, 2, 3, 4, 1, 2, 3, 4, 1, 2, 3, 4
        rows n - 1
.endif
.endm

.section in underOsLow
art     .byte LEVEL
        rows 16
.ends
```

Take the section out of the `transform`, or keep the bytes that differ in a
section of their own outside it.

## NGA6248

Nothing that runs while the file loads names `ngaChoice`, so the byte stays
zero and the file always loads as the first document. The program is built,
and the other documents are carried for nothing.

```ngp
include "atari/800xl.ngp"

choose { "low.ngp" "high.ngp" }

modules { "main.asm" }
container xex
```

```ngp low.ngp
constants { LEVEL = 1 }
```

```ngp high.ngp
constants { LEVEL = 2 }
```

```asm
.proc entry
        lda #LEVEL
        sta level
        rts
.endp

.section
level   .res 1
.ends
```

Give the `load` block an `init` that finds out which document the machine needs
and writes its index into `ngaChoice`.

## NGA7001

The lexer found a character, outside a comment or a literal, that no token of
C begins with: `$`, `@`, a backtick, a `\`, or a letter beyond ASCII in a name.
A name is ASCII letters, digits and `_`.

```c
static u8 total;

static void entry()
{
  total = $10;
}
```

`$` is the assembler's prefix for hexadecimal. C writes `0x10`. A character
beyond ASCII belongs in a literal that names a Charset, see
[`NGA7016`](#nga7016). A byte that is not valid UTF-8 is reported here too when
it stands outside a comment or a literal; inside one it is
[`NGA7003`](#nga7003).

## NGA7002

The lexer found a byte below `$20` other than tab, line feed and carriage
return, or the byte `$7F`, between tokens or inside a string literal or a
character constant. A form feed or a vertical tab is not whitespace here.
Refusing these bytes means that a file that is not text, such as a binary named
as a `.ngc` or a source saved as UTF-16, stops here and is not read as tokens.
Inside a literal, a raw tab is allowed, as it is in an assembler string.

Delete the character, or save the file as UTF-8. A byte that should be in a
string is written as an escape, such as `\n`, or as a number in a `u8` array.
A comment may hold a control character. The line quoted under the message shows
the byte as its symbol from Unicode's Control Pictures, `␀` for a NUL.

## NGA7003

A comment or a literal holds bytes that are not UTF-8. Source is UTF-8. A file
saved in an 8-bit code page, such as Windows-1250, or text exported from an
Atari editor, has bytes above `$7F` that are not valid UTF-8. A comment reports
this once, a literal at each bad byte.

Convert the file to UTF-8. Outside a comment or a literal, such a byte is
[`NGA7001`](#nga7001).

## NGA7004

A block comment runs to the first `*/`, and there is none before the end of the
file. Usually the `*/` was deleted, or a comment opened to disable some code
was never closed.

```c
static u8 total;

/* Clears the total.
static void clear()
{
  total = 0;
}
```

Close the comment, or write it as `//` lines.

## NGA7005

C joins a line that ends in a backslash to the next one before it looks for
comments. In C, a `//` comment that ends in `\` therefore also comments out the
line below it. The subset joins no lines, and refuses the comment rather than
compile a program that a C compiler reads differently.

```c
// Converted from C:\ART\
static u8 const tiles[] = { 0, 1, 2 };
```

End the comment with a character other than the backslash.

## NGA7006

C reads a constant with a leading zero as octal, so `040` is 32 in C. The
subset has no octal. Reading `040` as 40 would differ from C, so the spelling
is refused. Padding a column of numbers with zeros is the usual cause.

```c
static u8 const widths[] = { 100, 040 };
```

Drop the leading zeros. `0` alone is zero.

## NGA7007

C23's suffixes are recognised and refused: `u`, `l`, `ll` and `wb`, in either
case, and `u` beside one of the others. A constant of the subset has no type of
its own. It takes the type of the operand beside it or of the object it is
assigned to, so a suffix has nothing to say.

```c
static u16 delay;

static void entry()
{
  delay = 1000u;
}
```

Drop the suffix. Where the type matters, a cast gives it: `(u16)1000`. Any
other letter after the digits is [`NGA7009`](#nga7009).

## NGA7008

There is no floating-point type, so a constant with a fraction or an exponent
has nothing to be. Raised for `1.5`, `.5`, `1e5` and the hexadecimal `0x1p3`.

```c
static u8 half;

static void entry()
{
  half = 0.5;
}
```

Write the value scaled to an integer, such as 0.5 as `128` in the low byte of
an 8.8 fixed-point `u16`.

## NGA7009

A constant holds a digit its base does not have, or a letter after the digits
that is no suffix: a `2` in a binary constant, a `G` in a hexadecimal one. A
`_` is one too: the assembler groups digits with `_`, and C groups them with
`'`. The character may stand straight after the prefix, as in `0xG0` or
`0b_1010`.

```c
static u8 const mask = 0b1010_0101;
```

Write `0b1010'0101`. A suffix C would take is [`NGA7007`](#nga7007) instead.

## NGA7010

`0x` or `0b` has nothing after it, or only a suffix such as `u`. A prefix
followed by a character that is no digit of its base, as in `0xG0` or `0b_1010`,
is [`NGA7009`](#nga7009) at that character.

```c
static u8 const mask = 0x;
```

Write the digits after the prefix.

## NGA7011

`'` groups the digits of a constant, as in `0xFF'FF` or `1'000`. It needs a
digit on each side: it cannot follow the prefix, end the constant, or stand
next to another `'`. The assembler has the same rule for `_`,
[`NGA0012`](#nga0012).

```c
static u8 const mask = 0b'1010'0101;
```

Only the first misplaced separator of a constant is reported.

## NGA7012

The lexer holds a constant in a signed 64-bit number, and this one is above
`0x7FFF'FFFF'FFFF'FFFF`. No type of the subset is wider than 16 bits, so no
program needs a constant this large.

```c
static u16 const mask = 0xFFFF'FFFF'FFFF'FFFF;
```

A constant the lexer can hold and the type where it is used cannot is
[`NGA7206`](#nga7206).

## NGA7013

C reserves the names that begin with `__`, or with `_` and a capital letter, for
the compiler. This compiler uses them: the scratch bytes it adds to a Proc are
named `__t0`, `__t1` and on, in the Proc's scope. Refusing them in a program
keeps the two sets of names apart.

```c
static u8 __tmp;
```

Rename it. `_tmp`, with a lower-case letter after one `_`, is allowed, and so
is a `__` anywhere but at the start.

## NGA7014

A string literal or a character constant ends on the line it starts on. Raised
where the closing quote is missing, or where a text was continued onto the next
line.

```c
static u8 const title[] = "SCORE;
```

Close the quote. For a text longer than a line, write a literal on each line.
Adjacent literals join where they carry the same prefix, see
[`NGA7103`](#nga7103).

## NGA7015

A literal has the assembler's six escapes and no others, so there is no `\x`,
no octal and no `\r`. Each escape is one ASCII character that the literal's
Charset translates, and a byte that no character stands for has no escape.

```c
static u8 const title[] = "SCORE\x9B";
```

Name a Charset that maps a character to the byte: `atascii` maps `\n` to
`$9B`. Otherwise write the bytes as an array of constants. See
[Character sets](../11-character-sets.md).

## NGA7016

A literal without a prefix is ASCII, copied to the bytes as it is. A character
beyond ASCII has no byte until a Charset says which one, so it is refused, once
per character.

```c
static u8 const lives[] = "LIVES ♥";
```

Glue the Charset's name to the opening quote, in a Project that includes
`atari/800xl.ngp`. An array given a literal also gets its terminating `\0`,
which `atascii` does not map, so give the array its size to leave it out:
`lives[7] = atascii"LIVES ♥"`. A prefix that no Module exports is
[`NGA7201`](#nga7201). See [Character sets](../11-character-sets.md).

## NGA7017

A character constant is one character, or one escape, between single quotes.
Raised for `''`, and for more than one character, which C accepts with a value
each compiler chooses. A string written in single quotes is the usual cause.

```c
static u8 const title[] = 'SCORE';
```

Write a string in double quotes.

## NGA7100

The parser found a token that the grammar does not allow where it stands. It
reports what it expected and what it found, skips to the `;` that ends the
statement or past the `}` of the block, and carries on. Several things C has
and the subset does not arrive here: a prototype, `unsigned`, `long`, `short`
and `signed`, `goto`, a value on an enumerator, and a statement before the
first label of a `switch`.

```c
static void draw();

static void entry()
{
  draw();
}

static void draw()
{
}
```

Delete the prototype. A function can be called from anywhere in its file,
before its definition as well as after it. See
[What is not there](../17-c-dialect.md#what-is-not-there).

## NGA7101

The parser limits how deep a program nests: operators 64 deep in one
expression, parentheses 64 deep, and blocks together with `if`, `while`, `do`
and `for` 127 deep. A call, a prefix operator and `=` each count as one level
of operators. The compiler walks the tree recursively in every pass, and the
limits keep that within a thread's stack. The parser reports once and skips
what it refused.

Split the expression with locals, or move the inner part of a deep nest into a
function. See [Limits](../17-c-dialect.md#limits). The assembler's limit is
[`NGA0104`](#nga0104).

## NGA7102

The subset spells each type one way, and `int` and `char` are not the spelling
of any. The message names the one to use. Raised wherever a type is written: at
file scope, for a local, a parameter, a member or a cast.

```c
static int score;
```

Write `i16` for `int` and `u8` for `char`. A value that fits a byte is cheaper
as a `u8` or an `i8` on the 6502. `unsigned`, `long`, `short` and `signed` are
parse errors, [`NGA7100`](#nga7100).

## NGA7103

Adjacent string literals join into one, as in C, only where they carry the same
prefix. No prefix counts as a prefix of its own: ASCII, untranslated. C would
give the joined literal the one prefix written; the subset refuses the join.

```ngp
include "atari/800xl.ngp"

modules { "main.ngc" }
```

```c
static u8 const title[] = "SCORE " screen"000";
```

Give every piece the same prefix: `screen"SCORE " screen"000"`.

## NGA7200

The grammar reads more of C than the compiler compiles. A construct that parses
and has no code behind it is refused where it stands, and the message names it.
Among them are a call of anything but a name, an assignment to anything but a
name, an expression statement that neither assigns nor calls, a constant
shifted by a value, and any operator on a pointer but `+`, `-`, `==` and `!=`.

```c
static u8 x;
static u8 y;

static void entry()
{
  x = 1 << y;
}
```

Rewrite it with what compiles, here as `x = 1;` followed by `x <<= y;`. See
[Statements and expressions](../17-c-dialect.md#statements-and-expressions).

## NGA7201

A name needs no declaration before its use, and most names are visible to the
whole program. A `static` name is seen only in its own file, a name of an
assembler Module only where that Module `.export`s it, and a local only in its
block or in the statement that declares it. The Charset before a literal is a
name too.

```c
static u8 shown;

static void entry()
{
  reset();
  shown = lives;
}
```

```c other.ngc
static u8 lives;

void reset()
{
  lives = 3;
}
```

`lives` is `static` in `other.ngc`. Drop the `static`, or give `other.ngc` a
function that returns it. A name that is visible but cannot be called is
[`NGA7202`](#nga7202).

## NGA7202

Only a C function or an assembler `.proc` can be called. Raised for a call of an
object, a type, or a name the assembler exports that is not a Proc, such as a
Label over code in a `.section`. A Proc says what it takes and returns through
its `.declare` lines. A Label says nothing about either.

```c
static void entry()
{
  clear();
}
```

```asm clear.asm
.export clear

.section
clear   lda #0
        sta $0600
        rts
.ends
```

Write the routine as `.proc clear` and `.endp`, and keep the `.export`. See
[Reading the assembler](../17-c-dialect.md#reading-the-assembler).

## NGA7203

A function's name stands for its code, and code is not assigned. Raised for an
assignment to a C function or to an assembler `.proc`. The usual intent is a
handler that changes while the program runs, which is a pointer of a function
type.

```c
static void idle()
{
}

static void update()
{
}

static void entry()
{
  update = idle;
}
```

Declare the handler as a pointer, `typedef void step();` and
`static step* update;`, and assign the function to it. See
[Function types](../17-c-dialect.md#function-types).

## NGA7204

A file defines each name once. Raised for a second object, function, type or
enumerator of a name the file defines already, and for a local or a parameter
declared twice in one scope. C takes two `u8 x;` at file scope as one object,
and the subset refuses them as well. An enumerator of a plain `enum` is a name
of the file, so it meets the file's other names.

```c
static u8 score;
static u16 score;
```

Rename one, or delete the second. While a file has a name defined twice at file
scope its bodies are not checked, so more findings may follow the fix. The same
name exported by two files is [`NGA2205`](#nga2205).

## NGA7205

Follows [`NGA7204`](#nga7204), and [`NGA7271`](#nga7271) for a second `extern`,
and points at the first definition of the name.

## NGA7206

An integer constant takes its type from where it is used: the object it is
assigned to, the parameter it is given to, or the operand beside it. Raised
where that type cannot hold it. An expression of constants is folded exactly,
and only the answer is held to the type, so `256 - 1` stands where a `u8` is
wanted and `200 + 100` does not. A `-` directly before a constant is its sign,
so `-128` is an `i8`.

```c
static u8 timer;

static void entry()
{
  timer = 300;
}
```

Use a wider type, or a cast where the wrap is meant: `(u8)300` is 44. A
constant beside a `u8` operand is held to `u8` even where the result goes into a
`u16`, so widen the operand: `(u16)x + 300`. In a comparison the same constant
is a warning, [`NGA7210`](#nga7210).

## NGA7207

The operands of one operator have one signedness, and so do an object and the
value assigned to it. C converts one to the other by its promotion rules, which
are easy to misread, so the subset asks for a cast that says which is meant.

```c
static u8 x;
static i8 dx;

static void entry()
{
  x += dx;
}
```

Cast one side. `x += (u8)dx;` adds the same byte, so a negative `dx` still
moves `x` down. A wider value assigned to a narrower object is
[`NGA7208`](#nga7208).

## NGA7208

A wider value is not assigned to a narrower object without a cast. C drops the
high byte silently; here the cast says that it is meant. Raised for an
assignment, an argument and a `return`. A narrower value is widened where it is
assigned, a `u8` with zeros and an `i8` with its sign, so that direction needs
nothing.

```c
static u16 score;
static u8 low;

static void entry()
{
  low = score;
}
```

Cast to keep the low byte, `low = (u8)score;`, or write `(u8)(score >> 8)` for
the high one.

## NGA7209

A `bool` holds `true`, `false` or the result of a comparison. An integer is not
assigned to one, a `bool` is not assigned to an integer, and a `bool` is no
operand of arithmetic or of a comparison. C converts any integer to 0 or 1 on
the way into a `bool`, and the subset has no such conversion. `!`, `&&` and
`||` take a `bool`.

```c
static bool paused;

static void entry()
{
  paused = 0;
}
```

Write `false` or `true`. Make a `bool` of a number with a comparison,
`paused = x != 0;`, and read one as a number with a cast, `(u8)paused`. An
integer where a condition is expected is [`NGA7212`](#nga7212).

## NGA7210

A comparison with a constant outside the range of the other operand has one
answer for every value, and the message says which. A `u8` widened by a cast
still holds 0 to 255, and the message names `u8` rather than the type of the
cast.

```c
static u8 table[256];

static void entry()
{
  for (u8 i = 0; i < 256; ++i)
    table[i] = i;
}
```

This loop never ends, because `i` goes from 255 back to 0. Count with a wider
type, or write the loop as `do ... while (i != 0);`, which stops when `i` wraps.
A constant out of range anywhere but a comparison is an error,
[`NGA7206`](#nga7206).

## NGA7211

An operator works in the width of its operands. C promotes a `u8` to `int`
before a shift, and the subset does not, so the count runs from 0 to 7 for an
8-bit value and from 0 to 15 for a 16-bit one. Raised for a constant count
outside that.

```c
static u8 high;
static u8 low;
static u16 address;

static void entry()
{
  address = high << 8 | low;
}
```

Widen the value before the shift: `(u16)high << 8 | low`.

## NGA7212

The condition of `if`, `while`, `do`, `for` and `?:`, and each operand of `!`,
`&&` and `||`, is a `bool`. C takes any integer or pointer there and tests it
against zero. The subset wants the comparison written.

```c
static u8 frames;

static void entry()
{
  while (1)
    frames += 1;
}
```

Write `while (true)` or `for (;;)` for a loop without end, `if (lives != 0)`
for a count, and `p != nullptr` for a pointer.

## NGA7213

`continue` stands in a loop, and `break` in a loop or a `switch`. Raised for one
in neither, usually a `break` meant to leave the function.

```c
static u8 lives;
static u8 shown;

static void entry()
{
  if (lives == 0)
    break;
  shown = lives;
}
```

Write `return` to leave the function. In a block under `[[with(...)]]`, a
`break` or a `continue` that would leave the block is [`NGA7283`](#nga7283).

## NGA7214

A declaration starts with a type, and this name is not one. The types are `u8`,
`i8`, `u16`, `i16` and `bool`, and the names of the program's `enum`,
`enum struct`, `struct` and `union` types and of its function types. A name from
`<stdint.h>` is the usual cause, since there is no preprocessor and no header.

```c
static uint8_t lives;
```

`uint8_t` is `u8`, `int8_t` is `i8`, `uint16_t` is `u16` and `int16_t` is
`i16`. `int` and `char` are [`NGA7102`](#nga7102).

## NGA7216

An enumerator is reached through its type, as in `Light::RED`, and the type has
no enumerator of this name. Usually it is misspelled, or it belongs to another
enumeration.

```c
enum struct Light { RED, AMBER, GREEN };

static Light light;

static void entry()
{
  light = Light::YELLOW;
}
```

Use a name the type declares. An enumerator of another type used as a `case`
label is [`NGA7220`](#nga7220).

## NGA7217

The values of an enumeration are assigned and compared, by the order of their
enumerators, and are never computed. There is no arithmetic on them, no `++`
and no bitwise operator. A plain `enum` is held to this as an `enum struct` is.

```c
enum struct Light { RED, AMBER, GREEN };

static Light light;

static void entry()
{
  light = light + 1;
}
```

Assign the next enumerator by name, in a `switch` over the current one. Where
the program needs the number, `(u8)light` reads it, and a `u8` is the type for
a value that is computed. A value of another type meeting the enumeration is
[`NGA7218`](#nga7218).

## NGA7218

An enumeration meets only its own type: it is assigned a value of that type and
compared with one. An integer, a constant and a value of another enumeration
are different types. The two ways of a `?:` must be of one type as well, and so
must two `struct` objects in an assignment.

```c
enum struct Light { RED, AMBER, GREEN };

static Light light;

static void entry()
{
  light = 0;
}
```

Assign the enumerator, `light = Light::RED;`. No cast turns a number into an
enumerator. Arithmetic on a value of the type is [`NGA7217`](#nga7217).

## NGA7219

Raised for a `switch` over a `bool`, a pointer, a `struct` or a `union`.

```c
static bool paused;
static u8 colour;

static void entry()
{
  switch (paused)
  {
  case true:
    colour = 2;
    break;
  }
}
```

Test a `bool` with `if` and `else`.

## NGA7220

A `switch` over an enumeration takes the type's own enumerators as its labels.
Raised for a number, or an enumerator of another type. A `switch` over a plain
`enum` takes a label written either way, `EASY` or `Level::EASY`.

```c
enum struct Light { RED, AMBER, GREEN };

static Light light;
static u8 stops;

static void entry()
{
  switch (light)
  {
  case 0:
    stops += 1;
    break;
  default:
    break;
  }
}
```

Write the enumerator, `case Light::RED:`. A name the type does not declare is
[`NGA7216`](#nga7216).

## NGA7221

A label stands once in a `switch`, and so does `default`. The labels of a
`switch` over an integer are compared by value, after folding, so `0x41`
repeats `'A'`. Over a plain `enum`, `EASY` and `Level::EASY` are one label.

```c
static u8 key;
static u8 dx;

static void entry()
{
  switch (key)
  {
  case 'A':
    dx = 255;
    break;
  case 0x41:
    dx = 1;
    break;
  }
}
```

Delete one of the two, or join their statements under one label.

## NGA7222

A `switch` over an `enum` or an `enum struct` names every enumerator in a
`case`, or has a `default`. Raised where some enumerators are named by no `case` and nothing
takes the rest, so a value of one of them runs none of the `switch`.

```c
enum struct Light { RED, AMBER, GREEN };

static Light light;
static u8 stops;

static void entry()
{
  switch ( light )
  {
  case Light::RED:
    stops += 1;
    break;
  }
}
```

Add the missing cases, or a `default:` with an empty `break` where doing
nothing is meant. A `switch` over an integer is not checked.

## NGA7223

A value of an enumeration is one byte, and its enumerators are numbered from 0,
so a type names 256 of them at most. Raised at the name of a type that declares
more. Split it into two types.

## NGA7224

The subset has no default arguments and no variadic functions. The count is
the function's own for a function of C, the function type's for a call through
a pointer, and the number of `.declare arg` lines for a `.proc` of the
assembler.

```c
static u8 total;

static u8 add( u8 x, u8 y )
{
  return x + y;
}

static void entry()
{
  total = add( 1 );
}
```

Where the callee is a `.proc`, check that every byte the caller is meant to
write has a `.declare arg` over it. A `.ztemp` without one is the Proc's own
scratch.

## NGA7225

Raised where a call is read as a value, in an assignment, an expression or a
condition, and the callee returns nothing: a `void` function, or a `.proc` with
no `.declare ret`. A call that stands as a statement is not refused.

```c
static u8 level;

static void entry()
{
  level = roll();
}
```

```asm dice.asm
.export roll

.proc roll
result  .ztemp 1
        lda #4
        sta result
        rts
.endp
```

Here the Proc leaves a value in `result`, but nothing says the byte is its
result. Put `.declare ret` on the line above `result`. A result left in `A` is
declared as well, `.declare ret a`; see [`.declare`](../16-assembler.md#declare).

## NGA7226

Raised at a bare `return;` in a function declared with a result.

```c
static u8 lives;

static u8 loseLife()
{
  if ( lives == 0 )
    return;
  lives -= 1;
  return lives;
}
```

Give the `return` the value the caller should see, or declare the function
`void` where no caller reads its result.

## NGA7227

Raised at `return` with a value in a `void` function.

```c
static u8 lives;

static void loseLife()
{
  lives -= 1;
  return lives;
}
```

Declare the function's result type where the value is meant to reach the
caller, and drop the value otherwise.

## NGA7228

Raised at the closing brace of a function with a result, where some path through
the body reaches it without a `return`. A loop that nothing leaves reaches no
end, and an `if` whose two arms both return is complete.

```c
enum struct Light { RED, GREEN };

static u8 delay( Light light )
{
  switch ( light )
  {
  case Light::RED:
    return 50;
  case Light::GREEN:
    return 10;
  }
}
```

A `switch` over an enumeration is not complete even where every enumerator has
a `case` that returns, because a byte that holds no enumerator's value runs none
of them. Make one of the cases `default:`, or end the function with a `return`.

## NGA7229

Raised at a call that closes a cycle of calls: a function that calls itself, or
functions that call one another, in one file or across several. Each cycle is
reported once, at the call written last.

A function's parameters, locals and result are Temporaries, each at one
address, and not a frame on a stack. A second activation of a function would
write over the bytes of the first.

```c
static u8 depth;

static void descend( u8 level )
{
  depth = level;
  if ( level < 4 )
    descend( level + 1 );
}
```

Write the recursion as a loop. Where it needs to remember a path, keep the path
in an array of your own and index it as a stack.

## NGA7230

Raised at a cast the subset does not have. Only a `bool` casts to a `bool`: a
`bool` holds `true`, `false` or a comparison, and a cast is none of them. A
pointer casts to another pointer or to a `u16`, and only a pointer, a `u16` or a
constant casts to a pointer. A function's address casts to nothing: it goes to a
pointer of a function type.

```c
static u8 lives;
static bool alive;

static void entry()
{
  alive = (bool)lives;
}
```

Write the comparison that is meant, `lives != 0`. To get a pointer into a
narrower integer, cast it to `u16` first.

## NGA7231

An object at file scope or a `static` local that is given a value has that
value in the program's bytes when it is loaded. No code runs to compute it, so
the value must be a constant: a literal, a `const` with a constant value, an
enumerator, `sizeof`, an address the assembler knows, or `nullptr`. Also raised
at a `case` label of a `switch` over an integer that is not a constant; an
enumerator there is a constant of another type, which is
[`NGA7218`](#nga7218).

```c
static u8 width = 40;
static u8 middle = width / 2;
```

Here `width` is a variable. Declare it `const`, and `width / 2` is a constant.
Where the value really is computed, assign it at the start of `entry`.

## NGA7232

Raised at a `const` object declared without a value, at file scope or in a
function. Nothing may assign it later, so it would hold whatever the byte held.

```c
static const u8 LIMIT;
```

Give it its value where it is declared. To read a value the assembler or the
Project defines, declare it `extern const u8 LIMIT;` instead; see [Reading the
assembler](../17-c-dialect.md#reading-the-assembler).

## NGA7233

Raised where constants are defined through one another in a circle, so that
none of them has a value to start from. The circle may pass through the size of
an array, and through other files. It is reported once, at the name that closes
it.

```c
static const u8 FIRST = SECOND + 1;
static const u8 SECOND = FIRST;
```

A constant that names itself directly is [`NGA7235`](#nga7235).

## NGA7234

Raised at an assignment, a compound assignment, `++` or `--` whose target is
`const`: a `const` object, an element or a member of one, or a name of the
assembler that C reads as a constant or that an `extern const` declares.

```c
static const u8 LIVES = 3;

static void entry()
{
  LIVES -= 1;
}
```

Drop the `const` where the object is meant to change. A write through a pointer
to `const` is [`NGA7247`](#nga7247).

## NGA7235

Raised where the value a declaration gives reads the name being declared. As in
C, the name is in scope from its own declarator on, and there it holds nothing
yet. An array whose size is `sizeof` of itself is the same mistake.

```c
static u8 count = 0;

static void entry()
{
  u8 count = count + 1;
}
```

The usual cause is a local named like a global: the local hides the global
already in its own value. Rename the local.

## NGA7236

Raised where a constant index is outside an array of known size: below 0, or at
or past its count. `sizeof t[9]` is held to it too.

```c
static u8 scores[4];
static u8 last;

static void entry()
{
  last = scores[4];
}
```

Only a constant index of an array is checked. A pointer is not, an array of the
assembler whose count a string leaves unknown is not, and neither is an index
computed at run time; see [What a program may not count
on](../17-c-dialect.md#what-a-program-may-not-count-on).

## NGA7237

The index of an array is a `u8`, a `u16`, or a constant inside the array. A
signed index is refused, since an array starts at index 0, and so is a `bool` or
a value of an enumeration. A pointer takes any integer, signed included, since
it may point into the middle of something. A `[[striped]]` array takes only a
`u8` or a constant, because its index goes into `X`.

```c
static u8 heights[8];
static i8 column;
static u8 height;

static void entry()
{
  height = heights[column];
}
```

Cast the index to the unsigned type that is meant, `heights[(u8)column]`, or
declare it unsigned.

## NGA7238

Raised at an assignment, or a compound one, to a whole array: an array of C, an
array member of a `struct`, or an array the assembler exports. C has no array
assignment either.

```c
static u8 shown[4];
static u8 saved[4];

static void entry()
{
  saved = shown;
}
```

Copy the elements in a loop. Or keep the array in a `struct`, which is assigned
whole: with `struct Row { u8 cells[4]; };`, two `Row`s copy with `saved =
shown;`.

## NGA7239

Raised at `x[i]` where `x` is neither an array nor a pointer, and at `sizeof
x[0]` of such a name.

```c
static u16 score;
static u8 low;

static void entry()
{
  low = score[0];
}
```

A byte of a `u16` is a cast: `(u8)score` is the low byte and `(u8)(score >> 8)`
the high one. An address held in a `u16` is indexed through a cast to a
pointer, `((u8*)address)[2]`.

## NGA7240

Raised where the divisor of `/` or `%` is a constant that folds to zero. A
divisor computed at run time is not checked.

```c
static const u8 ROWS = 0;
static u8 perRow;

static void entry()
{
  perRow = 200 / ROWS;
}
```

## NGA7241

Raised where a list gives an array more values than its size. A string literal
gives its characters and a `\0`; where the array holds the characters exactly,
the `\0` is dropped, so `u8 name[3] = "ABC";` is accepted and `u8 name[2] =
"ABC";` is not.

```c
static const u8 steps[3] = { 1, 2, 4, 8 };
```

Leave the size out, `steps[]`, and the list gives it.

## NGA7242

Raised where an array is given a single value, or a scalar is given a braced
list.

```c
static u8 counts[4] = 0;
```

An array takes a list, `{ 0, 0, 0, 0 }`, and a scalar a bare value.

## NGA7243

Raised where the size of an array is missing, zero, not a constant, or so large
that the array would pass 64 KB. The largest count is 65536 divided by the size
of an element, and the message gives it. The size may be left out only where a
list or a string literal gives the count.

```c
static u8 buffer[];
```

Write the size, `buffer[40]`. A size from a variable is not in the subset; use
a `const`.

## NGA7244

A declaration of several names declares them all pointers, or none. In C,
`u8* from, to;` makes `from` a pointer and `to` a byte, which reads as two
pointers.

```c
static u8* from, to;
```

Write one declaration per name, or put the `*` on each, `u8 *from, *to;`.

## NGA7245

Raised at `&` of a parameter. A parameter is a byte of the callee that the
caller writes before the `jsr`, and the compiler may keep it in a register or
share its byte with another Temporary, so it has no address to give.

```c
static void clear( u8* at )
{
  *at = 0;
}

static void reset( u8 count )
{
  clear( &count );
}
```

A local whose address is taken is given a byte of its own, so `u8 copy =
count;` and `clear( &copy );` work.

## NGA7246

Raised where a pointer meets a value it does not agree with, in an assignment,
an argument, a `return`, `==`, `!=` or `-`. A pointer takes `nullptr`, or a
pointer to the same type, which may add `const` or `volatile` but not drop
them. An integer is not a pointer, and an array's name is a pointer to its first
element, so `n = table` is refused here too.

```c
static u8* cursor;

static void entry()
{
  cursor = 0;
}
```

Write `nullptr` for no address. An address held in a `u16` goes in through a
cast, `cursor = (u8*)address;`.

## NGA7247

Raised at a write through a pointer to `const`, by `*p`, `p[i]` or `p->m`. The
pointer may change; what it points at may not be written through it.

```c
static const u8* source;

static void entry()
{
  source[0] = 1;
}
```

Declare the pointer without `const` where it is meant to write. It can then no
longer be given the address of a `const` object, which is
[`NGA7246`](#nga7246).

## NGA7248

Raised at `*x` where `x` is not a pointer.

```c
static u16 address;
static u8 value;

static void entry()
{
  value = *address;
}
```

A `u16` that holds an address is not a pointer. Cast it, `*(u8*)address`, or
keep the address in a pointer. `x->m` on what is no pointer is
[`NGA7255`](#nga7255).

## NGA7249

Raised at a size written in a parameter's brackets. In C the parameter is a
pointer whatever the size says, and the function takes an array of any length,
so here the size is refused rather than ignored.

```c
static void clearRow( u8 row[40] )
{
  row[0] = 0;
}
```

Write `u8 row[]` or `u8* row`, and pass the length where the function needs it.

## NGA7250

Raised at a `struct` or a `union` declared with no members.

```c
struct Empty { };
```

Give it the members it is meant to hold, or remove it.

## NGA7251

Raised where a `struct` or a `union` holds itself by value, directly or through
other types. It is reported once per cycle, at the member that closes it.

```c
struct Node { u8 value; Node next; };
```

Hold a pointer instead, `Node* next;`.

## NGA7252

Raised at a member that is `const`, a `const` pointer, or given a value in the
type.

```c
struct Ship { const u8 speed; u8 x; };
```

Put `const` on the object, `static const Ship SCOUT = { 3, 0 };`, and give the
values in its list. A member may still be a pointer to `const`.

## NGA7253

Raised at `s.m` or `p->m` where the type has no member of that name. Through a
pointer, the member is looked up in the type the pointer points at.

```c
struct Point { u8 x; u8 y; };

static Point cursor;

static void entry()
{
  cursor.z = 0;
}
```

## NGA7254

Raised at the second member of one name in a `struct` or a `union`.

```c
struct Point { u8 x; u8 x; };
```

## NGA7255

Raised at `s.m` where `s` is not a `struct` or a `union`, and at `p->m` where
`p` does not point at one. `.` on a pointer lands here too.

```c
struct Point { u8 x; u8 y; };

static Point* cursor;

static void entry()
{
  cursor.x = 0;
}
```

Write `cursor->x` through a pointer, and `s.x` on an object.

## NGA7256

Raised where a braced list does not fit what it fills: more values than a
`struct` has members, more than one for a `union`, whose list gives its first
member alone, an array member given a single value or more values than it
holds, or a list where a scalar stands.

```c
struct Point { u8 x; u8 y; };

static Point start = { 1, 2, 3 };
```

Give one value per member, in order. An array member takes a list of its own,
in braces, as a nested `struct` does: C's elided braces are not read, so a
`struct Row { u8 cells[2]; };` is given `{ { 1, 2 } }`.

## NGA7257

Raised where a `struct` or a `union` is the operand of an operator: a
comparison, arithmetic, a unary operator or a cast. A `struct` is assigned
whole, passed to a function, or read member by member.

```c
struct Point { u8 x; u8 y; };

static Point here;
static Point there;
static bool same;

static void entry()
{
  same = here == there;
}
```

Compare the members, `here.x == there.x && here.y == there.y`.

## NGA7258

The dialect's attributes are the nine listed in
[Attributes](../17-c-dialect.md#attributes), written without a prefix. Raised at
any other, at one with a prefix such as `[[gnu::packed]]` or
`[[nga::striped]]`, at C23's own such as `[[deprecated]]`, and at `striped`,
`trampoline` or `slot` given arguments.

```c
[[maybe_unused]] static u8 spare;
```

## NGA7259

Raised where one of the dialect's attributes stands on something it does not
apply to. The message names what it applies to. Two cases are not obvious from
the table in [Attributes](../17-c-dialect.md#attributes): `[[in(PANE)]]` is refused
on a pointer, which lies in the zero page that no Window covers, and an
`extern` that is a `[[slot]]` takes no other attribute.

```c
[[striped]] static u16 score;
```

## NGA7260

`[[striped]]` splits each element into one array per byte, so it applies to an
array of `u16`, of `i16`, or of a `struct` whose members are scalars, pointers
or `struct`s of the same kind. An array of one-byte elements is a single stripe
already. A `union`, and a `struct` with an array member, are refused.

```c
[[striped]] static u8 heights[8];
```

Drop the attribute from an array of bytes. For a `struct` with an array member,
write the array's elements as members of their own.

## NGA7261

A striped array is indexed through `X`, so it holds 256 elements at most.

```c
[[striped]] static u16 scores[300];
```

Split it into two striped arrays, or drop the attribute.

## NGA7262

Raised at `&` of an element of a `[[striped]]` array, at `&` of a member of more
than one byte in one, and at the array's name used as a value. The bytes of an
element lie in different stripes, so neither the element nor the array has one
address.

```c
struct Point { u8 x; u8 y; };

[[striped]] static Point points[8];
static Point* chosen;

static void entry()
{
  chosen = &points[2];
}
```

A member of one byte has an address, so `&points[2].x` is allowed. Pass the
index where the code would pass a pointer.

## NGA7263

Raised where C reads a name an `.asm` Module exports, and the Label has no
shape C can give a type. A Label's shape is the run of one kind of directive at
it: `.byte` lines, `.word` lines, or reservations. The message gives the reason:
it holds both `.byte` and `.word`, or code, or nothing, or a reservation whose
size the compiler cannot fold, or a `.if` chooses what stands at it. The note
points at the Label.

```c
static u8 first;

static void entry()
{
  first = enemies[0];
}
```

```asm enemies.asm
.export enemies

.section
enemies .byte 1
        .word 300
.ends
```

For data of mixed widths, say how C reads it with an `extern` declaration,
usually of a `struct`: `struct Enemy { u8 kind; u16 speed; };` and `extern
const Enemy enemies[];`. See [Reading the
assembler](../17-c-dialect.md#reading-the-assembler).

## NGA7264

C names something an `.asm` Module exports that C has no reading for: a
Charset used as a value, a Slot or a macro of the assembler, a Pane or a Window
anywhere but in an attribute, or a Constant that is a string or names one of
those. Raised where an expression names it and where an `extern` declares it.

C reads a Label as memory and a Constant as a number, and a string is neither.
Put text under a Label in a `.byte` line, which C reads as `u8[]`. A Charset is
named as a literal's prefix, `letters"AB"`, and a Pane in `[[in(...)]]` and
`[[with(...)]]`.

```c
static u8 x;

static void entry()
{
  x = GREETING[0];
}
```

```asm text.asm
.export GREETING

GREETING = "HELLO"
```

## NGA7265

C names a Region the Target declares `reserved`, in an expression or in an
`extern`. A named `ram` Region or a `register` is memory at its address in C;
a `reserved` one belongs to something else, usually the machine's OS or a DOS,
and C does not reach it.

Write what touches those bytes in an `.asm` Module, and call that Proc from C.

```ngp
target {
  region $0080 .. $00FF ram
  region dos $0700 .. $1FFF reserved
  region $2000 .. $BFFF ram
}

modules { "main.ngc" }
```

```c
static u8 x;

static void entry()
{
  x = dos[0];
}
```

## NGA7267

Follows [`NGA7263`](#nga7263), [`NGA7264`](#nga7264) or
[`NGA7265`](#nga7265), and points at the definition of the name C was refused:
the Label, Constant or Charset in its `.asm` Module, or the Region in the
Project file.

## NGA7268

C calls a `.proc` of the assembler, with no `extern` for it, and one of the
Proc's `.declare arg` or `.declare ret` lines is `u8[N]`: written so, or over a
reservation of three bytes or more with no type written. Such bytes are not a
value, and a call from C hands over values only.

Declare the Proc with an `extern` whose parameter is a `struct` of those N
bytes, `extern void draw(Spot spot);` for the example below, and the call
copies the `struct` into the Proc's bytes. Or split the bytes into
`.declare arg` lines of `u8` and `u16`.

```c
static void entry()
{
  draw( 0 );
}
```

```asm draw.asm
.export draw

.proc draw
        .declare arg
spot    .ztemp 3
        rts
.endp
```

## NGA7269

An `extern` names a variable or a function that a `.ngc` file defines. In this
dialect `extern` says how C reads a name the assembler exports. A name defined
in C is visible to every file without a declaration, so delete the `extern`.

```c
extern u8 lives;

static void entry()
{
  lives -= 1;
}
```

```c other.ngc
u8 lives = 3;
```

## NGA7270

An `extern` names something no `.asm` Module exports. The Module may define
the Label without an `.export`, the Module may be missing from the Project's
`modules` block, or the two spellings may differ.

```c
extern u16 score;

static void entry()
{
  score += 10;
}
```

```asm score.asm
.section
score   .res 2
.ends
```

Add `.export score` to the Module.

## NGA7271

A second `extern` of one name, in another `.ngc` file. One `extern` is visible
to every file of the program, so the second could only repeat it or contradict
it. Keep one, in any file. Two in one file are
[`NGA7204`](#nga7204) instead.

```c
extern u16 score;

static void entry()
{
  score += 10;
}
```

```c other.ngc
extern u16 score;
```

```asm score.asm
.export score

.section
score   .res 2
.ends
```

## NGA7272

An `extern` the dialect refuses as it is written. The message gives the
reason. An `extern` is given no value and carries no attribute, because the
bytes and where they stand are the assembler's. A `.proc` is declared as a
function and not as an object. A Constant is declared `const`, as a scalar, an
`enum struct` or a pointer.

Here a Constant of the Project is declared without `const`:

```ngp
constants {
  LEVELS = 8
}

modules { "main.ngc" }
```

```c
extern u8 LEVELS;
static u8 level;

static void entry()
{
  level = LEVELS;
}
```

A Constant is a number and not memory, so C cannot write it. Write
`extern const u8 LEVELS;`.

## NGA7273

An `extern` whose type does not fit the bytes it reads. The bytes start at the
Label and run past other Labels to the end of the Label's Section, and no
further. The message says what does not fit: the type needs more bytes than
the assembler wrote before the Section ends; `NAME[]` reads elements that do
not divide those bytes evenly; or a named Region is longer or shorter than the
type.

```c
extern const u8 steps[4];
static u8 step;

static void entry()
{
  step = steps[1];
}
```

```asm steps.asm
.export steps

.section
steps   .byte 1, 2, 4
.ends
```

Give the array the size of the data, or write `steps[]` and let the bytes give
the count. Where something other than data ends the bytes first, the finding is
[`NGA7274`](#nga7274).

## NGA7274

The bytes an `extern` reads run into something that is not data before the
type is covered: code, lines a `.if` chooses between, or a `.res` whose size
the compiler cannot fold. The message names which. An `extern` is checked item
by item, and none of these has items to check.

```c
extern const u8 table[4];
static u8 value;

static void entry()
{
  value = table[3];
}
```

```asm table.asm
.export table

.section
table   .byte 1, 2
        rts
.ends
```

Make the type as long as the data in front of the code, or put the data in a
Section of its own.

## NGA7275

An `extern`'s type puts the start or the end of a value of C inside an item the
assembler wrote. The usual cases are a `.word` read as two `u8`s, and a
`struct` whose `u16` member starts on the second byte of a `.word`. The message
names the element or member and its offset.

A `.word` holds one value, so C may not read it as two bytes. Two `.byte`
items read as one `u16` are accepted, and a `.res` may be read as any type.

```c
extern const u8 speeds[4];
static u8 low;

static void entry()
{
  low = speeds[0];
}
```

```asm speeds.asm
.export speeds

.section
speeds  .word 300, 500
.ends
```

Declare `speeds` as `u16`, or write the data as `.byte` if it is bytes.

## NGA7276

An `extern` of a `bool` or an `enum struct` over a byte whose value is outside
the type: not 0 or 1 for a `bool`, not below the number of enumerators for an
`enum struct`. A Constant declared as one of them is checked the same way. Only
items whose value the compiler can fold are checked, so an address is not.

```c
enum struct Mode { OFF, ON, BLINK };

extern const Mode modes[];
static Mode mode;

static void entry()
{
  mode = modes[2];
}
```

```asm modes.asm
.export modes

.section
modes   .byte 0, 1, 3
.ends
```

Correct the table, or add the enumerator it was written for.

## NGA7277

`extern RESULT NAME(PARAMETERS);` does not match the `.proc` it declares. The
message says where: the name is no `.proc`; there are more or fewer parameters
than `.declare arg` lines; a result is declared where the Proc has no
`.declare ret`, or the reverse; a parameter's type does not fit its bytes; or a
parameter's name is not the name of the byte its `.declare` stands over.

```c
extern void plot(u8 x, u8 y);

static void entry()
{
  plot( 3, 4 );
}
```

```asm plot.asm
.export plot

.proc plot
        .declare arg
column  .ztemp 1
        .declare arg
row     .ztemp 1
        rts
.endp
```

A parameter's name, where one is written, is held to the Proc's byte. Name them
`column` and `row`, or leave the names out: `extern void plot(u8, u8);`. See
[Reading the assembler](../17-c-dialect.md#reading-the-assembler).

## NGA7278

A string or character literal's prefix names something an `.asm` Module
exports, and it is not a Charset. Here a Label has the name the program meant
for a Charset. A prefix that names nothing at all is
[`NGA7201`](#nga7201).

```c
static const u8 title[] = screen"READY";
static u8 first;

static void entry()
{
  first = title[0];
}
```

```asm screen.asm
.export screen

.section
screen  .res 40
.ends
```

Rename the Label, or write the Charset's own name in front of the quote. See
[Character sets](../11-character-sets.md).

## NGA7279

`[[in(...)]]`, `[[under(...)]]` or `[[with(...)]]` names something that is not
what the form takes. `in` takes a Pane or a family. `under` takes a Pane, not a
family, since which member is shown is known only at run time. `with(NAME)`
takes a Pane, `with(NAME, i)` a family or a Window, and `with(NAME = STATE)` a
Window. The message says what the name is instead, or that the program does
not know it.

```ngp
include "atari/130xe.ngp"

modules { "main.ngc" }

panes in ext { rows[3] }
```

```c
[[in(rows)]] static u8 row[40];

static void entry()
{
  [[with(rows)]]
  {
    row[0] = 0;
  }
}
```

`rows` is a family, so the block names a member: `[[with(rows, i)]]`. See
[Panes](../17-c-dialect.md#panes).

## NGA7280

A direct call of a function `[[in(FAMILY)]]` from code that is not in a block
showing a member of the family. The compiler wraps a call into another Pane in
a `.with` of that Pane, and a family has several members; the call does not
say which one.

```ngp
include "atari/130xe.ngp"

modules { "main.ngc" }

panes in ext { rows[3] }
```

```c
[[in(rows)]] static u8 row[40];

[[in(rows)]] static void clear()
{
  row[0] = 0;
}

static void entry()
{
  clear();
}
```

Make the call inside `[[with(rows, i)]]`, where `i` names the member.

## NGA7281

Inside a `[[with]]` block, a call into a function in another Pane of the
Window the block shows. The compiler would wrap the call in a `.with` of that
other Pane, and after the call the Window would still show it. The rest of the
block would read `map` with `other` shown.

```ngp
include "atari/130xe.ngp"

modules { "main.ngc" }

panes in ext { level, other }
```

```c
[[in(level)]] static u8 map[40];
[[in(other)]] static u8 tiles[8];

[[in(other)]] static void loadTiles()
{
  tiles[0] = 1;
}

static void entry()
{
  [[with(level)]]
  {
    map[0] = 1;
    loadTiles();
  }
}
```

Make the call before or after the block. A call into a Pane of another Window
is wrapped as usual.

## NGA7282

A `[[with]]` block stands inside another one on the same Window. A Window
shows one state at a time, so the inner block would take away what the outer
one shows for the rest of it.

```ngp
include "atari/130xe.ngp"

modules { "main.ngc" }

panes in ext { level, other }
```

```c
[[in(level)]] static u8 map[40];
[[in(other)]] static u8 tiles[8];

static void entry()
{
  [[with(level)]]
  {
    [[with(other)]]
    {
      map[0] = tiles[0];
    }
  }
}
```

Write the two blocks one after the other, and carry the value between them in
a local. A block on another Window may stand inside.

## NGA7283

`return` inside a `[[with]]` block, or a `break` or `continue` of a loop outside
the block. A `[[transition(...)]] return` there is refused the same way.

The block is compiled into a macro used once under `.with`, and a statement
under `.with` runs to its end, as the assembler holds with
[`NGA3007`](#nga3007). Where the code is in a Pane, the end of the `.with` is
what shows that Pane again.

```ngp
include "atari/130xe.ngp"

modules { "main.ngc" }

panes in ext { level }
```

```c
[[in(level)]] static u8 map[40];

static void entry()
{
  [[with(level)]]
  {
    if ( map[0] == 0 )
    {
      return;
    }
    map[0] = 1;
  }
}
```

Test inside the block, keep the answer in a local, and leave after the block.
A loop inside the block may `break` and `continue` as usual.

## NGA7284

Inside a `[[with]]` block, the address of data the block shows, such as `map`
or `&map[i]`, is stored anywhere but in a local declared in the block, or is
returned. The address means that data only while the block shows its Pane.
After the block the same address may show another Bank.

```ngp
include "atari/130xe.ngp"

modules { "main.ngc" }

panes in ext { level }
```

```c
[[in(level)]] static u8 map[40];
static u8* cursor;

static void entry()
{
  [[with(level)]]
  {
    cursor = &map[1];
  }
}
```

Keep the pointer in a local of the block, or hand it to a call made inside the
block. To remember a position past the block, keep the index rather than the
address.

## NGA7286

The index of `[[with(FAMILY, i)]]` or `[[with(WINDOW, i)]]` is not a `u8`
variable or a constant from 0 to 255. The message says what it is. The index is
loaded into `X` before the block, as `.with NAME, x` takes it in the
assembler.

```ngp
include "atari/130xe.ngp"

modules { "main.ngc" }

panes in ext { rows[3] }
```

```c
[[in(rows)]] static u8 row[40];
static u16 which;

static void entry()
{
  [[with(rows, which)]]
  {
    row[0] = 0;
  }
}
```

Compute the index into a `u8` local before the block, `u8 member = (u8)which;`,
and name that local.

## NGA7287

An attribute of the dialect is given arguments it does not take. The message
says what it takes. `[[placement(...)]]` takes `zeropage` or `absolute`;
`[[in(...)]]` and `[[under(...)]]` take one name; `[[transition(...)]]` takes
one Phase; `[[with(...)]]` takes one of its four forms; and `trampoline` stands
only beside a `with`.

```c
[[placement(zp)]] static u8 counter;

static void entry()
{
  counter = 0;
}
```

An attribute given arguments where it takes none is not one of the dialect's,
[`NGA7258`](#nga7258). See [Attributes](../17-c-dialect.md#attributes).

## NGA7288

A function carries both `[[in(PANE)]]` and `[[under(PANE)]]`. `in` puts the
function's code in the Pane. `under` keeps the code outside the Window and has
it run while the Pane is shown, which is what lets it show another state of
the Window. A function is one or the other.

```ngp
include "atari/130xe.ngp"

modules { "main.ngc" }

panes in ext { level }
```

```c
[[in(level)]] static u8 map[40];

[[in(level), under(level)]] static void draw()
{
  map[0] = 1;
}

static void entry()
{
  [[with(level)]]
  {
    draw();
  }
}
```

Keep `in` where the code may live in the Bank beside its data. Keep `under`
where it must stay outside the Window.

## NGA7289

Code `[[in(PANE)]]` holds a `[[with]]` block that shows another state of the
Pane's own Window. Switching the Window would take away the code doing the
switching. The block has to be a Trampoline: a Proc outside the Window that
the code calls with `jsr`, which shows the other state, runs the block and
shows the code's Pane again. That costs a Proc and a `jsr`, so the compiler
asks for it to be written.

```ngp
include "atari/130xe.ngp"

modules { "main.ngc" }

panes in ext { level, other }
```

```c
[[in(level)]] static u8 map[40];
[[in(other)]] static u8 tiles[8];

[[in(level)]] static void draw()
{
  u8 tile = 0;
  [[with(other)]]
  {
    tile = tiles[0];
  }
  map[0] = tile;
}

static void entry()
{
  draw();
}
```

Write `[[with(other), trampoline]]`. A function `[[under(level)]]` stands
outside the Window and needs no Trampoline for the same block. See
[Panes](../17-c-dialect.md#panes).

## NGA7290

`trampoline` on a `[[with]]` block that does not need one. The message says
why: the code stands outside every Window, it is in a Pane of another Window
than the block's, or it runs `[[under]]` a Pane and shows that Pane again
itself. There the block is a macro under `.with` that costs no `jsr`, and
`trampoline` would state a cost the code does not pay.

```ngp
include "atari/130xe.ngp"

modules { "main.ngc" }

panes in ext { level }
```

```c
[[in(level)]] static u8 map[40];

static void entry()
{
  [[with(level), trampoline]]
  {
    map[0] = 1;
  }
}
```

Remove `trampoline`. The opposite case, a block that needs one and does not
say so, is [`NGA7289`](#nga7289).

## NGA7291

A `[[with]]` block shows the Pane the code is in already. Code in a Pane runs
only while that Pane is shown, so the block switches nothing. Code in a family
is refused any block on the family, since it already runs in the member that
is shown.

```ngp
include "atari/130xe.ngp"

modules { "main.ngc" }

panes in ext { level }
```

```c
[[in(level)]] static u8 map[40];

[[in(level)]] static void draw()
{
  [[with(level)]]
  {
    map[0] = 1;
  }
}

static void entry()
{
  draw();
}
```

Remove the block and keep its statements.

## NGA7292

Code `[[in(P)]]` calls a function in another Pane of the same Window. The call
would have to switch the Window, and the switch would take the calling code
away before the call returned to it.

```ngp
include "atari/130xe.ngp"

modules { "main.ngc" }

panes in ext { level, other }
```

```c
[[in(level)]] static u8 map[40];
[[in(other)]] static u8 tiles[8];

[[in(other)]] static void loadTiles()
{
  tiles[0] = 1;
}

[[in(level)]] static void draw()
{
  loadTiles();
  map[0] = 1;
}

static void entry()
{
  draw();
}
```

The message names the two answers. Make the call in a
`[[with(other), trampoline]]` block, or write the calling code as an
`[[under(level)]]` function, which stands outside the Window and may switch
it. See [Panes](../06-panes.md).

## NGA7293

A call of a function `[[under(P)]]` from code that does not run with `P`
shown: not code in `P`, not another function under `P`, and not inside a
block that shows `P`. The function names what `P` holds and does not show `P`
itself, so whoever calls it shows it.

```ngp
include "atari/130xe.ngp"

modules { "main.ngc" }

panes in ext { level }
```

```c
[[in(level)]] static u8 map[40];

[[under(level)]] static void draw()
{
  map[0] = 1;
}

static void entry()
{
  draw();
}
```

Make the call inside `[[with(level)]]`.

## NGA7294

`[[transition(PHASE)]] return` in a function that returns a value, or on a
`return` that carries one. A Transition enters the next Phase and never comes
back, so there is no caller to take a value.

```ngp
modules { "main.ngc" "game.ngc" }

phase title { needs main  then game  entry titleStart }
phase game  { needs game              entry gameStart }
entry title
```

```c
static u8 lives;

u8 titleStart()
{
  lives = 3;
  [[transition(game)]] return lives;
}
```

```c game.ngc
void gameStart()
{
  for ( ;; )
  {
  }
}
```

Make the function `void` and the `return` bare. The warning after the error
follows from it: with the Transition refused, nothing takes the edge to `game`.

## NGA7295

`[[slot]] extern` on an object whose type gives no Binding. A Slot's Binding
comes from its type: a function binds a `vector`, and a pointer that is
`const` binds a `pointer`. Anything else is refused, and the message says
what the type is. The pointer must be `const` because the Transition routine
writes the Cell, not the program.

```c
[[slot]] extern u8* title;

static u8 first;

static void entry()
{
  first = title[0];
}
```

Write `[[slot]] extern u8* const title;`. See [Slots](../07-slots.md#in-c).

## NGA7296

`[[implements(SLOT)]]` stands on a definition that is not what the Slot's Cell
is read as: an object where the Slot is a function, a function where it is a
pointer, a function of another signature, or data of another type than the
pointer's.

```c
[[slot]] extern void draw();

static u8 drawn;

[[implements(draw)]] static void drawRow(u8 row)
{
  drawn = row;
}

static void entry()
{
  draw();
}
```

Make the Implementation match the Slot. A Slot to a function may take and
return, so where the parameter is meant, give it to the Slot too:
`[[slot]] extern void draw(u8 row);`.

## NGA7297

The address of a function goes to a pointer of a function type, and the
function's signature is not the type's. The message says where they differ.
Raised also where the address goes to anything but a pointer of a function
type, and for a `.proc` of the assembler that does not say
`.proc NAME, as TYPE`.

```c
typedef u8 chooser(u8 x);

static chooser* pick;

static u8 wide(u16 x)
{
  return (u8)x;
}

static void entry()
{
  pick = &wide;
}
```

Give the function the type's signature. See
[Function types](../17-c-dialect.md#function-types).

## NGA7298

One function's address is taken as two function types. A member of a type
reads its arguments from the type's Temporaries and has none of its own, so it
belongs to one type.

```c
typedef u8 chooser(u8 x);
typedef u8 scorer(u8 x);

static chooser* pick;
static scorer* score;

static u8 twice(u8 x)
{
  return x + x;
}

static void entry()
{
  pick = &twice;
  score = &twice;
}
```

Use one type for both pointers, or write a second function for the second
type that calls the first by name.

## NGA7299

An object, a member or a local is declared of a function type, without `*`. A
function type has no bytes, so nothing is of it; it is a type only as a
pointer.

```c
typedef u8 chooser(u8 x);

static chooser pick;

static void entry()
{
}
```

Write `static chooser* pick;`.

## NGA7300

A member of a function type calls its own type: through a pointer of the type,
or by naming another member. The call would write the type's Temporaries, from
which the member is still reading its own arguments.

```c
typedef u8 chooser(u8 x);

static chooser* pick;

static u8 again(u8 x)
{
  return pick( x - 1 );
}

static void entry()
{
  pick = &again;
  pick( 3 );
}
```

A member that dispatches makes the second call through another function type,
or calls a function that is no member of its own type by name.

## NGA7301

A local written `auto` is given a value with no type of its own: an integer
constant, `nullptr`, or the address of a function. Such a value takes its type
from where it is used, so it has none for `auto` to take. A later use of the
local is then reported as not declared.

```c
static void entry()
{
  auto step = 5;
}
```

Write the type, `u8 step = 5;`, or cast the value, `auto step = (u8)5;`.

## NGA7302

A function written `inline` cannot be wrapped into its caller, for the reason
the message gives. The reasons are in the shape of the function: it holds a
`static` local or a local array, or a `[[with]]` block; it is in a Pane or runs under one; it is a member of a
function type or fills a Slot. A call made under a `.with` or inside a
`[[with]]` block is refused at the call. Without `inline` the compiler leaves
such a call as it is and says nothing.

```c
static u8 out;

inline static u8 counted()
{
  static u8 seen = 0;
  seen += 1;
  return seen;
}

static void entry()
{
  out = counted();
}
```

Remove `inline`, or move what stops the wrap out of the function. Here `seen`
can become an object at file scope.

## NGA7303

`inline` is written on something that has no body: a `.proc` of the assembler
declared by `extern`, a function type, an object, a `struct` or an `enum`. The
compiler wraps only bodies written in C.

```c
inline extern void clear();

static void entry()
{
  clear();
}
```

```asm clear.asm
.export clear

.proc clear
        rts
.endp
```

Remove `inline`.

## NGA7304

An expression uses `++`, `--` or a compound assignment as a value, and reads or
writes the same variable again elsewhere in it. C does not fix the order of
the two, and the subset does not choose one. The variable is the whole object,
so stepping one element of an array and reading another in the same
expression is refused too.

```c
static u8 queue[] = { 0, 0, 0, 0 };
static u8 head;

static void entry()
{
  queue[head] = head++;
}
```

Split it into two statements: `queue[head] = head;` and then `head += 1;`.

## NGA7305

`[[placement(absolute)]]` on the declaration of a pointer. The compiler reads
through a pointer with `(zp),y`, which has no absolute form, so a pointer lies
in the zero page whatever is asked.

```c
static u8 screen[] = { 1, 2, 3 };

static void entry()
{
  [[placement(absolute)]] u8* at = &screen[0];
  *at = 0;
}
```

Remove the attribute from the pointer. Over a function or a block, `absolute`
leaves a pointer declared inside in the zero page and says nothing.

## NGA7306

`[[placement(...)]]` and `[[striped]]` on one array. A striped array is a
Section per stripe, laid out so that an element is reached with `abs,x`, and
that layout already settles where it stands.

```c
struct Point { u8 x; u8 y; };

[[placement(zeropage), striped]] static Point points[4];

static void entry()
{
  points[0].x = 1;
}
```

Remove `[[placement]]`. See [`[[striped]]`](../17-c-dialect.md#striped).

## NGA7307

`[[placement(...)]]` asks for the class the declaration has anyway: `absolute`
on an object at file scope, the class of the function or the block around a
local, or `zeropage` on a pointer anywhere, since a pointer lies in the zero page
whatever is asked. A `static` local, a `volatile` one, one whose address is
taken and any local array are `absolute` with no `[[placement]]` over them, as an
object at file scope is, and take the class of a `[[placement]]` on a block or on
the function around them. A `struct` or a `union` of more than four bytes, and a
local array or `struct` that is `const` and given constants, are `absolute`
whatever is asked, so `absolute` on one of them is this warning too.

```c
[[placement(absolute)]] static u8 lives;

static void entry()
{
  lives = 3;
}
```

Remove the attribute.

## NGA7308

A `static` local is read on some path from the function's entry before any
statement writes it. This dialect does not zero static storage, so the first
call reads whatever was in that byte. Arrays, objects whose address is taken,
and objects given a value are not checked.

```c
static u8 frames;

static void tick()
{
  static u8 count;
  count += 1;
  frames = count;
}

static void entry()
{
  tick();
}
```

Give it a value where it is declared: `static u8 count = 0;`. See
[What `static`, `inline` and `volatile` mean here](../17-c-dialect.md#what-static-inline-and-volatile-mean-here).

## NGA7309

A definition in C is named with `::`: a variable, a function, a parameter, a
member or an enumerator. In C, `gfx::count` reads `gfx.count`, a name the
assembler exports from `.namespace gfx`, and C defines no name of its own in a
Namespace.

```c
static void gfx::clear()
{
}

static void entry()
{
}
```

Give the definition a plain name, or define it in the assembler inside the
`.namespace` and read it from C with `::`.

## NGA7310

A plain assignment stands where a value is read: on the right of another
assignment, in a condition, as an argument or as an operand. In this dialect
`=` makes a statement and gives no value, so `a = b = 0` and
`if ((c = next()) != 0)` are errors. A compound assignment such as `+=`, and
`++` and `--`, are values.

```c
static u8 x;
static u8 y;

static void entry()
{
  x = y = 0;
}
```

Write two assignments, `y = 0;` and then `x = y;`. See
[Statements and expressions](../17-c-dialect.md#statements-and-expressions).

## NGA7311

`[[placement(zeropage)]]` stands on a local array or `struct` that is `const` and
given constants. The compiler keeps such a local as read-only data, filled once
and kept between calls as a `static` one is, and that data lies off the zero
page whatever is asked: it may go to ROM, and the zero page is RAM. The same
local given a value that is not a constant, such as `{ seed }`, is written each
time its declaration runs and takes `[[placement]]` as any local array does.

```c
static u8 seed;
static u8 sink;

static void entry()
{
  [[placement(zeropage)]] const u8 steps[] = { 1, 2, 4, 8 };
  sink = steps[seed];
}
```

Remove the attribute, or drop `const` if the bytes must be in the zero page. See
[`[[placement(...)]]`](../17-c-dialect.md#placement).

## NGA7312

`[[placement(...)]]` stands on a local scalar that is `const` and given a
constant, `static` or not. The compiler makes such a local a constant of the
assembler, which takes no byte at all, so there is nothing for the attribute to
place: the value goes into each instruction that reads it, which costs less
than a byte on the zero page would. This is a warning and not an error, since
the program is no worse for it.

```c
static u8 sink;

static void entry()
{
  [[placement(zeropage)]] const u8 step = 4;
  sink = sink + step;
}
```

Remove the attribute. Drop `const` if the value must be a byte the program can
take the address of or change.
