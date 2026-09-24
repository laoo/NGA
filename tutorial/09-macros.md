# Macros

Chapter six drew the map like this:

```asm
        .with level
        jsr drawMap
```

and chapter six's C did not. `[[with]]` there stood over a **block**, so the
loop ran inside the switch and no call was paid; the assembler paid one because
`.with` covers the statement that follows it and a loop is many statements. A
macro use is one statement. This chapter is what chapter six could not write
yet.

## A body, instantiated where it is used

<!-- include 09-macros/inline/level.asm tag=loop -->
```asm
; The drawing loop, as a macro rather than a Proc: a use of one is a single
; statement, which is what `.with` covers, so the whole loop runs with the Pane
; shown and there is no call to make. The bytes it works in are the user's,
; because a macro body holds instructions and data and declares nothing.
.macro drawMap at, row, left
        lda #<levelMap
        sta at
        lda #>levelMap
        sta at+1
        lda #levelRows
        sta left
        lda #0
        sta row
@line
        ldy #levelWidth - 1
@copy
        lda (at),y
        sta levelRow,y
        dey
        bpl @copy

; tag hero
        lda row                 ; where the map said the level starts
        cmp #levelStartRow
        bne @print
        lda #levelHero
        sta levelRow + levelStartColumn
; end hero
@print
        ldx #<levelRow
        ldy #>levelRow
        lda #levelWidth
        jsr printLine

        clc
        lda at
        adc #levelWidth
        sta at
        bcc @counted
        inc at+1
@counted
        inc row
        dec left
        bne @line
.endm
```
<!-- end -->

`.macro NAME params` opens a body and `.endm` closes it. A use is the name
where a mnemonic stands, indented like one, with its arguments after it; a
parameter is a name the body uses wherever a Constant could stand. The body is
assembled **once**, into a template, and each use is a clone of it with the
arguments put in — not text replayed, so a finding in it points at the body and
says which use it came from.

<!-- include 09-macros/inline/level.asm tag=use -->
```asm
.proc levelDraw
at      .ztemp 2
row     .ztemp 1
left    .ztemp 1

        .with level
        drawMap at, row, left
        rts
.endp
```
<!-- end -->

The `.with` covers the use, the use is the whole loop, and the Trampoline of
chapter six is gone: there is nothing left to call.

## What it saved

| | chapter eight | here |
|---|---|---|
| `level.drawMap` | 66 bytes, a Proc | — |
| `level.levelDraw` | 16 bytes, a Proc | 76 bytes |
| the edge's Frame | one more block | one fewer |
| the file | 1163 bytes | 1151 |

Six bytes of code, because the `jsr`, the `rts` and the Proc's own entry are
not there; and six of Frame, because what the edge loads is one Section rather
than two. The Window is switched once either way — a `.with` switches for what
it covers, and what it covers got bigger.

## A macro is not a cheaper Proc

`printLine` is called from four places and stays a Proc. Four copies of its
body would cost more than four calls, and nothing about it needs to be inline.
A macro earns its place where a call **cannot** go — under a `.with`, as here
— or where the body is shorter than the call that would reach it.

The driver has been using one since chapter four without saying so. A role is
declared with `.driver` and expanded wherever the routine writes `nga.read`, so
it has to be a macro; what its body holds is the driver's business, and
`atari/portb.asm` puts a single `jsr` there. A macro is how the tool reaches
code it cannot name, not a promise about what the code looks like.

## What a body may hold

Instructions, data, labels, uses of other macros, and a `.with` over one of its
statements. Not a Section, not a reservation, not a `.transition`:

<!-- include 09-macros/declares/level.asm tag=declares -->
```asm
.macro drawMap row, left
at      .ztemp 2                ; the scratch this loop walks with
        lda #<levelMap
        sta at
        lda #>levelMap
        sta at+1
        lda #levelRows
        sta left
        lda #0
        sta row
@line
        ldy #levelWidth - 1
@copy
        lda (at),y
        sta levelRow,y
        dey
        bpl @copy

; tag hero
        lda row                 ; where the map said the level starts
        cmp #levelStartRow
        bne @print
        lda #levelHero
        sta levelRow + levelStartColumn
; end hero
@print
        ldx #<levelRow
        ldy #>levelRow
        lda #levelWidth
        jsr printLine

        clc
        lda at
        adc #levelWidth
        sta at
        bcc @counted
        inc at+1
@counted
        inc row
        dec left
        bne @line
.endm
```
<!-- end -->

<!-- diagnostics 09-macros/declares -->
```
error[NGA0158]: `.ztemp` cannot stand in a macro body, which holds instructions and data
  --> level.asm:22:9
   |
22 | at      .ztemp 2                ; the scratch this loop walks with
   |         ^^^^^^
```
<!-- end -->

The bytes a macro works in are its user's, which is why `levelDraw` declares
the three Temporaries and hands them over. That is not a restriction working
around something: a body is instantiated in the Section that used it, so a
reservation in one would be a different reservation at every use, and a name
for all of them would mean nothing.
