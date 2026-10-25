// What the nodes of the tree of a Container's bytes say, from nodes written by
// hand.

import * as assert from "node:assert/strict";
// Called through the namespace: dprint lays out a bare `describe` and `it` in a
// style of its own, with no space inside the parentheses.
import * as spec from "node:test";

import { containerLabel, partLabel, runLabel } from "../src/ByteTree.ts";

spec.describe( "the tree of a Container's bytes", () =>
{
  spec.it( "names the Container by its kind and size", () =>
  {
    assert.equal(
      containerLabel( { project: "/work/main.ngp", container: "xex", size: 1803, header: [], parts: [] } ),
      "xex, 1803 bytes"
    );
  } );

  spec.it( "names a segment by the addresses it loads at, and any other part by where it begins", () =>
  {
    const part = {
      at: 16,
      start: 0x2000,
      end: 0x20f4,
      runs: [ { at: 20, length: 2, address: 0x2000, what: "section" } ]
    };
    assert.equal( partLabel( part, "xex" ), "segment $2000–$20F4, 1 run" );
    assert.equal( partLabel( part, "car" ), "part at byte 16, 1 run" );
  } );

  spec.it( "says where a run lands, how long it is, what wrote it and how it begins", () =>
  {
    assert.equal(
      runLabel( {
        at: 6,
        length: 5,
        address: 0x2001,
        what: "section",
        section: "table",
        module: "one",
        data: "0102030405"
      } ),
      "$2001 5 B  one.table  01 02 03 04 …"
    );
    assert.equal( runLabel( { at: 0, length: 2, address: 0, what: "container header" } ), "2 B  container header" );
  } );
} );
