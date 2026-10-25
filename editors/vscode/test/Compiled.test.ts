// Which lines of compiled text came from which line of the C, from the marks
// of a text written by hand.

import * as assert from "node:assert/strict";
// Called through the namespace: dprint lays out a bare `describe` and `it` in a
// style of its own, with no space inside the parentheses.
import * as spec from "node:test";

import { compiledLinesOf } from "../src/server/Compiled.ts";

const TEXT = [
  '.source "sum.ngc", 3', // 0
  ".proc entry",
  '        .source "sum.ngc", 6', // 2
  "        tax",
  "@l2",
  '        .source "sum.ngc", 7', // 5
  "        adc numbers,x",
  '        .source "sum.ngc", 6', // 7
  "        inx",
  "        jcc @l2",
  '        .source "sum.ngc", 9', // 10
  "        rts",
  ".endp"
].join( "\n" );

spec.describe( "compiled text", () =>
{
  const runs = compiledLinesOf( TEXT );

  spec.it( "is cut at every mark, each run the line of the C its mark names", () =>
  {
    assert.deepEqual( runs.map( ( one ) => [ one.source, one.from, one.to ] ), [
      [ 2, 1, 2 ],
      [ 5, 3, 5 ],
      [ 6, 6, 7 ],
      [ 5, 8, 10 ],
      [ 8, 11, 13 ]
    ] );
  } );

  spec.it( "of one line of the C may stand in several places", () =>
  {
    assert.equal( runs.filter( ( one ) => one.source === 5 ).length, 2 );
  } );
} );
