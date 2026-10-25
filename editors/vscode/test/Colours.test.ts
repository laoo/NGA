// The protocol's encoding of a file's colours, from spans and meanings written
// by hand: what is held here is the encoding, not the tool.

import * as assert from "node:assert/strict";
// Called through the namespace: dprint lays out a bare `describe` and `it` in a
// style of its own, with no space inside the parentheses.
import * as spec from "node:test";

import { namedIn, semanticTokensOf, TOKEN_MODIFIERS, TOKEN_TYPES } from "../src/server/Colours.ts";
import type { Named, Span } from "../src/server/Colours.ts";
import type { FactsDocument } from "../src/server/Facts.ts";

interface Token
{
  line: number;
  character: number;
  length: number;
  type: string;
  modifiers: string[];
}

function decoded( data: number[] ): Token[]
{
  const tokens: Token[] = [];
  let line = 0;
  let character = 0;
  for ( let i = 0; i + 4 < data.length; i += 5 )
  {
    const [ deltaLine = 0, deltaCharacter = 0, length = 0, type = 0, bits = 0 ] = data.slice( i, i + 5 );
    line += deltaLine;
    character = deltaLine === 0 ? character + deltaCharacter : deltaCharacter;
    tokens.push( {
      line,
      character,
      length,
      type: TOKEN_TYPES[type] ?? "?",
      modifiers: TOKEN_MODIFIERS.filter( ( modifier ) => ( bits & ( 1 << TOKEN_MODIFIERS.indexOf( modifier ) ) ) !== 0 )
    } );
  }
  return tokens;
}

function meaning( offset: number, length: number, type: string, modifiers: string[] = [] ): Named
{
  return { offset, length, meaning: { type, modifiers } };
}

spec.describe( "the colours of a file", () =>
{
  spec.it( "follow the classes, and leave a name and punctuation alone", () =>
  {
    const text = ".proc start\n        lda #1 ; one\n.endp\n";
    const spans: Span[] = [
      [ 0, 1, "directive" ],
      [ 1, 4, "directive" ],
      [ 6, 5, "name" ],
      [ 20, 3, "mnemonic" ],
      [ 24, 1, "punctuation" ],
      [ 25, 1, "number" ],
      [ 27, 5, "comment" ],
      [ 33, 1, "directive" ],
      [ 34, 4, "directive" ]
    ];
    assert.deepEqual( decoded( semanticTokensOf( text, spans, [] ) ).map( ( one ) => one.type ), [
      "directive",
      "directive",
      "keyword",
      "number",
      "comment",
      "directive",
      "directive"
    ] );
  } );

  spec.it( "stand where the reader sees them after a character of several bytes", () =>
  {
    // `ż` is two bytes and one character.
    const text = '        .byte "ż", count\n';
    const [ , , , name ] = decoded(
      semanticTokensOf(
        text,
        [ [ 8, 1, "directive" ], [ 9, 4, "directive" ], [ 14, 4, "string" ], [ 20, 5, "name" ] ],
        [
          meaning( 20, 5, "variable" )
        ]
      )
    );
    assert.deepEqual( name, { line: 0, character: 19, length: 5, type: "variable", modifiers: [] } );
  } );

  spec.it( "of a comment over several lines are a token on each", () =>
  {
    const text = "/* one\n   two */ x\n";
    assert.deepEqual( decoded( semanticTokensOf( text, [ [ 0, 16, "comment" ] ], [] ) ), [
      { line: 0, character: 0, length: 6, type: "comment", modifiers: [] },
      { line: 1, character: 0, length: 9, type: "comment", modifiers: [] }
    ] );
  } );

  spec.it( "give a qualified name's meaning to its last part", () =>
  {
    const text = "        jsr level.draw\n";
    const tokens = decoded(
      semanticTokensOf(
        text,
        [ [ 8, 3, "mnemonic" ], [ 12, 5, "name" ], [ 17, 1, "punctuation" ], [ 18, 4, "name" ] ],
        [ meaning( 12, 10, "function" ) ]
      )
    );
    assert.deepEqual( tokens.map( ( one ) => [ one.character, one.type ] ), [ [ 8, "keyword" ], [ 18, "function" ] ] );
  } );
} );

spec.describe( "what a name means", () =>
{
  const site = ( offset: number, length: number ) => ( { path: "/work/one.asm", offset, length } );
  const facts: FactsDocument = {
    schema: 2,
    nga: "devel",
    diagnostics: [],
    map: {
      sections: [
        { name: "start", module: "one", kind: "proc" },
        { name: "count", module: "one", kind: "temporary" }
      ],
      pruned: [ { name: "unused", module: "one", kind: "proc" } ]
    },
    symbols: [
      {
        name: "start",
        module: "one",
        kind: "label",
        section: "start",
        definition: site( 6, 5 ),
        uses: [ site( 90, 5 ) ]
      },
      { name: "start.loop", module: "one", kind: "label", section: "start", definition: site( 20, 4 ), uses: [] },
      { name: "count", module: "one", kind: "label", section: "count", definition: site( 40, 5 ), uses: [] },
      { name: "unused", module: "one", kind: "label", section: "unused", definition: site( 60, 6 ), uses: [] },
      { name: "step", module: "one", kind: "constant", definition: site( 70, 4 ), uses: [] },
      { name: "AUDF1", module: "nga.regions", kind: "region", definition: site( 0, 0 ), uses: [ site( 80, 5 ) ] }
    ]
  };
  const named = namedIn( facts ).get( "/work/one.asm" ) ?? [];
  const at = ( offset: number ) => named.find( ( one ) => one.offset === offset )?.meaning;

  spec.it( "is a function where a Label names the Proc it starts, there and wherever it is used", () =>
  {
    assert.deepEqual( at( 6 ), { type: "function", modifiers: [ "declaration" ] } );
    assert.deepEqual( at( 90 ), { type: "function", modifiers: [] } );
    assert.deepEqual( at( 60 ), { type: "function", modifiers: [ "declaration" ] } );
  } );

  spec.it( "is a label inside a Proc, a variable in a Temporary, and a constant or a region as declared", () =>
  {
    assert.equal( at( 20 )?.type, "label" );
    assert.equal( at( 40 )?.type, "variable" );
    assert.deepEqual( at( 70 ), { type: "variable", modifiers: [ "readonly", "declaration" ] } );
    assert.deepEqual( at( 80 ), { type: "variable", modifiers: [ "static" ] } );
  } );
} );
