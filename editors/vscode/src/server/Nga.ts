import { execFile, spawn } from "node:child_process";
import * as fs from "node:fs";
import * as path from "node:path";

import type { Classification, FactsDocument, Vocabulary } from "./Facts.ts";

const EXECUTABLE = process.platform === "win32" ? "nga.exe" : "nga";

// The `nga` to run: the one the reader configured, then the one the VSIX carries
// beside the server, then the one on `PATH`. The carried one is first among the
// last two because its facts are the ones this server was built against — see
// docs/decisions/0258-an-editor-reads-the-facts.md.
export function findNga( configured: string, serverDirectory: string ): string | undefined
{
  if ( configured !== "" )
  {
    return isFile( configured ) ? path.resolve( configured ) : undefined;
  }
  const carried = path.join( serverDirectory, "..", "bin", EXECUTABLE );
  if ( isFile( carried ) )
  {
    // A VSIX is a zip, and what unpacks it need not keep a file executable.
    if ( process.platform !== "win32" )
    {
      try
      {
        fs.chmodSync( carried, 0o755 );
      }
      catch
      {
        // Read-only, and so as it was packed; running it says whether that was enough.
      }
    }
    return carried;
  }
  for ( const directory of ( process.env["PATH"] ?? "" ).split( path.delimiter ) )
  {
    const candidate = path.join( directory, EXECUTABLE );
    if ( directory !== "" && isFile( candidate ) )
    {
      return candidate;
    }
  }
  return undefined;
}

function isFile( candidate: string ): boolean
{
  try
  {
    return fs.statSync( candidate ).isFile();
  }
  catch
  {
    return false;
  }
}

// One run of the tool, which can be abandoned: an older build is cancelled
// rather than waited for when a newer one starts.
export interface Run
{
  facts: Promise<FactsDocument>;
  cancel(): void;
}

export class RunCancelled extends Error
{
}

// `nga PROJECT --facts - --fact SETS`, with no `-o`: a run for the editor writes
// no binary, and every Step runs all the same. The Project is given absolute,
// so that every path the facts name is absolute too, and the tool runs where
// the Project stands, as a reader would run it.
//
// An exit of 1 is a build that failed, and its document is as good as any:
// what the reader wants most of a build that failed is why. Anything else
// without a document is the tool failing, and is said with what it printed.
//
// Each overlay is a file the reader holds with text that is not on disk, and
// the file that text was written to — see docs/spec/project-file.md.
export function runFacts( nga: string, project: string, sets: string[], overlays: [ string, string ][] = [] ): Run
{
  const given = overlays.flatMap( ( [ file, text ] ) => [ "--overlay", file, text ] );
  const child = spawn( nga, [ project, "--facts", "-", "--fact", sets.join( "," ), ...given ], {
    cwd: path.dirname( project ),
    stdio: [ "ignore", "pipe", "pipe" ]
  } );
  let cancelled = false;
  const facts = new Promise<FactsDocument>( ( resolve, reject ) =>
  {
    const out: Buffer[] = [];
    const err: Buffer[] = [];
    child.stdout.on( "data", ( chunk: Buffer ) => out.push( chunk ) );
    child.stderr.on( "data", ( chunk: Buffer ) => err.push( chunk ) );
    child.on( "error", reject );
    child.on( "close", ( code ) =>
    {
      if ( cancelled )
      {
        reject( new RunCancelled() );
        return;
      }
      const text = Buffer.concat( out ).toString( "utf8" );
      if ( code !== 0 && code !== 1 )
      {
        reject( new Error( `${nga} ended with ${String( code )}: ${Buffer.concat( err ).toString( "utf8" )}` ) );
        return;
      }
      try
      {
        resolve( JSON.parse( text ) as FactsDocument );
      }
      catch ( error )
      {
        reject( error );
      }
    } );
  } );
  return {
    facts,
    cancel: () =>
    {
      cancelled = true;
      child.kill();
    }
  };
}

// `nga --classify FILE`, through the overlay of the buffer where the reader
// holds one: the same classes `tokens` gives a file of a build, for any text,
// and without a build — see docs/spec/facts.md.
export function classify( nga: string, file: string, overlay: [ string, string ] | undefined ): Promise<Classification>
{
  const args = [ "--classify", file, ...( overlay === undefined ? [] : [ "--overlay", ...overlay ] ) ];
  return new Promise( ( resolve, reject ) =>
  {
    execFile( nga, args, { encoding: "utf8", maxBuffer: 256 * 1024 * 1024 }, ( error, stdout ) =>
    {
      if ( error !== null )
      {
        reject( error );
        return;
      }
      try
      {
        resolve( JSON.parse( stdout ) as Classification );
      }
      catch ( failed )
      {
        reject( failed );
      }
    } );
  } );
}

// `nga --vocabulary -`: the words of the languages the tool reads, which
// describe the tool and no build, so they are asked once of each `nga`.
export function vocabularyOf( nga: string ): Promise<Vocabulary>
{
  return new Promise( ( resolve, reject ) =>
  {
    execFile( nga, [ "--vocabulary", "-" ], { encoding: "utf8" }, ( error, stdout ) =>
    {
      if ( error !== null )
      {
        reject( error );
        return;
      }
      try
      {
        resolve( JSON.parse( stdout ) as Vocabulary );
      }
      catch ( failed )
      {
        reject( failed );
      }
    } );
  } );
}
