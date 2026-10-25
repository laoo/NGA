import * as fs from "node:fs";
import * as path from "node:path";

import type { ProjectOfResult } from "../Protocol.ts";
import type { FactsDocument } from "./Facts.ts";
import type { Run } from "./Nga.ts";

// What one `.ngp` turned out to be when the tool was run on it: every file its
// build read, and of those the `.ngp` files.
interface Reading
{
  files: Set<string>;
  projectFiles: Set<string>;
}

// How a `.ngp` is read: one run of the tool asking for `project` and `tokens`.
// `tokens` is what lists every file of the build by the path the tool opened,
// including the `.ngp` files it read, which is what tells a Project from a
// fragment some Project includes.
export type Reader = ( project: string ) => Run;

// How many `.ngp` files are read at once. A run is a whole build, and Place may
// take more than one core of its own.
const READING_AT_ONCE = 2;

// Which Project builds a file — see docs/decisions/0258-an-editor-reads-the-facts.md.
//
// The nearest directory above the file that holds a `.ngp` naming it decides.
// A `.ngp` another one of that directory includes is a fragment and not a
// Project: the Kung Fu Master port keeps four Projects and five fragments side
// by side, and a fragment run alone names the files of its Projects too. Where
// more than one Project remains, the first is taken until the reader chooses,
// and the choice holds for every file with the same candidates.
export class Projects
{
  private mReadings = new Map<string, Promise<Reading>>();
  private mChoices = new Map<string, string>();
  private mQueue: Array<() => void> = [];
  private mReading = 0;
  private mRead: Reader;
  private mBoundaries: string[];

  // `boundaries` are the directories the search stops at, which are the
  // workspace's folders; a file outside all of them is searched to the root.
  constructor( read: Reader, boundaries: string[] )
  {
    this.mRead = read;
    this.mBoundaries = boundaries.map( realPathOf );
  }

  // Everything read so far is read again when asked: a `.ngp` changed, and
  // what it names may have changed with it.
  forget(): void
  {
    this.mReadings.clear();
  }

  choose( candidates: string[], project: string ): void
  {
    this.mChoices.set( keyOf( candidates ), project );
  }

  async ownerOf( file: string ): Promise<ProjectOfResult>
  {
    const target = realPathOf( file );
    for ( const directory of searchedFrom( path.dirname( target ), this.mBoundaries ) )
    {
      const projects = projectFilesIn( directory );
      if ( projects.length === 0 )
      {
        continue;
      }
      const readings = new Map(
        await Promise.all( projects.map( async ( one ) => [ one, await this.readingOf( one ) ] as const ) )
      );
      const naming = projects.filter( ( one ) => readings.get( one )?.files.has( target ) === true );
      if ( naming.length === 0 )
      {
        continue;
      }
      const included = ( one: string ) =>
        projects.some( ( other ) => other !== one && readings.get( other )?.projectFiles.has( one ) === true );
      const roots = naming.filter( ( one ) => !included( one ) );
      const candidates = roots.length > 0 ? roots : naming;
      const chosen = this.mChoices.get( keyOf( candidates ) );
      return {
        project: chosen !== undefined && candidates.includes( chosen ) ? chosen : candidates[0] ?? null,
        candidates
      };
    }
    return { project: null, candidates: [] };
  }

  private readingOf( project: string ): Promise<Reading>
  {
    let reading = this.mReadings.get( project );
    if ( reading === undefined )
    {
      reading = this.whenFree( () => this.mRead( project ).facts ).then( readingFrom, () => emptyReading() );
      this.mReadings.set( project, reading );
    }
    return reading;
  }

  private async whenFree<T>( work: () => Promise<T> ): Promise<T>
  {
    if ( this.mReading >= READING_AT_ONCE )
    {
      await new Promise<void>( ( resolve ) => this.mQueue.push( resolve ) );
    }
    ++this.mReading;
    try
    {
      return await work();
    }
    finally
    {
      --this.mReading;
      this.mQueue.shift()?.();
    }
  }
}

// A run that failed outright names nothing; a build that failed still names
// every file it read, since `tokens` is written whenever the Project was.
function readingFrom( facts: FactsDocument ): Reading
{
  const reading = emptyReading();
  for ( const file of facts.tokens ?? [] )
  {
    const where = realPathOf( file.path );
    reading.files.add( where );
    if ( file.role === "project" )
    {
      reading.projectFiles.add( where );
    }
  }
  return reading;
}

function emptyReading(): Reading
{
  return { files: new Set(), projectFiles: new Set() };
}

function keyOf( candidates: string[] ): string
{
  return [ ...candidates ].sort().join( "\n" );
}

// The directory and every one above it, to the first boundary that holds it,
// or to the root where none does.
function* searchedFrom( start: string, boundaries: string[] ): Generator<string>
{
  const boundary = boundaries
    .filter( ( one ) => start === one || start.startsWith( one + path.sep ) )
    .sort( ( a, b ) => b.length - a.length )[0];
  let directory = start;
  for ( ;; )
  {
    yield directory;
    const parent = path.dirname( directory );
    if ( directory === boundary || parent === directory )
    {
      return;
    }
    directory = parent;
  }
}

function projectFilesIn( directory: string ): string[]
{
  try
  {
    return fs
      .readdirSync( directory, { withFileTypes: true } )
      .filter( ( entry ) => entry.isFile() && entry.name.endsWith( ".ngp" ) )
      .map( ( entry ) => path.join( directory, entry.name ) )
      .sort();
  }
  catch
  {
    return [];
  }
}

// A path as the file system holds it, so that the path a reader opened and the
// one the tool was given compare equal through a symbolic link. A path that is
// no file — the tool's own `<nga>/…` and `<generated>/…`, or a buffer never
// saved — stays as it is.
export function realPathOf( file: string ): string
{
  try
  {
    return fs.realpathSync.native( file );
  }
  catch
  {
    return file;
  }
}
