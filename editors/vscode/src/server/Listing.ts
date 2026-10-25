// The listing of a file: the address and the bytes of every statement, read
// off the Container the last build wrote — see docs/spec/facts.md. A
// Container the facts do not read back gives no runs, and so no listing,
// rather than one guessed from the model.

import type { FactsDocument } from "./Facts.ts";

// What one statement wrote: where its first run landed, its bytes, and how
// many other runs it wrote, which a Section the file loads more than once —
// or holds in more than one image — writes.
export interface Listed
{
  offset: number;
  address: number;
  data: string;
  others: number;
}

// The most bytes a hint shows before it says how many there are.
const MOST_BYTES = 8;

// Every statement of a file that wrote bytes, by where it is written; `pathOf`
// turns a path of the facts into the one the file is looked up by.
export function listingOf( facts: FactsDocument, path: string, pathOf: ( path: string ) => string ): Listed[]
{
  const byOffset = new Map<number, Listed>();
  const runs = [ ...( facts.bytes?.header ?? [] ), ...( facts.bytes?.segments ?? [] ).flatMap( ( one ) => one.runs ) ];
  for ( const run of runs )
  {
    if ( run.what !== "section" || run.source === undefined || run.data === undefined )
    {
      continue;
    }
    if ( pathOf( run.source.path ) !== path )
    {
      continue;
    }
    const known = byOffset.get( run.source.offset );
    if ( known === undefined )
    {
      byOffset.set( run.source.offset, { offset: run.source.offset, address: run.address, data: run.data, others: 0 } );
    }
    else
    {
      ++known.others;
    }
  }
  return [ ...byOffset.values() ].sort( ( a, b ) => a.offset - b.offset );
}

// `$2100: A9 01`, the bytes cut short after a few with how many there are.
export function labelOf( listed: Listed ): string
{
  const bytes = listed.data.match( /../g ) ?? [];
  const shown = bytes.slice( 0, MOST_BYTES ).join( " " ).toUpperCase();
  const rest = bytes.length > MOST_BYTES ? ` … ${String( bytes.length )} B` : "";
  const more = listed.others > 0 ? ` (+${String( listed.others )})` : "";
  return `$${listed.address.toString( 16 ).toUpperCase().padStart( 4, "0" )}: ${shown}${rest}${more}`;
}
