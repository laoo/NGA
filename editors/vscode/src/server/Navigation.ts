// Where a name is defined and where it is used, read off the last build's
// `symbols` — see docs/spec/facts.md. A use is every place the name is
// written, a `.export` list, a branch the build excluded and the Project file
// among them, which is what lets a rename reach them all.

import { SymbolKind } from "vscode-languageserver/node";

import type { FactsDocument, Site, SymbolFacts } from "./Facts.ts";

// A name written at a site, and the Symbol it is.
export interface Written
{
  site: Site;
  symbol: SymbolFacts;
  definition: boolean;
}

// Every site of every Symbol of one build, by the path of the file it is in.
export class Sites
{
  private mByPath = new Map<string, Written[]>();

  // `pathOf` turns the path the facts name a file by into the one the server
  // looks it up by: the file system's, for a file on disk.
  constructor( facts: FactsDocument, pathOf: ( path: string ) => string )
  {
    for ( const symbol of facts.symbols ?? [] )
    {
      this.add( pathOf, { site: symbol.definition, symbol, definition: true } );
      for ( const use of symbol.uses )
      {
        this.add( pathOf, { site: use, symbol, definition: false } );
      }
    }
    for ( const list of this.mByPath.values() )
    {
      list.sort( ( a, b ) => a.site.offset - b.site.offset );
    }
  }

  // The name written at a byte of a file, ending at it or covering it, so that
  // a cursor just after a name finds the name.
  at( path: string, offset: number ): Written | undefined
  {
    return ( this.mByPath.get( path ) ?? [] ).find( ( one ) =>
      one.site.offset <= offset && offset <= one.site.offset + one.site.length
    );
  }

  // Every Symbol defined in a file, in the order they are written.
  definedIn( path: string ): SymbolFacts[]
  {
    return ( this.mByPath.get( path ) ?? [] ).filter( ( one ) => one.definition ).map( ( one ) => one.symbol );
  }

  private add( pathOf: ( path: string ) => string, written: Written ): void
  {
    if ( written.site.path === "" || written.site.length === 0 )
    {
      return;
    }
    const path = pathOf( written.site.path );
    const list = this.mByPath.get( path );
    if ( list === undefined )
    {
      this.mByPath.set( path, [ written ] );
    }
    else
    {
      list.push( written );
    }
  }
}

// What a Symbol is shown as in an outline. `section` is the kind of Section a
// Label stands in, from `map`.
export function symbolKindOf( symbol: SymbolFacts, section: string | undefined ): SymbolKind
{
  switch ( symbol.kind )
  {
    case "label":
      if ( section === "proc" && symbol.name === symbol.section )
      {
        return SymbolKind.Function;
      }
      return section === "temporary" ? SymbolKind.Variable : SymbolKind.Field;
    case "constant":
      return SymbolKind.Constant;
    case "region":
      return SymbolKind.Property;
    case "macro":
      return SymbolKind.Method;
    case "slot":
      return SymbolKind.Interface;
    default:
      return SymbolKind.Struct;
  }
}

// The kind of Section each Label of a build stands in, by its Module and the
// Section's name.
export function sectionKindsOf( facts: FactsDocument ): Map<string, string>
{
  const kinds = new Map<string, string>();
  for ( const section of [ ...( facts.map?.sections ?? [] ), ...( facts.map?.pruned ?? [] ) ] )
  {
    kinds.set( `${section.module}\n${section.name}`, section.kind );
  }
  return kinds;
}
