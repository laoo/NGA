// The memory map as a page written to a file, by the composition the editor
// shows: what `scripts/map-html.py` was before the editor needed the same page.
//
//   node editors/vscode/dist/MapPage.cjs FACTS.json -o map.html
//   node editors/vscode/dist/MapPage.cjs main.ngp -o map.html   # runs the tool for them
//   node editors/vscode/dist/MapPage.cjs FACTS.json             # to standard output
//
// A Project is read by the `nga` that `NGA_BINARY` names, or the debug build
// of the tree this stands in.

import { execFileSync } from "node:child_process";
import * as fs from "node:fs";
import * as path from "node:path";

import { mapPageOf } from "./MapPage.ts";
import type { FactsDocument } from "./server/Facts.ts";

function factsOf( source: string ): FactsDocument
{
  if ( !source.endsWith( ".ngp" ) )
  {
    return JSON.parse( fs.readFileSync( source, "utf8" ) ) as FactsDocument;
  }
  const nga = process.env["NGA_BINARY"]
    ?? path.join( __dirname, "..", "..", "..", "build", "debug", "src", "nga_cli", "nga" );
  // The tool's exit says whether the build failed; the document is written
  // either way, and says whether there is a map.
  let written: string;
  try
  {
    written = execFileSync( nga, [ path.resolve( source ), "--facts", "-", "--fact", "map,project" ], {
      encoding: "utf8",
      maxBuffer: 256 * 1024 * 1024
    } );
  }
  catch ( error )
  {
    written = ( error as { stdout?: string; } ).stdout ?? "";
  }
  return JSON.parse( written ) as FactsDocument;
}

function main(): number
{
  const args = process.argv.slice( 2 );
  const out = args.indexOf( "-o" );
  const output = out >= 0 ? args[out + 1] : undefined;
  const source = args.find( ( one, i ) => one !== "-o" && ( out < 0 || i !== out + 1 ) );
  if ( source === undefined || ( out >= 0 && output === undefined ) )
  {
    process.stderr.write( "usage: MapPage.cjs FACTS.json|PROJECT.ngp [-o PAGE.html]\n" );
    return 2;
  }
  const facts = factsOf( source );
  if ( facts.map === undefined )
  {
    process.stderr.write( "these facts carry no map; write them with --fact map\n" );
    return 1;
  }
  const page = mapPageOf( facts );
  if ( output === undefined )
  {
    process.stdout.write( page );
  }
  else
  {
    fs.writeFileSync( output, page );
    process.stdout.write( `wrote ${output}\n` );
  }
  return 0;
}

process.exitCode = main();
