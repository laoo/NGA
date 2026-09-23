# Temporaries and Liveness

## A section says where it goes

`.section` takes attributes, and the first of them is the placement class.

<!-- include 02-temporaries-and-liveness/two-passes/scan.asm tag=data -->
```asm
.section
input
        .byte 3, 1, 4, 1, 5
.ends

.section zeropage
largest
        .res 1
.ends

.section zeropage
above
        .res 1
.ends
```
<!-- end -->

`absolute` is the default. `zeropage` puts the section below `$0100`, where an instruction reaches it in two bytes instead of three.

The class is declared and never worked out from the address. So programmer declares the placement class and the solver picks the address within it.

`largest` and `above` keep their values. Nothing else may have their bytes.

## A variable that need not survive

Besides placement class the section can be declated as a temporary. Only resevrations are allowed in such section. A section is temporary when its value is not needed while nothing that names it is running, counting a procedure as running until the call it made has returned.

Temporary section can be spelled as:
```asm
.section zeropage, temporary
variable .res 1
.ends
```

or using a shorthand:
```asm
variable .ztemp 1
```

Temporary variable outside of a zeropage can be declated as `.temp 1` which is the same as `.ztemp 1` but without a `zeropage` attribute in its long form.

A section can also be written inside a `.proc`. It does not become part of the
code, which holds instructions and nothing else, and is placed beside it. What
it gains is the procedure's name: inside `entry` it is `best`, and anywhere
else it is `entry.best`, which is how the map lists it.


<!-- include 02-temporaries-and-liveness/two-passes/scan.asm tag=body -->
```asm
.proc entry
best    .ztemp 1
tally   .ztemp 1

        lda #0
        sta best
        ldx #4
@pick   lda input,x
        cmp best
        jcc @kept
        sta best
@kept   dex
        bpl @pick
        lda best
        sta largest

        lda #0
        sta tally
        ldx #4
@count  lda input,x
        cmp #3
        jcc @next
        inc tally
@next   dex
        bpl @count
        lda tally
        sta above
        rts
.endp
```
<!-- end -->

`best` holds the largest byte seen so far, because the accumulator is busy
holding the candidate. `tally` counts, for the same reason. Each is written
with `.ztemp`


## What that buys

<!-- map 02-temporaries-and-liveness/two-passes -->
```
NGA memory map

phase phase0 (0)
  zero page: 3 of 16 bytes
  memory:    50 of 32768 bytes
  $0080-$0080  scan.largest     section            phase0
  $0081-$0081  scan.above       section            phase0
  $0082-$0082  scan.entry.best  temporary          phase0  shares with entry.tally
  $0082-$0082  scan.entry.tally temporary          phase0  shares with entry.best
  $2000-$2004  scan.input       section            phase0
  $2005-$2031  scan.entry       proc               phase0
```
<!-- end -->

`largest` and `above` have a byte each. `best` and `tally` have one byte
between them, and the map names the partner in the last column.

Nothing declared that. The two are finished with at different times — the
largest is found and stored before the counting starts — so they are never
both holding a value at once, and one byte does for both.

## The same work in one pass

Both loops read the same five bytes, so an obvious improvement is to do both
jobs in one pass over them.

<!-- include 02-temporaries-and-liveness/one-pass/scan.asm tag=body -->
```asm
.proc entry
best    .ztemp 1
tally   .ztemp 1

        lda #0
        sta best
        sta tally
        ldx #4
@scan   lda input,x
        cmp best
        jcc @kept
        sta best
@kept   lda input,x
        cmp #3
        jcc @next
        inc tally
@next   dex
        bpl @scan
        lda best
        sta largest
        lda tally
        sta above
        rts
.endp
```
<!-- end -->

<!-- map 02-temporaries-and-liveness/one-pass -->
```
NGA memory map

phase phase0 (0)
  zero page: 4 of 16 bytes
  memory:    43 of 32768 bytes
  $0080-$0080  scan.entry.tally temporary          phase0
  $0081-$0081  scan.entry.best  temporary          phase0
  $0082-$0082  scan.above       section            phase0
  $0083-$0083  scan.largest     section            phase0
  $2000-$2004  scan.input       section            phase0
  $2005-$202A  scan.entry       proc               phase0
```
<!-- end -->

They no longer share. Now `best` is holding a value while `tally` is being
counted up, and neither can have the other's byte.

That is the trade, and the map is where it is stated: one pass is 38 bytes of
code against 45, and costs one more byte of the zero page. Neither number was
asked for. You wrote a faster loop, and the tool told you what it cost
somewhere else.

## Between procedures

