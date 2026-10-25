// What a rename changes, from the last build's `symbols`, whose uses are every
// place a name is written — see docs/spec/facts.md.
//
// A name is written whole or qualified: `start`, `start.inner`, `one.start`.
// What a rename replaces is the part that is the Symbol's own, the last one.
// A Proc is a scope, though, and its Labels are named under it: renaming the
// Proc renames `start.inner` too, so where a Label of it is written qualified,
// the part that is the Proc's name changes with it.

import type { Site, SymbolFacts } from "./Facts.ts";

// One change to one file, in bytes of the text the build read.
export interface Change
{
  path: string;
  offset: number;
  length: number;
  text: string;
}

// The text a site writes, as the file holds it.
export type WrittenAt = ( site: Site ) => string;

// A name a rename may give: one part, as an identifier is spelled.
export function isName( name: string ): boolean
{
  return /^[A-Za-z_][A-Za-z0-9_]*$/.test( name );
}

// The last part of a qualified name.
export function lastPartOf( name: string ): string
{
  return name.slice( name.lastIndexOf( "." ) + 1 );
}

// Every change renaming `symbol` to `name`: its definition and every use, each
// at its last part, and the Proc's own part of every qualified use of the
// Labels it holds, which `inner` lists.
export function changesOf( symbol: SymbolFacts, inner: SymbolFacts[], name: string, writtenAt: WrittenAt ): Change[]
{
  const changes: Change[] = [];
  for ( const site of [ symbol.definition, ...symbol.uses ] )
  {
    const written = writtenAt( site );
    const last = written.lastIndexOf( "." ) + 1;
    changes.push( { path: site.path, offset: site.offset + last, length: written.length - last, text: name } );
  }

  // `start.inner` written for a Label of the Proc `start`: the part before the
  // Label's own name is the Proc's, and it is replaced where it is written.
  const own = lastPartOf( symbol.name );
  for ( const label of inner )
  {
    const rest = label.name.slice( symbol.name.length + 1 );
    for ( const site of label.uses )
    {
      const written = writtenAt( site );
      const proc = written.length - rest.length - 1 - own.length;
      if ( written.endsWith( `${own}.${rest}` ) && proc >= 0 && ( proc === 0 || written[proc - 1] === "." ) )
      {
        changes.push( { path: site.path, offset: site.offset + proc, length: own.length, text: name } );
      }
    }
  }
  return changes;
}
