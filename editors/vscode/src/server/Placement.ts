// What the last build says of where a name stands, in words: for a hover, and
// in one line for a CodeLens above a Section. Read off `symbols` and `map` —
// see docs/spec/facts.md — and deciding nothing.

import type { FactsDocument, MapSection, SymbolFacts } from "./Facts.ts";

// An address as the tool writes one: `$` and four hex digits at least, more
// for a linear address past 64 KB.
export function hex( value: number ): string
{
  return "$" + value.toString( 16 ).toUpperCase().padStart( 4, "0" );
}

// The Phases a run of a Section spans, by name: one, or the first and the last.
function phasesOf( entry: MapSection, facts: FactsDocument ): string
{
  const names = facts.map?.phases ?? [];
  const name = ( index: number | null | undefined ) =>
    index === null || index === undefined ? undefined : names[index]?.name ?? String( index );
  const first = name( entry.firstPhase );
  const last = name( entry.lastPhase );
  if ( first === undefined || last === undefined )
  {
    return "in no Phase";
  }
  return first === last ? first : `${first}..${last}`;
}

// Every run of a Section in the map — a Section held in Phases that are not
// consecutive is one entry for each run — and whether Prune dropped it.
export function entriesOf( facts: FactsDocument, module: string, name: string ): {
  entries: MapSection[];
  pruned: boolean;
}
{
  const same = ( one: MapSection ) => one.module === module && one.name === name;
  return { entries: ( facts.map?.sections ?? [] ).filter( same ), pruned: ( facts.map?.pruned ?? [] ).some( same ) };
}

// One line for a Section: where it stands and how long it is, the Phases that
// hold it, its Pane, what it waits as, and the Temporaries that share its
// bytes.
export function sectionLine( entries: MapSection[], pruned: boolean, facts: FactsDocument ): string
{
  if ( pruned )
  {
    return "dropped by Prune: nothing reaches it";
  }
  const first = entries[0];
  if ( first === undefined || first.begin === undefined || first.end === undefined )
  {
    return "not placed by the last build";
  }
  const parts = [
    `${hex( first.begin )}–${hex( Math.max( first.begin, first.end - 1 ) )}`,
    `${String( first.end - first.begin )} B`,
    entries.map( ( one ) => phasesOf( one, facts ) ).join( ", " )
  ];
  if ( first.pane !== undefined )
  {
    parts.push( `pane ${first.pane}, state ${String( first.paneState ?? 0 )}` );
  }
  if ( first.units !== undefined )
  {
    parts.push( `linear in ${first.units}` );
  }
  const waits = first.waits;
  if ( waits !== undefined )
  {
    const size = waits.pieces.reduce( ( sum, piece ) => sum + piece.size, 0 );
    parts.push( `waits as ${waits.as}, ${String( size )} B` );
  }
  if ( ( first.shares ?? [] ).length > 0 )
  {
    parts.push( `shares with ${( first.shares ?? [] ).join( ", " )}` );
  }
  if ( first.root === true )
  {
    parts.push( "root" );
  }
  if ( first.kept === true )
  {
    parts.push( "kept" );
  }
  return parts.join( " · " );
}

// What a hover says of a Symbol, as Markdown.
export function describe( symbol: SymbolFacts, facts: FactsDocument ): string
{
  const lines = [ `**${symbol.name}** — ${kindOf( symbol, facts )} of \`${symbol.module}\`` ];
  if ( symbol.kind === "label" )
  {
    if ( symbol.address !== undefined )
    {
      lines.push( `at ${hex( symbol.address )}${symbol.units === undefined ? "" : `, linear in ${symbol.units}`}` );
    }
    if ( symbol.section !== undefined )
    {
      const { entries, pruned } = entriesOf( facts, symbol.module, symbol.section );
      const own = symbol.section === symbol.name ? "" : `in \`${symbol.section}\`: `;
      lines.push( own + sectionLine( entries, pruned, facts ) );
      const waits = entries[0]?.waits;
      if ( waits !== undefined )
      {
        const unit = facts.map?.unit ?? "unit";
        const where = waits.pieces.map( ( piece ) =>
          `${unit} ${String( piece.unit )} at ${hex( piece.offset )}, ${String( piece.size )} B`
        );
        lines.push( `waits as ${waits.as} in ${where.join( "; " )}` );
      }
    }
  }
  else if ( symbol.kind === "constant" && symbol.value !== undefined )
  {
    lines.push( `= ${String( symbol.value )} (${hex( symbol.value )})` );
  }
  else if ( symbol.kind === "region" )
  {
    const region = facts.project?.regions?.find( ( one ) => one.name === symbol.name );
    if ( region !== undefined )
    {
      lines.push( `${hex( region.begin )}–${hex( region.end - 1 )}, ${region.property}` );
    }
  }
  return lines.join( "\n\n" );
}

function kindOf( symbol: SymbolFacts, facts: FactsDocument ): string
{
  if ( symbol.kind !== "label" || symbol.section === undefined )
  {
    return symbol.kind;
  }
  const kind = entriesOf( facts, symbol.module, symbol.section ).entries[0]?.kind
    ?? facts.map?.pruned?.find( ( one ) => one.module === symbol.module && one.name === symbol.section )?.kind;
  if ( kind === "proc" && symbol.name === symbol.section )
  {
    return "proc";
  }
  return kind === "temporary" ? "temporary" : "label";
}
