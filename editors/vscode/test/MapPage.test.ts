// The memory map as a page, from facts written by hand: what is held here is
// the composition, which the editor and the command line share.

import * as assert from "node:assert/strict";
// Called through the namespace: dprint lays out a bare `describe` and `it` in a
// style of its own, with no space inside the parentheses.
import * as spec from "node:test";

import { colourOf, linkKey, mapPageOf } from "../src/MapPage.ts";
import type { FactsDocument } from "../src/server/Facts.ts";

const facts: FactsDocument = {
  schema: 2,
  nga: "devel",
  diagnostics: [],
  project: {
    path: "/work/main.ngp",
    phases: [ { name: "intro", entry: true, then: [ 1 ] }, { name: "game", then: [ 0 ] } ],
    windows: [ { name: "ext", ranges: [ [ 0x2800, 0x3000 ] ] } ]
  },
  map: {
    unit: "bank",
    unitSize: 16384,
    pools: { zeroPage: [ [ 0x80, 0x100 ] ], general: [ [ 0x2000, 0xc000 ] ], readOnly: [] },
    phases: [ { name: "intro", zeroPageUsed: 2, memoryUsed: 72 }, { name: "game", zeroPageUsed: 2, memoryUsed: 300 } ],
    sections: [
      {
        name: "draw",
        module: "hud",
        kind: "proc",
        begin: 0x2100,
        end: 0x2148,
        initialised: [ 0x2100, 0x2148 ],
        firstPhase: 0,
        lastPhase: 1,
        pinned: true,
        then: "redraw",
        shares: []
      },
      {
        name: "a1",
        module: "hud",
        kind: "temporary",
        begin: 0x80,
        end: 0x82,
        firstPhase: 0,
        lastPhase: 1,
        shares: [ "b1" ]
      },
      {
        name: "level",
        module: "game",
        kind: "section",
        begin: 0x3000,
        end: 0x3100,
        initialised: [ 0x3000, 0x3080 ],
        firstPhase: 1,
        lastPhase: 1,
        align: 256,
        shares: [],
        waits: { as: "zx0", liveFirst: 1, liveLast: 1, pieces: [ { unit: 0, offset: 24, size: 41 } ] }
      }
    ],
    frames: [ { name: "frame intro -> game", unit: 0, offset: 0, size: 12, liveFirst: 0, liveLast: 1 } ],
    units: [ { index: 0, used: 53 } ]
  }
};

const count = ( text: string, what: string ) => text.split( what ).length - 1;

