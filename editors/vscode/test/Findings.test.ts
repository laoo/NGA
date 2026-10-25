// Where a finding is shown and how it reads, from a document written by hand:
// what is held here is the conversion, not the tool, so no tool runs.

import * as assert from "node:assert/strict";
// Called through the namespace: dprint lays out a bare `describe` and `it` in a
// style of its own, with no space inside the parentheses.
import * as spec from "node:test";
import { pathToFileURL } from "node:url";

import type { FactsDocument } from "../src/server/Facts.ts";
import { characterOf, findingsOf } from "../src/server/Findings.ts";
import { writtenUri } from "../src/Written.ts";

const PROJECT = "/work/main.ngp";
const TEXTS = new Map<string, string>( [
  [ PROJECT, 'modules { "one.asm" }\n' ],
  // An em dash is three bytes and one UTF-16 code unit; `ż` is two and one.
  [ "/work/one.asm", "; żółw — a comment\n        lda missing ; ż\n" ],
  [ "/work/one.ngc", "void start()\n{\n  helper();\n}\n" ]
] );

function textOf( file: string ): string | undefined
{
  return TEXTS.get( file );
}

function uriOf( file: string ): string
{
  return pathToFileURL( file ).toString();
}

function document( diagnostics: FactsDocument["diagnostics"] ): FactsDocument
{
  return { schema: 2, nga: "devel", diagnostics };
}

spec.describe( "a column", () =>
{
  spec.it( "is counted in UTF-16 where the tool counts bytes", () =>
  {
    // `—` starts at byte 11 (one-based), after `; żółw `, which is ten bytes and
    // seven characters, and its three bytes are one character.
    assert.equal( characterOf( "; żółw — a comment", 11 ), 7 );
    assert.equal( characterOf( "; żółw — a comment", 14 ), 8 );
  } );

  spec.it( "past the end of its line is the end", () =>
  {
    assert.equal( characterOf( "abc", 10 ), 3 );
  } );
} );

