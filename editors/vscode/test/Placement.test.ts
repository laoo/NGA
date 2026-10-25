// What a hover and a CodeLens say of where a name stands, from facts written
// by hand: what is held here is the wording, not the tool.

import * as assert from "node:assert/strict";
// Called through the namespace: dprint lays out a bare `describe` and `it` in a
// style of its own, with no space inside the parentheses.
import * as spec from "node:test";

import type { FactsDocument, MapSection, SymbolFacts } from "../src/server/Facts.ts";
import { describe, entriesOf, sectionLine } from "../src/server/Placement.ts";

const site = { path: "/work/one.asm", offset: 0, length: 1 };

const proc: MapSection = {
  name: "draw",
  module: "hud",
  kind: "proc",
  begin: 0x2100,
  end: 0x2148,
  firstPhase: 0,
  lastPhase: 1,
  shares: [],
  waits: { as: "zx0", liveFirst: 1, liveLast: 1, pieces: [ { unit: 0, offset: 24, size: 41 } ] }
};

const facts: FactsDocument = {
  schema: 2,
  nga: "devel",
  diagnostics: [],
  project: { path: "/work/main.ngp", regions: [ { name: "PORTB", begin: 0xd301, end: 0xd302, property: "register" } ] },
  map: {
    unit: "bank",
    phases: [ { name: "title" }, { name: "game" }, { name: "over" } ],
    sections: [
      proc,
      {
        name: "a1",
        module: "hud",
        kind: "temporary",
        begin: 0x80,
        end: 0x82,
        firstPhase: 0,
        lastPhase: 2,
        shares: [ "b1" ]
      },
      {
        name: "song",
        module: "hud",
        kind: "section",
        begin: 0x3000,
        end: 0x3100,
        firstPhase: 0,
        lastPhase: 0,
        shares: []
      },
      {
        name: "song",
        module: "hud",
        kind: "section",
        begin: 0x3000,
        end: 0x3100,
        firstPhase: 2,
        lastPhase: 2,
        shares: []
      }
    ],
    pruned: [ { name: "unused", module: "hud", kind: "proc" } ]
  }
};

function line( module: string, name: string ): string
{
  const { entries, pruned } = entriesOf( facts, module, name );
  return sectionLine( entries, pruned, facts );
}

spec.describe( "where a Section stands", () =>
{
  spec.it( "says its range, size, Phases and what it waits as", () =>
  {
    assert.equal( line( "hud", "draw" ), "$2100–$2147 · 72 B · title..game · waits as zx0, 41 B" );
  } );

  spec.it( "says the Temporaries that share its bytes", () =>
  {
    assert.equal( line( "hud", "a1" ), "$0080–$0081 · 2 B · title..over · shares with b1" );
  } );

  spec.it( "says each run of the Phases that hold it", () =>
  {
    assert.equal( line( "hud", "song" ), "$3000–$30FF · 256 B · title, over" );
  } );

  spec.it( "says Prune dropped it", () =>
  {
    assert.equal( line( "hud", "unused" ), "dropped by Prune: nothing reaches it" );
  } );
} );

spec.describe( "a hover", () =>
{
  const symbol = ( fields: Partial<SymbolFacts> ): SymbolFacts => ( {
    name: "draw",
    module: "hud",
    kind: "label",
    definition: site,
    uses: [],
    ...fields
  } );

  spec.it( "on a Proc says where it is, where it stands and where it waits", () =>
  {
    assert.equal(
      describe( symbol( { section: "draw", address: 0x2100 } ), facts ),
      "**draw** — proc of `hud`\n\nat $2100\n\n$2100–$2147 · 72 B · title..game · waits as zx0, 41 B\n\n"
        + "waits as zx0 in bank 0 at $0018, 41 B"
    );
  } );

  spec.it( "on a label inside a Proc says which", () =>
  {
    assert.match(
      describe( symbol( { name: "draw.loop", section: "draw", address: 0x2104 } ), facts ),
      /in `draw`: \$2100/
    );
  } );

  spec.it( "on a Constant and a Region says what they are", () =>
  {
    assert.match( describe( symbol( { name: "step", kind: "constant", value: 3 } ), facts ), /= 3 \(\$0003\)/ );
    assert.match( describe( symbol( { name: "PORTB", kind: "region" } ), facts ), /\$D301–\$D301, register/ );
  } );
} );