spec.describe( "the page of a memory map", () =>
{
  spec.it( "colours a Module as the page always has", () =>
  {
    assert.equal( colourOf( "hud" ), "#d89177" );
    assert.equal( colourOf( "nga.transition" ), "#7780d8" );
    assert.equal( colourOf( "one" ), "#d877d0" );
  } );

  spec.it( "draws a Section in each Phase that holds it, a Temporary hollow, the pools behind them", () =>
  {
    const page = mapPageOf( facts );
    assert.equal( count( page, 'class="entry"' ), 5 );
    assert.ok( page.includes( 'fill="none" stroke="#d89177" stroke-width="2"' ) );
    assert.equal( count( page, 'class="pool"' ), 2 );
    assert.ok( page.includes( "hud.a1  $0080-$0081  temporary  shares with b1" ) );
  } );

  spec.it( "shades a Window, which the pools leave out, and says what the text asked of a Section", () =>
  {
    const page = mapPageOf( facts );
    assert.equal( count( page, 'class="window"' ), 1 );
    assert.ok( page.includes( "<title>window ext  $2800-$2FFF</title>" ) );
    assert.ok( page.includes( "hud.draw  $2100-$2147  proc  pinned  then redraw<" ) );
    assert.ok( page.includes( "game.level  $3000-$30FF  section  align 256  initialised $3000-$307F" ) );
    assert.ok( page.includes( "hud.a1  $0080-$0081  temporary  shares with b1<" ) );
  } );

  spec.it( "draws a unit set apart, with a Pane's Sections on its Banks, and keeps it out of memory", () =>
  {
    // Four Banks of 16 KB from $10000 behind a Window at $4000; a family of
    // two Panes on Banks 1 and 2, and a Section in the set itself at a
    // linear address the CPU panels must not stretch to.
    const banked: FactsDocument = {
      ...facts,
      project: {
        path: "/work/main.ngp",
        unitSets: [ { name: "extension", count: 4, linear: [ 0x10000, 0x20000 ] } ],
        windows: [ {
          name: "ext",
          ranges: [ [ 0x4000, 0x8000 ] ],
          states: [ { index: 0, units: "extension", count: 4 }, { index: 4, name: "main" } ]
        } ],
        panes: [ { name: "tables", window: "ext", count: 2 }, { name: "low", window: "ext", state: 4 } ]
      },
      map: {
        phases: [ { name: "only" } ],
        sections: [
          { name: "tile", module: "game", kind: "section", begin: 0x4100, end: 0x4200, pane: "tables", paneState: 1 },
          { name: "shown", module: "game", kind: "section", begin: 0x4000, end: 0x4010, pane: "low", paneState: 4 },
          { name: "pixels", module: "game", kind: "section", begin: 0x1c000, end: 0x1d000, units: "extension" }
        ].map( ( one ) => ( { ...one, kind: "section" as const, firstPhase: 0, lastPhase: 0 } ) )
      }
    };
    const page = mapPageOf( banked );
    assert.ok( page.includes( '<h2>Memory <span class="range">$0100-$41FF</span>' ) );
    assert.ok( page.includes( '<h2>extension <span class="range">$10000-$1FFFF</span>' ) );
    assert.ok( page.includes( `data-from="${String( 0x14100 )}" data-to="${String( 0x14200 )}"` ) );
    assert.ok( page.includes( `data-from="${String( 0x18100 )}" data-to="${String( 0x18200 )}"` ) );
    assert.ok( page.includes( `data-from="${String( 0x1c000 )}" data-to="${String( 0x1d000 )}"` ) );
    assert.equal( count( page, `data-from="${String( 0x1c000 )}"` ), 1 );
    assert.equal( count( page, 'class="pool"' ), 2 );
  } );

  spec.it( "lists what waits in storage, a piece and a Frame a row each", () =>
  {
    const page = mapPageOf( facts );
    assert.ok( page.includes( "<td>bank 0 $0018</td><td>game.level <em>zx0</em></td><td>41</td><td>1</td>" ) );
    assert.ok( page.includes( "<td>bank 0 $0000</td><td>frame intro -&gt; game <em>frame</em></td>" ) );
  } );

  spec.it( "draws the Phases and the edges between them, the entry heavier", () =>
  {
    const page = mapPageOf( facts );
    assert.equal( count( page, 'class="edge"' ), 2 );
    assert.ok( page.includes( '<g class="node entry">' ) );
  } );

  spec.it( "links a Section to where it is written, and admits only its own script, where an editor shows it", () =>
  {
    const links = new Map( [ [ linkKey( "hud", "draw" ), { uri: "file:///work/hud.asm", line: 3, character: 0 } ] ] );
    const page = mapPageOf( facts, { links, nonce: "abc" } );
    assert.equal( count( page, 'data-uri="file:///work/hud.asm" data-line="3"' ), 2 );
    assert.ok( page.includes( "script-src 'nonce-abc'" ) && page.includes( '<script nonce="abc">' ) );
    assert.ok( !mapPageOf( facts ).includes( "Content-Security-Policy" ) );
  } );

  spec.it( "says so where the build reached no layout", () =>
  {
    assert.match( mapPageOf( { schema: 2, nga: "devel", diagnostics: [] } ), /reached no layout/ );
  } );
} );
