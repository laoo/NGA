# The Project and Modules

## The project

NGA is designed for scalability, so there are no
loose source files here: every file belongs to a project, and the project is
what you hand to the tool. The only thing
the minimal project must say is which files the program is made of:

<!-- include 01-the-project-and-modules/sum/main.ngp -->
```
modules { "sum.asm" }
```
<!-- end -->

`modules` lists them, one module per file, named by the file's stem.

The module defined by a source file is the smallest unit of translation.
There is no concept of file inclusion and each module is assembled/compiled
independently in parallel.

The program starts at a label called `entry`.

## The module

<!-- include 01-the-project-and-modules/sum/sum.asm -->
```asm
.section
total
        .res 1
.ends

.proc entry
        lda #0
        ldx #4
@add    clc
        adc numbers,x
        dex
        bpl @add
        sta total
        rts
.endp

.section
numbers
        .byte 3, 1, 4, 1, 5
.ends
```
<!-- end -->

A **section** is a run of bytes that stays together, and it is a unit of allocation handled by the tool which is given an address. Everything that emits or reserves bytes stands inside one and a statement outside a section is an error. `.res 1`
reserves a byte and emits nothing, so `total` occupies an address and costs the
output file nothing.

A **label** is an identifier in column one with no colon after it. `@add` is a local label, which belongs to the proc it stands in and
may be spelled the same in another.

A **proc** is a section of code with one entry. Nothing outside it may name a
position inside it, which is what makes it the unit that is dropped when
nothing calls it.

Sections can stand in a module any order.

## Building it

```sh
nga main.ngp -o sum.bin --map sum.map
```

`sum.bin` is a raw memory image. The map is where the result of that decision is read:

<!-- map 01-the-project-and-modules/sum -->
```
NGA memory map

phase phase0 (0)
  zero page: 0 of 128 bytes
  memory:    21 of 57344 bytes
  $2000-$2000  sum.total   section            phase0
  $2001-$200F  sum.entry   proc               phase0
  $2010-$2014  sum.numbers section            phase0
```
<!-- end -->

Three rows, for the three sections of `sum.asm`, at `$2000` onwards because
that is where the stand-in leaves memory free. Nothing in the source asked for
that address, or for any address. Change the program and they change with it,
and this is where you look to see what they became.

## The same program in C

<!-- include 01-the-project-and-modules/sum-in-c/sum.ngc -->
```c
static u8 total;

static void entry()
{
  u8 sum = 0;
  for ( u8 i = 0; i < 5; ++i )
    sum += numbers[i];
  total = sum;
}

static const u8 numbers[] = { 3, 1, 4, 1, 5 };
```
<!-- end -->

A `.ngc` file is a module like any other:

<!-- include 01-the-project-and-modules/sum-in-c/main.ngp -->
```
modules { "sum.ngc" }
```
<!-- end -->

The types are the machine's — `u8`, `i8`, `u16`, `i16` — and there is no `int`.

There is no preprocessor so no `#include` and no `#define`.
The rest of this program is C with some C++ flavour.

## What the C became

A `.ngc` module is compiled to the text of an assembler module, and that text
is assembled exactly as the one you wrote by hand. `--emit-asm DIR` writes it
out:

<!-- emit 01-the-project-and-modules/sum-in-c -->
```asm
.section
total
        .res 1
.ends

.proc entry
        lda #0
        tax
        cpx #5
        jcs @l4
@l2
        adc numbers,x
        inx
        cpx #5
        jcc @l2
@l4
        sta total
        rts
.endp

.section
numbers
        .byte 3, 1, 4, 1, 5
.ends
```
<!-- end -->

The compiler has no runtime of its own to hide in and no stack frames to build: it writes the
assembler you would have written, against the same model, and everything the
rest of this tutorial teaches about memory applies to both languages in the
same words.
