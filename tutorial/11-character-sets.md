# Character sets

Every literal in this tutorial so far has been the source's own bytes. `.byte
"INTRO"` put `$49 $4E $54 $52 $4F` in the program, the Atari drew INTRO, and
nothing was translated at all. That worked because ATASCII keeps its letters
where ASCII keeps them — a coincidence, and one that stops the moment a
program wants anything but a letter.

Chapter four wanted an end of line and got `$0A`, which the Atari does not use
for one. Chapters eight and nine wrote `$2E` and `$23` and `0x40` with a
comment beside each saying which character it was. That is what a **Charset**
is for.

## A picture that is its own source

<!-- include 11-character-sets/a-box/box.asm tag=box -->
```asm
; The picture is the source. Every character here is the Unicode the Atari's
; own glyph is drawn as, and `atascii` is where the two are put side by side.
; `▄` and `▀` are one glyph and its inverse, which is the same byte with bit
; seven set — and Unicode draws both, so neither is a number here. So is a
; letter in inverse video: `🆂` is `S` with the bit set, and stands in a line of
; ordinary text without the line being cut in two.
.section
box
        .byte atascii"┌───────┐\n"
        .byte atascii"│ HELLO │\n"
        .byte atascii"└───────┘\n"
        .byte atascii"▄▀▄▀▄▀▄▀▄\n"
        .byte atascii"PRESS 🆂\n"
boxEnd
.ends
```
<!-- end -->

On the machine that is a box with HELLO in it, drawn with the Atari's own
graphics characters. Nothing in the source is a number.

`.charset` declares a set, and an entry maps a **run**: the first character of
the string takes the byte on the right and the rest follow it. The Atari's
graphics sit from `$00` — where an ASCII machine keeps control codes — so one
run puts all of them in place, written as the Unicode each glyph is drawn as.

<!-- include ../lib/atari/charsets.asm tag=atascii -->
```asm
.charset atascii
  "♥├▕┘┤┐╱╲◢▗◣▝▘▔▂▖♣┌─┼●▄▎┬┴▌└"                                       = $00
  "↑↓←→"                                                              = $1C
  " !\"#$%&'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_" = $20
  "♦"                                                                 = $60
  "abcdefghijklmnopqrstuvwxyz"                                        = $61
  "♠"                                                                 = $7B
  "│"                                                                 = $7C
  "\n"                                                                = $9B

  ; Inverse video is the same glyph with the bits the other way round, which
  ; for the blocks and the triangles is another character Unicode draws. Every
  ; one of these was read off the machine's own font and is exact; the rest of
  ; the inverse half has no glyph to be written as, and is reached by adding
  ; $80 or by a Charset derived from this one.
  "▊" = $82        ; ▕
  "◤" = $88        ; ◢
  "▛" = $89        ; ▗
  "◥" = $8A        ; ◣
  "▙" = $8B        ; ▝
  "▟" = $8C        ; ▘
  "▆" = $8D        ; ▔
  "▜" = $8F        ; ▖
  "▀" = $95        ; ▄
  "▐" = $99        ; ▌
  "█" = $A0        ; the space

  ; And the letters, which Unicode squares off in a negative.
  "🅰🅱🅲🅳🅴🅵🅶🅷🅸🅹🅺🅻🅼🅽🅾🅿🆀🆁🆂🆃🆄🆅🆆🆇🆈🆉" = $C1
.endch
```
<!-- end -->

A prefix is part of the literal's token: `atascii"..."` with no space. Escapes
are translated like anything else, so `\n` is `$0A` asked of the set and `$9B`
answered — the end of line the Atari actually uses, with nothing exempt and
nothing remembered.

Inverse video is the same glyph with the bits the other way round, `$80` added
to the code, and much of it is another character Unicode draws. The inverse of
`▄` is `▀`, of `▌` is `▐`, of `◢` is `◤`, and of the space is `█`; the letters
Unicode squares off in a negative, so `🆂` is `S` with the bit set. All of them
are in the set, read off the machine's own font, so `atascii"PRESS 🆂"` is seven
bytes and not one of them is written as a number.

For a line of inverse text that is the wrong tool — nobody wants to type
twenty of those. A Charset may be **derived** from another with a transform,
and `.charset bright : atascii ^ $80` is the whole inverse half in one line,
after which `bright"PAUSED"` is six bytes. The character is for the one letter
that stands inside a line of ordinary text, where a second set would mean
cutting the line in two.

## The same letters, other numbers

The display does not read ATASCII. Screen memory holds a code of its own, and
a program that writes where the display looks wants that one.

<!-- include 11-character-sets/two-codes/poke.asm tag=word -->
```asm
; Written where the display looks rather than through the character I/O, so
; these are the display's numbers and not the machine's.
.section
word
        .byte screen"NGA"
wordEnd
.ends
```
<!-- end -->

Those three bytes are `$2E $27 $21`, where `atascii"NGA"` would have been
`$4E $47 $41`. The same word, the same file, two sets, and the difference is
not something to keep in one's head:

<!-- include 11-character-sets/two-codes/poke.asm tag=assert -->
```asm
; The same letter has a number in each, and nothing has to be remembered: the
; tool is held to both, here, at the line that says so.
.assert atascii'A'  == $41
.assert screen'A'   == $21
.assert atascii'\n' == $9B
```
<!-- end -->

`.assert` states a condition checked after Place — the one construct whose
expression cannot be folded early even in principle. A character literal with
a prefix is an `Integer`, so a set can be held to what it says it maps, at the
line that says it.

## What is refused

<!-- include 11-character-sets/unprefixed/name.asm tag=unprefixed -->
```asm
; A literal with no prefix is the source's own bytes, so it has to be ASCII.
.section
name
        .byte "ŻÓŁW"
.ends
```
<!-- end -->

<!-- diagnostics 11-character-sets/unprefixed -->
```
error[NGA2237]: `Ż` is not ASCII and this literal names no character set
 --> name.asm:5:15
  |
5 |         .byte "ŻÓŁW"
  |               ^^^^^^
```
<!-- end -->

An unprefixed literal used as a value must be entirely ASCII. Somebody who
writes `Ż` wanted a translation and did not say which, and quietly emitting
UTF-8 into a 6502 program is the class of wrong answer this tool exists to
prevent. A run inside a `.charset` is the exception, and has to be: it is a
list of code points, and must hold whatever the font draws.

Both sets are in the tool's library, listed by the machine variants, so a
Project that includes one has them by name. A program with a font of its own
declares a set for it the same way, and a set may also be **derived** from
another with a transform — the Atari's inverse video is every code with bit
seven set, which is one line rather than a second table.
