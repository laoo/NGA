// Which lines of what a `.ngc` compiled to came from which line of the C, read
// off the `.source` marks the compiled text carries: a mark covers what
// follows it to the next one — see docs/spec/syntax.md and
// docs/decisions/0063-source-marks.md.

import type { CompiledLines } from "../Protocol.ts";

const MARK = /^\s*\.source\s+"((?:[^"\\]|\\.)*)"\s*,\s*(\d+)/;

// Every run of compiled lines one line of the C became, in the order they
// stand; a line of the C that became code in several places is several runs.
// Lines count from zero, as the protocol counts them, and a run excludes the
// mark itself.
export function compiledLinesOf( text: string ): CompiledLines[]
{
  const lines = text.split( /\r\n|\r|\n/ );
  const runs: CompiledLines[] = [];
  for ( let i = 0; i < lines.length; ++i )
  {
    const mark = MARK.exec( lines[i] ?? "" );
    if ( mark === null )
    {
      continue;
    }
    const last = runs[runs.length - 1];
    if ( last !== undefined )
    {
      last.to = i;
    }
    runs.push( { source: Number( mark[2] ) - 1, from: i + 1, to: lines.length } );
  }
  return runs.filter( ( run ) => run.to > run.from );
}