Inside one procedure it is enough to ask which lines hold a value. Between
procedures the question is which of them can be running at the same time, and
the answer is a stack: a `jsr` pushes, an `rts` pops, and a procedure is
*active* from the call into it until the return out of it.

Two procedures that are never on that stack together can share everything.
`findLargest` and `countAbove` are called one after the other and neither
calls the other, so their scratch bytes are the same byte.

One procedure in this program is different:

<!-- include 02-temporaries-and-liveness/three-procs/scan.asm tag=report -->
```asm
.proc report
least   .ztemp 1
        lda #$FF
        sta least
        ldx #4
@pick   lda input,x
        cmp least
        jcs @kept
        sta least
@kept   dex
        bpl @pick
        jsr findLargest
        lda largest
        sec
        sbc least
        sta spread
        rts
.endp
```
<!-- end -->

`least` is still needed after `jsr findLargest`, so for the length of that call
two procedures are on the stack and both of them are holding a value.

<!-- map 02-temporaries-and-liveness/three-procs -->
```
NGA memory map

phase phase0 (0)
  zero page: 5 of 16 bytes
  memory:    90 of 32768 bytes
  $0080-$0080  scan.largest          section            phase0
  $0081-$0081  scan.above            section            phase0
  $0082-$0082  scan.spread           section            phase0
  $0083-$0083  scan.countAbove.tally temporary          phase0  shares with findLargest.best
  $0083-$0083  scan.findLargest.best temporary          phase0  shares with countAbove.tally
  $0084-$0084  scan.report.least     temporary          phase0
  $2000-$2004  scan.input            section            phase0
  $2005-$200E  scan.entry            proc               phase0
  $200F-$2025  scan.findLargest      proc               phase0
  $2026-$203C  scan.countAbove       proc               phase0
  $203D-$2059  scan.report           proc               phase0
```
<!-- end -->

`findLargest` and `countAbove` share `$0083`. `report` has `$0084` to itself,
because its byte is live across a call and the callee's is live inside it.

## Where the tool gets this from

It reads the program. Every use of a name is a reference the tool can see, and
a byte cannot be reached any other way: taking a temporary's address is refused
unless the source says what follows it. So the lifetime of every byte is
something the tool works out rather than something you assert, and adding a
line that reads a variable later can only make its life longer, never shorter.

What it cannot see is what is not in the program. `temporary` is still your
word, and the chapter on phases is where that matters.

## In C you write none of it

Every local variable of a C function is a temporary of its procedure. There is
no declaration to write and no attribute to remember.

<!-- include 02-temporaries-and-liveness/in-c/scan.ngc -->
```c
static const u8 input[] = { 3, 1, 4, 1, 5 };

static u8 largest;
static u8 above;
static u8 spread;

static void findLargest()
{
  u8 best = 0;
  for ( u8 i = 0; i < 5; ++i )
    if ( input[i] > best )
      best = input[i];
  largest = best;
}

static void countAbove()
{
  u8 tally = 0;
  for ( u8 i = 0; i < 5; ++i )
    if ( input[i] >= 3 )
      tally += 1;
  above = tally;
}

static void report()
{
  u8 least = 255;
  for ( u8 i = 0; i < 5; ++i )
    if ( input[i] < least )
      least = input[i];
  findLargest();
  spread = largest - least;
}

void entry()
{
  findLargest();
  countAbove();
  report();

  countAbove();
  report();
}
```
<!-- end -->

<!-- map 02-temporaries-and-liveness/in-c -->
```
NGA memory map

phase phase0 (0)
  zero page: 1 of 16 bytes
  memory:    102 of 32768 bytes
  $0080-$0080  scan.countAbove.__0tally temporary          phase0  shares with report.__0least
  $0080-$0080  scan.report.__0least     temporary          phase0  shares with countAbove.__0tally
  $2000-$2004  scan.input               section            phase0
  $2005-$2005  scan.largest             section            phase0
  $2006-$2006  scan.above               section            phase0
  $2007-$2007  scan.spread              section            phase0
  $2008-$201B  scan.findLargest         proc               phase0
  $201C-$2034  scan.countAbove          proc               phase0
  $2035-$2055  scan.report              proc               phase0
  $2056-$2065  scan.entry               proc               phase0
```
<!-- end -->

Two things in that map are worth reading twice.

`countAbove` and `report` have one byte between them, exactly as the assembler
versions did, and for the same reason. The compiler declared nothing and merged
nothing: it gave each local a byte of its own and left the packing to the same
step that packs a hand-written `.ztemp`.

And `findLargest` is not there at all. Its local never reached memory, because
the loop can keep the largest byte so far in the accumulator and compare
against the array directly. A local costs a byte when it needs one, which is
not the same as whenever you write `u8`.
