// How a document of the text the tool wrote is named, and read back.

import * as assert from "node:assert/strict";
// Called through the namespace: dprint lays out a bare `describe` and `it` in a
// style of its own, with no space inside the parentheses.
import * as spec from "node:test";

import { isWritten, writtenOf, writtenUri } from "../src/Written.ts";

spec.describe( "a written document", () =>
{
  spec.it( "is named by the path the facts use and the Project that wrote it", () =>
  {
    const uri = writtenUri( "/work/main.ngp", "<generated>/sum.asm" );
    assert.equal( uri, "nga-text:/%3Cgenerated%3E/sum.asm?%2Fwork%2Fmain.ngp" );
    assert.deepEqual( writtenOf( uri ), { project: "/work/main.ngp", file: "<generated>/sum.asm" } );
  } );

  spec.it( "is read back however an editor encoded its URI again", () =>
  {
    assert.deepEqual( writtenOf( "nga-text:/%3cnga%3e/runtime.asm?/work/main.ngp" ), {
      project: "/work/main.ngp",
      file: "<nga>/runtime.asm"
    } );
  } );

  spec.it( "is no file on disk", () =>
  {
    assert.equal( isWritten( "<nga>/runtime.asm" ), true );
    assert.equal( isWritten( "/work/one.asm" ), false );
    assert.equal( writtenOf( "file:///work/one.asm" ), undefined );
  } );
} );
