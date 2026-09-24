# Generators

The map chapters six and seven drew was ten lines of `.byte` typed into a
Module. No real one is. A map comes out of an editor, a font out of a drawing
program, a table out of whatever computed it — and what the assembler wants of
them is bytes. A **Generator** is a Module whose text the tool writes rather
than reads, called where a file would be named.

## From a file

<!-- include 08-generators/from-a-file/main.ngp tag=generator -->
```
  binary( "maps/level1.map", section = levelMap, in = level ) as maps
```
<!-- end -->

`binary` makes one Section holding the file's bytes, and takes beside the path
the attributes a `.section` takes — one named argument each. `in = level` puts
it in the Pane of chapter six, exactly as `in level` did when it was written
out; `section = levelMap` is the Label at its start, which is what names a
Section; `as maps` is the Module's name, and `needs` lists it like any other.

`maps/level1.map` is three hundred and seventy bytes: the ten rows end to end
and nothing between them. It has to be, because what `binary` puts in the
Section is the file, byte for byte.

<!-- map 08-generators/from-a-file phase=level rows=maps -->
```
phase level (1)
  zero page: 145 of 256 bytes
  memory:    16661 of 56576 bytes
  ...
  $4000-$4171  maps.levelMap                section            level  in pane level (state 4)
  ...
```
<!-- end -->

A Label stands for its Section, so the drawing loop asks the Section how long
it is rather than being told:

```asm
        lda #levelMap.runtimeSectionSize / levelWidth
```

which is the one thing a file cannot hand over. It has bytes and nothing else
— no count, no width, and no way to say that the first row is the top.

## From a script

A map with no line endings is not a map anybody would keep, and one that cannot
say where the level starts is not much of a level. This one does both:

<!-- include 08-generators/from-a-script/maps/level1.map -->
```
#####################################
#...................................#
#...................................#
#......########.....................#
#......#......#.....................#
#......#......#.....................#
#......########.....................#
#..............@....................#
#...................................#
#####################################
```
<!-- end -->

The `@` marks the start. It is not a character the program should draw, and
where it is is not something the assembler can be told twice without the two
going out of step, so the script takes it out and says where it was.

<!-- include 08-generators/from-a-script/tools/rows.js -->
```js
// A map a person can read, as the bytes a program can draw: the line endings
// come out, and so does the `@` that marks where the level starts. Where it
// was goes back as Constants, which is what a file cannot say for itself.
//
// Two Sections, because the row the program builds each line in is as wide as
// the map and there is no sense in the two knowing that apart.

const START = 0x40;                     // `@`
const FLOOR = 0x2E;                     // `.`

export default ( args, nga ) =>
{
  const map = nga.read( args.map ).filter( b => b !== 10 && b !== 13 );

  if ( map.length % args.width !== 0 )
  {
    nga.error( `${ args.map } is ${ map.length } characters, `
             + `which is no whole number of rows of ${ args.width }` );
  }

  const at = map.indexOf( START );
  if ( at < 0 )
  {
    nga.error( `${ args.map } has no @, so there is nowhere to start` );
  }
  map[ at ] = FLOOR;

  return {
    sections: [
      {
        name: "levelMap",
        in: args.in,
        items: [
          map,
          { constant: "levelWidth", value: args.width },
          { constant: "levelRows", value: map.length / args.width },
          { constant: "levelStartRow", value: Math.floor( at / args.width ) },
          { constant: "levelStartColumn", value: at % args.width }
        ]
      },
      { name: "levelRow", in: args.in, items: [ { reserve: args.width } ] }
    ]
  };
};
```
<!-- end -->

<!-- include 08-generators/from-a-script/main.ngp tag=generator -->
```
  script( "tools/rows.js", map = "maps/level1.map", width = 37, in = level ) as maps
```
<!-- end -->

Two Sections come back, and four Constants. `levelMap` is the same three
hundred and seventy bytes the file was, in the same Pane; `levelRow` is a
reservation as wide as a row, which the drawing code builds each line in so
that the hero can stand on a square without the map being written over. They
are emitted together because the width is the same width, and a generator is
where a thing like that stops being said twice.

The Constants are read as any other name is:

<!-- include 08-generators/from-a-script/level.asm tag=hero -->
```asm
        lda row                 ; where the map said the level starts
        cmp #levelStartRow
        bne @print
        lda #levelHero
        sta levelRow + levelStartColumn
```
<!-- end -->

Nothing of that survives into the program as arithmetic: `levelStartRow` is a
value the comparison is against, and `levelStartColumn` the assembler folds
into the address it writes to.

Below the Project nothing can tell a generated Module from one that was read:
it is named in `needs`, its Sections go in a Pane, and a `transform` would
compress them.

## What a script returns

The default export is called with the arguments and with the tool's own object,
and returns the description of a Module. Two fields:

| | |
|---|---|
| `namespace` | optional: a Namespace around everything the Module declares, so its names come out as `city.tiles` |
| `sections` | the Sections, in the order they are emitted, which is the order their Payloads are packed in |

A section is one to one with `.section`. `name` is the Label at its start,
which is what names it; the rest are the attributes the directive takes —
`placement`, `at`, `align`, `within`, `in`, `movable`, `root`, `temporary` —
and `items`, which is what it holds.

| Item | Becomes |
|---|---|
| a `Uint8Array`, or an array of numbers | those bytes |
| `{ words: Uint16Array }`, or an array of numbers | two bytes each, low byte first |
| `{ reserve: N }` | `.res N`: space that emits nothing |
| `{ constant: "NAME", value: N }` | `NAME = N` |
| `{ label: "NAME" }` | a label and nothing else |

Any item may carry `label: "NAME"` beside it, which puts a label in front of
what it emits, so one Section may hold several names as a hand-written one may.
A generator called twice with different arguments makes two Modules, so
`tools/rows.js` would make a second level out of a second file without a line
of it changing.

One thing a generated Section cannot do is fill a [Slot](07-slots.md):
`.implements` names a Label of the Module it stands in, and a script emits no
directives. The title in this program is still written out for that reason.

## What a script may reach

`nga.read` is the only way in, and every path read joins the build's inputs,
so a changed map is a changed program. Everything else is absent. There is no
clock and no random, because a build that produced different bytes on Tuesday
would be a build nobody could check; there is no filesystem beyond `nga.read`,
and in particular **a directory cannot be listed**, because what a program
contains is a property of a document and never of the state of a folder.

What is there besides the language is a way to complain.

<!-- diagnostics 08-generators/ragged -->
```
error[NGA1212]: maps: maps/level1.map is 110 characters, which is no whole number of rows of 37
 --> main.ngp:4:3
  |
4 |   script( "tools/rows.js", map = "maps/level1.map", width = 37, in = level ) as maps }
  |   ^^^^^^
note: the generator said so at tools/rows.js:18:56
```
<!-- end -->

`nga.error` is a finding like the tool's own: it carries an identifier, it
stops the build, and it points at the call in the Project **and** at the line
of the script that raised it. A generator that knows what its input should
look like can say so, and the person who mis-drew the map reads it where they
read everything else.
