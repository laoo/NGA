// The listing of a file from runs written by hand: what is held here is how
// runs become a statement's line, not the tool.

import * as assert from "node:assert/strict";
// Called through the namespace: dprint lays out a bare `describe` and `it` in a
// style of its own, with no space inside the parentheses.
import * as spec from "node:test";

import type { FactsDocument } from "../src/server/Facts.ts";
import { labelOf, listingOf } from "../src/server/Listing.ts";

const facts: FactsDocument = {
  schema: 2,
  nga: "devel",
  diagnostics: [],
  bytes: {
    container: "xex",
    header: [ { address: 0, length: 2, what: "container header" } ],
    segments: [
      {
        runs: [
          {
            address: 0x2100,
            length: 2,
            what: "section",
            source: { path: "/work/one.asm", offset: 12, length: 6 },
            data: "a901"
          },
          {
            address: 0x2102,
            length: 10,
            what: "section",
            source: { path: "/work/one.asm", offset: 30, length: 9 },
            data: "0102030405060708090a"
          },
          {
            address: 0x2200,
            length: 2,
            what: "section",
            source: { path: "/work/two.asm", offset: 0, length: 3 },
            data: "eaea"
          },
          { address: 0x02e0, length: 2, what: "runad" }
        ]
      },
      {
        runs: [
          {
            address: 0x4100,
            length: 2,
            what: "section",
            source: { path: "/work/one.asm", offset: 12, length: 6 },
            data: "a901"
          }
        ]
      }
    ]
  }
};

spec.describe( "a listing", () =>
{
  const listed = listingOf( facts, "/work/one.asm", ( one ) => one );

  spec.it( "has a line for each statement of the file that wrote bytes, and for nothing else", () =>
  {
    assert.deepEqual( listed.map( ( one ) => one.offset ), [ 12, 30 ] );
  } );

  spec.it( "says where a statement landed and what it wrote", () =>
  {
    assert.equal( labelOf( listed[0] ?? { offset: 0, address: 0, data: "", others: 0 } ), "$2100: A9 01 (+1)" );
  } );

  spec.it( "cuts many bytes short and says how many there are", () =>
  {
    assert.equal(
      labelOf( listed[1] ?? { offset: 0, address: 0, data: "", others: 0 } ),
      "$2102: 01 02 03 04 05 06 07 08 … 10 B"
    );
  } );
} );