spec.describe( "a finding", () =>
{
  spec.it( "is shown where it is located, its identifier linked to the catalogue", () =>
  {
    const shown = findingsOf(
      document( [ {
        id: "NGA2206",
        severity: "error",
        message: "`missing` is not defined",
        location: { file: "/work/one.asm", line: 2, column: 13, endLine: 2, endColumn: 20 }
      } ] ),
      PROJECT,
      textOf
    );
    const [ diagnostic ] = shown.get( uriOf( "/work/one.asm" ) ) ?? [];
    assert.deepEqual( diagnostic?.range, { start: { line: 1, character: 12 }, end: { line: 1, character: 19 } } );
    assert.equal( diagnostic?.code, "NGA2206" );
    assert.equal( diagnostic?.codeDescription?.href, "https://laoo.github.io/NGA/diagnostics/#nga2206" );
    assert.equal( diagnostic?.severity, 1 );
  } );

  spec.it( "after a character of several bytes stands where the reader sees it", () =>
  {
    const shown = findingsOf(
      document( [ {
        id: "NGA0001",
        severity: "warning",
        message: "a dash",
        location: { file: "/work/one.asm", line: 1, column: 11, endLine: 1, endColumn: 14 }
      } ] ),
      PROJECT,
      textOf
    );
    assert.deepEqual( shown.get( uriOf( "/work/one.asm" ) )?.[0]?.range, {
      start: { line: 0, character: 7 },
      end: { line: 0, character: 8 }
    } );
  } );

  spec.it( "with no location is the Project's, and its notes say where to look", () =>
  {
    const shown = findingsOf(
      document( [ {
        id: "NGA5631",
        severity: "error",
        message: "phase `main` does not fit",
        notes: [
          {
            id: "NGA5633",
            severity: "note",
            message: "`big` accounts for 300 bytes",
            location: { file: "/work/one.asm", line: 2, column: 9, endLine: 2, endColumn: 12 }
          },
          {
            id: "NGA5633",
            severity: "note",
            message: "`ngaLoad` accounts for 40 bytes",
            location: { file: "<nga>/load.asm", line: 3, column: 1 }
          },
          { id: "NGA9999", severity: "note", message: "nowhere at all" }
        ]
      } ] ),
      PROJECT,
      textOf
    );
    const [ diagnostic ] = shown.get( uriOf( PROJECT ) ) ?? [];
    assert.deepEqual( diagnostic?.range, { start: { line: 0, character: 0 }, end: { line: 0, character: 0 } } );
    assert.deepEqual( diagnostic?.relatedInformation, [ {
      location: {
        uri: pathToFileURL( "/work/one.asm" ).toString(),
        range: { start: { line: 1, character: 8 }, end: { line: 1, character: 11 } }
      },
      message: "`big` accounts for 300 bytes"
    } ] );
    assert.equal(
      diagnostic?.message,
      "phase `main` does not fit\n`ngaLoad` accounts for 40 bytes (<nga>/load.asm:3:1)\nnowhere at all"
    );
  } );

  spec.it( "in what a `.ngc` compiled to is shown on the line of the C it came from", () =>
  {
    const shown = findingsOf(
      {
        ...document( [ {
          id: "NGA2206",
          severity: "error",
          message: "`helper` is not defined",
          location: { file: "<generated>/one.asm", line: 4, column: 13 },
          source: { file: "one.ngc", line: 3 }
        } ] ),
        sources: [ {
          module: "one",
          path: "<generated>/one.asm",
          role: "compiled",
          text: "\n\n\n        jsr helper\n"
        } ]
      },
      PROJECT,
      textOf
    );
    assert.deepEqual( shown.get( uriOf( "/work/one.ngc" ) )?.[0]?.range, {
      start: { line: 2, character: 0 },
      end: { line: 2, character: 11 }
    } );
  } );

  spec.it( "in text the tool wrote is shown in that text, which `sources` holds", () =>
  {
    const shown = findingsOf(
      {
        ...document( [ {
          id: "NGA5202",
          severity: "error",
          message: "there is no room for `__mul8`",
          location: { file: "<nga>/runtime.asm", line: 2, column: 7, endLine: 2, endColumn: 13 }
        } ] ),
        sources: [ { module: "nga.runtime", path: "<nga>/runtime.asm", role: "source", text: "; —\n.proc __mul8\n" } ]
      },
      PROJECT,
      textOf
    );
    assert.deepEqual( shown.get( writtenUri( PROJECT, "<nga>/runtime.asm" ) )?.[0]?.range, {
      start: { line: 1, character: 6 },
      end: { line: 1, character: 12 }
    } );
  } );

  spec.it( "in text the tool wrote, and nowhere else, is the Project's and says where it was", () =>
  {
    const shown = findingsOf(
      document( [ {
        id: "NGA5202",
        severity: "error",
        message: "there is no room for `__mul8`",
        location: { file: "<nga>/runtime.asm", line: 20, column: 1, endLine: 20, endColumn: 6 }
      } ] ),
      PROJECT,
      textOf
    );
    assert.equal(
      shown.get( uriOf( PROJECT ) )?.[0]?.message,
      "there is no room for `__mul8` (<nga>/runtime.asm:20:1)"
    );
  } );

  spec.it( "of Prune is the dropped Section's text, faded and said to be dropped", () =>
  {
    const shown = findingsOf(
      {
        ...document( [] ),
        map: {
          sections: [],
          pruned: [ {
            name: "unused",
            module: "one",
            kind: "proc",
            // The second line: the first is 23 bytes and its line ending.
            text: [ { path: "/work/one.asm", offset: 24, length: 19 } ]
          } ]
        }
      },
      PROJECT,
      textOf
    );
    const [ faded ] = shown.get( uriOf( "/work/one.asm" ) ) ?? [];
    assert.deepEqual( faded?.range, { start: { line: 1, character: 0 }, end: { line: 1, character: 19 } } );
    assert.deepEqual( faded?.tags, [ 1 ] );
    assert.equal( faded?.severity, 4 );
    assert.match( String( faded?.message ?? "" ), /`unused` is dropped by Prune/ );
  } );
} );
