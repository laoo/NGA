// Local labels resolved from the tool's own tokens of a file: the text is
// classified by `nga --classify`, and what is held here is what the server
// makes of the tokens.

import * as assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
// Called through the namespace: dprint lays out a bare `describe` and `it` in a
// style of its own, with no space inside the parentheses.
import * as spec from "node:test";

import type { Span } from "../src/server/Colours.ts";
import { localsOf } from "../src/server/Locals.ts";

const ROOT = path.resolve( import.meta.dirname, "..", "..", ".." );
const NGA = process.env["NGA_BINARY"] ?? path.join( ROOT, "build", "debug", "src", "nga_cli", "nga" );

const TEXT = [
  ".proc start",
  "@loop   dex", // 1
  "        bne @-loop",
  "@       nop", // 3
  "        beq @+",
  "        bne @loop",
  "@", // 6
  "        jmp @-",
  ".section",
  "@loop   .byte 1", // 9: a scope of its own
  ".ends",
  ".if 1",
  "@y      nop", // 12
  "        bne @-y",
  ".else",
  "        bne @-y", // 15: the other branch sees nothing
  ".endif",
  ".endp",
  ".macro m",
  "@loop   dex", // 19
  "        bne @-loop",
  ".endm",
  ""
].join( "\n" );

// The byte a reference starts at, found as the `nth` place `context` is written
// and the reference inside it.
function offsetOf( context: string, reference: string, nth = 1 ): number
{
  let at = -1;
  for ( let i = 0; i < nth; ++i )
  {
    at = TEXT.indexOf( context, at + 1 );
  }
  return Buffer.byteLength( TEXT.slice( 0, at + context.indexOf( reference ) ), "utf8" );
}

function lineOf( offset: number ): number
{
  return Buffer.from( TEXT, "utf8" ).subarray( 0, offset ).toString( "utf8" ).split( "\n" ).length - 1;
}

spec.describe( "a local label", () =>
{
  let spans: Span[] = [];

  spec.before( () =>
  {
    const directory = fs.mkdtempSync( path.join( os.tmpdir(), "nga-locals-" ) );
    fs.writeFileSync( path.join( directory, "one.asm" ), TEXT );
    const done = spawnSync( NGA, [ "--classify", "one.asm" ], { cwd: directory, encoding: "utf8" } );
    spans = ( JSON.parse( done.stdout ) as { tokens: { spans: Span[]; }[]; } ).tokens[0]?.spans ?? [];
    fs.rmSync( directory, { recursive: true, force: true } );
  } );

  const resolved = ( context: string, reference: string, nth = 1 ) =>
    ( localsOf( TEXT, spans ).references.get( offsetOf( context, reference, nth ) )?.definitions ?? [] ).map( ( one ) =>
      lineOf( one.offset )
    );

  spec.it( "reached backwards or forwards is the nearest that way, named or not", () =>
  {
    assert.deepEqual( resolved( "bne @-loop", "@-loop" ), [ 1 ] );
    assert.deepEqual( resolved( "beq @+", "@+" ), [ 6 ] );
    assert.deepEqual( resolved( "jmp @-", "@-" ), [ 6 ] );
  } );

  spec.it( "named without a direction is the one of its name in the scope", () =>
  {
    assert.deepEqual( resolved( "bne @loop", "@loop" ), [ 1 ] );
  } );

  spec.it( "in a block of a Proc, or in a macro body, is a scope's own", () =>
  {
    assert.deepEqual( resolved( "bne @-loop", "@-loop", 2 ), [ 19 ] );
  } );

  spec.it( "in one branch is seen from that branch and not from another", () =>
  {
    assert.deepEqual( resolved( "bne @-y", "@-y" ), [ 12 ] );
    assert.deepEqual( resolved( "bne @-y", "@-y", 2 ), [] );
  } );
} );
