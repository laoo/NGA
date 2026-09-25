# Conditionals and packs

Two questions look alike and are not. **Which statements** are in the program
is one; **which value** a number has is the other. The tool answers them with
different constructs and refuses to let one do the other's work.

## Which statements

Advancing a pointer by one is `inc`. By two it is `inc` twice. By seven it is
an addition, and writing that out for one would be five bytes wasted.

<!-- include 10-conditionals/by-its-argument/skip.asm -->
```asm
; Advance a zero-page pointer by a constant. There is a shortest way for each
; of them, and which one is written out is decided where the macro is used.
.macro skip p, n
.if n == 1
        inc p
.elsif n == 2
        inc p
        inc p
.else
        clc
        lda p
        adc #n
        sta p
.endif
.endm

.proc entry
at      .ztemp 1

        skip at, 1
        skip at, 2
        skip at, 7
        rts
.endp
```
<!-- end -->

<!-- map 10-conditionals/by-its-argument -->
```
NGA memory map

phase phase0 (0)
  zero page: 1 of 128 bytes
  memory:    14 of 40960 bytes
  $0080-$0080  skip.entry.at temporary          phase0
  $2000-$200D  skip.entry    proc               phase0
```
<!-- end -->

Fourteen bytes: two for the first use, four for the second, seven for the
third, and the `rts`. The branches not taken are not in the program — not as a
jump, not as a byte — because a macro's conditionals are decided before any of
its body is instantiated, and each use is decided again.

The condition is a **declared value** and must be. It decides how many bytes
there are, so a condition naming a position or a size would make a size depend
on itself, which is the cycle this whole model is arranged to avoid. A label
inside a branch has to be local, `@name`: two branches want one name for the
same thing, and a Symbol may be defined once.

## However many there are

The last parameter of a macro may be a **pack**, with the dots touching the
name. It takes every argument the names before it do not, and it has two
readings, told apart by where the name stands.

<!-- include 10-conditionals/however-many/pack.asm tag=both -->
```asm
; Both readings of a pack in one statement: the name alone is how many
; elements it has, and the name with the dots spreads them where it stands.
.macro table values...
        .byte values, values...
.endm
```
<!-- end -->

The name alone is an `Integer`: how many elements there are. The name with the
dots **spreads** them — they stand in its place, among whatever else the list
holds. So that one statement writes a count and then the elements, and
`table 3, 1, 4, 1, 5` comes out as six bytes beginning with `5`.

The other way to walk a pack is to take it apart.

<!-- include 10-conditionals/however-many/pack.asm tag=walk -->
```asm
; The other way to walk one: a case per shape, and a recursion that takes an
; element off each turn. The empty case is where it stops.
.macro poke at, values...
.match values
.case first, rest...
        lda #first
        sta at
        poke at + 1, rest...
.case
.endmatch
.endm
```
<!-- end -->

`.match` chooses a `.case` by how many elements the pack has, first fit wins,
and the case's names stand for the elements — one each, with its own pack
taking the rest. The recursion spreads what is left, so each turn round is one
shorter, and the `.case` with no names fits the empty pack, which is where it
stops.

The pattern is not the only thing that can stop it. A count can, and then the
two constructs of this chapter are doing a job each:

<!-- include 10-conditionals/however-many/pack.asm tag=counted -->
```asm
; The third shape, and the one the library uses: a count that comes down by one
; each turn, with `.if` on it deciding whether there is another. It writes as
; many of the list as the number says and drops the rest, so a list may be
; longer than the machine it is written for.
.macro take n, values...
.if n > 0
.match values
.case head, rest...
        .byte head
        take n - 1, rest...
.endmatch
.endif
.endm
```
<!-- end -->

`.if n > 0` decides whether there is another turn and `.match` takes one
element off, so the recursion runs `n` times however long the list is. That
buys something the other shapes cannot: the list may be longer than the number,
and the rest is dropped. `take 3, 3, 1, 4, 1, 5` writes three bytes.
`atari/portb.asm` writes its table of register values exactly this way, so that
a machine with fewer Banks takes a prefix of the list and one with more adds to
it, with nothing in the driver needing to know which machine it is.

<!-- include 10-conditionals/however-many/pack.asm tag=uses -->
```asm
.proc entry
        poke buffer, 9, 2, 6
        lda counts
        lda shorter
        rts
.endp
```
<!-- end -->

<!-- map 10-conditionals/however-many -->
```
NGA memory map

phase phase0 (0)
  zero page: 0 of 128 bytes
  memory:    34 of 40960 bytes
  $2000-$2005  pack.counts  section            phase0
  $2006-$2008  pack.buffer  section            phase0
  $2009-$200B  pack.shorter section            phase0
  $200C-$2021  pack.entry   proc               phase0
```
<!-- end -->

Three stores to three consecutive addresses, a six-byte table that begins with
its own length, and a three-byte one out of a list of five. Not one of those
numbers is written anywhere.

## Which value

A second is fifty frames on one machine and sixty on the other. That is one
number with two values, and it is written as one.

<!-- include 10-conditionals/a-value/wait.asm tag=value -->
```asm
; A second is fifty frames on one machine and sixty on the other. That is one
; number with two values, not two programs, so it is written as a value.
frames = NTSC ? 60 : 50
```
<!-- end -->

`NTSC` comes from the Project, where a `constants` block gives a name a literal
and every Module sees it.

<!-- include 10-conditionals/a-value/main.ngp tag=constants -->
```ngp
constants {
  NTSC = 1
}
```
<!-- end -->

`?:` chooses between two values, and only the condition and the answer taken
are evaluated — which is not an optimisation but the meaning, and is what lets
`narrow ? 4 : buffer.runtimeSectionSize` be a declared value when `narrow`
holds. It is a conditional **value** and not conditional assembly: both answers
are References, so both keep what they name from being pruned.

## Not the other way

Every other assembler writes the frame count the first way anybody would think
of, and here it is refused twice over.

<!-- include 10-conditionals/conditional-name/wait.asm tag=name -->
```asm
; The way every other assembler writes it, and the one thing a branch may not
; hold: whether `frames` exists at all would depend on the condition.
.if NTSC
frames = 60
.endif
```
<!-- end -->

<!-- diagnostics 10-conditionals/conditional-name -->
```
error[NGA0138]: `.if` stands in no section; what has an address is written in a `.section` or a `.proc`
 --> wait.asm:4:1
  |
4 | .if NTSC
  | ^^^

error[NGA0169]: a constant definition cannot stand in a conditional branch, which holds instructions and data
 --> wait.asm:5:1
  |
5 | frames = 60
  | ^^^^^^
```
<!-- end -->

`.if` chooses statements, so it stands where a statement stands — in a
`.section`, a `.proc` or a macro body, and not at the top of a file where
nothing emits. And a name is not a statement. **No Symbol is ever conditional**
here: if one could be, whether a name existed at all would depend on a
condition, and every Step after this one would have to ask before it could look
anything up. What a branch holds is what a macro body holds, and for the same
reason.
