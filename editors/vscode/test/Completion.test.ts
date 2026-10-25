// What kind of word stands at the cursor, from the text before it alone.

import * as assert from "node:assert/strict";
// Called through the namespace: dprint lays out a bare `describe` and `it` in a
// style of its own, with no space inside the parentheses.
import * as spec from "node:test";

import { assemblerPlace, projectPlace } from "../src/server/Completion.ts";

spec.describe( "a place in an `.asm`", () =>
{
  spec.it( "after a dot at the head of a statement takes a directive", () =>
  {
    assert.deepEqual( assemblerPlace( ".pro" ), { kind: "directive", typed: "pro" } );
    assert.deepEqual( assemblerPlace( "count   .re" ), { kind: "directive", typed: "re" } );
  } );

  spec.it( "at the head of an indented statement takes a mnemonic or a macro", () =>
  {
    assert.deepEqual( assemblerPlace( "        ld" ), { kind: "statement", typed: "ld" } );
    assert.deepEqual( assemblerPlace( "loop    ld" ), { kind: "statement", typed: "ld" } );
  } );

  spec.it( "in an operand takes a name, a local label with its direction", () =>
  {
    assert.deepEqual( assemblerPlace( "        lda #<OFF" ), { kind: "operand", typed: "OFF" } );
    assert.deepEqual( assemblerPlace( "        bne @-lo" ), { kind: "operand", typed: "@-lo" } );
    assert.deepEqual( assemblerPlace( "        lda value+of" ), { kind: "operand", typed: "of" } );
  } );

  spec.it( "defining a label, in a comment or in a string takes nothing", () =>
  {
    assert.deepEqual( assemblerPlace( "lab" ), { kind: "none" } );
    assert.deepEqual( assemblerPlace( "        lda #1 ; comm" ), { kind: "none" } );
    assert.deepEqual( assemblerPlace( '        .byte "ab' ), { kind: "none" } );
  } );
} );

spec.describe( "a place in a `.ngp`", () =>
{
  spec.it( "in a string takes a path, of the library after `include`", () =>
  {
    assert.deepEqual( projectPlace( 'include "atari/1' ), { kind: "include", typed: "atari/1" } );
    assert.deepEqual( projectPlace( 'modules { "lev' ), { kind: "path", typed: "lev" } );
  } );

  spec.it( "in a Phase takes what its clause names", () =>
  {
    assert.deepEqual( projectPlace( "phase title { needs ti" ), { kind: "names", wanted: "modules", typed: "ti" } );
    assert.deepEqual( projectPlace( "phase title { needs title then le" ), {
      kind: "names",
      wanted: "phases",
      typed: "le"
    } );
    assert.deepEqual( projectPlace( "phase title { needs title  entry ti" ), {
      kind: "names",
      wanted: "labels",
      typed: "ti"
    } );
  } );

  spec.it( "takes a Module or a group where a list of them stands, and a Phase after `entry`", () =>
  {
    assert.deepEqual( projectPlace( "resident { hu" ), { kind: "names", wanted: "modules", typed: "hu" } );
    assert.deepEqual( projectPlace( "phase a { needs a }\nentry ti" ), {
      kind: "names",
      wanted: "phases",
      typed: "ti"
    } );
  } );

  spec.it( "takes a word of the grammar where a statement starts, and nothing in a comment", () =>
  {
    assert.deepEqual( projectPlace( "pha" ), { kind: "word", typed: "pha" } );
    assert.deepEqual( projectPlace( "target {\n  regi" ), { kind: "word", typed: "regi" } );
    assert.deepEqual( projectPlace( "; a comment" ), { kind: "none" } );
  } );
} );
