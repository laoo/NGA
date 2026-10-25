// What a rename changes, from Symbols and texts written by hand: what is held
// here is where the parts of a written name are, not the tool.

import * as assert from "node:assert/strict";
// Called through the namespace: dprint lays out a bare `describe` and `it` in a
// style of its own, with no space inside the parentheses.
import * as spec from "node:test";

import type { Site, SymbolFacts } from "../src/server/Facts.ts";
import { changesOf, isName } from "../src/server/Rename.ts";

const TEXT = ".proc helper\n        jmp helper.inner\ninner   rts\n.endp\n        jsr one.helper.inner\n";

function site( written: string, nth = 1 ): Site
{
  let at = -1;
  for ( let i = 0; i < nth; ++i )
  {
    at = TEXT.indexOf( written, at + 1 );
  }
  return { path: "/work/two.asm", offset: at, length: written.length };
}

function writtenAt( where: Site ): string
{
  return TEXT.slice( where.offset, where.offset + where.length );
}

function applied( changes: { offset: number; length: number; text: string; }[] ): string
{
  let text = TEXT;
  for ( const change of [ ...changes ].sort( ( a, b ) => b.offset - a.offset ) )
  {
    text = text.slice( 0, change.offset ) + change.text + text.slice( change.offset + change.length );
  }
  return text;
}

spec.describe( "a rename", () =>
{
  const helper: SymbolFacts = {
    name: "helper",
    module: "two",
    kind: "label",
    section: "helper",
    definition: site( "helper" ),
    uses: []
  };
  const inner: SymbolFacts = {
    name: "helper.inner",
    module: "two",
    kind: "label",
    section: "helper",
    definition: site( "inner", 2 ),
    uses: [ site( "helper.inner" ), site( "one.helper.inner" ) ]
  };

  spec.it( "of a Proc changes its Labels' qualified names, and leaves the Labels' own parts", () =>
  {
    assert.equal(
      applied( changesOf( helper, [ inner ], "assist", writtenAt ) ),
      ".proc assist\n        jmp assist.inner\ninner   rts\n.endp\n        jsr one.assist.inner\n"
    );
  } );

  spec.it( "of a Label changes the last part of every place it is written", () =>
  {
    assert.equal(
      applied( changesOf( inner, [], "outer", writtenAt ) ),
      ".proc helper\n        jmp helper.outer\nouter   rts\n.endp\n        jsr one.helper.outer\n"
    );
  } );

  spec.it( "gives a name and nothing else", () =>
  {
    assert.equal( isName( "assist_2" ), true );
    assert.equal( isName( "2assist" ), false );
    assert.equal( isName( "one.assist" ), false );
  } );
} );
