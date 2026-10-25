// What each node of the tree of a Container's bytes says, as the site's tree
// of a program says it: a part by where it stands, a run by where it lands,
// how long it is and what wrote it.

import type { ContainerBytesResult, PartNode, RunNode } from "./Protocol.ts";

function hex( value: number ): string
{
  return "$" + value.toString( 16 ).toUpperCase().padStart( 4, "0" );
}

// The bytes a run shows, a few of them, as the listing shows a statement's.
const MOST_BYTES = 4;

export function containerLabel( bytes: ContainerBytesResult ): string
{
  return `${bytes.container}, ${String( bytes.size )} bytes`;
}

// A segment of a `.xex` by the addresses it loads at; any other part, which
// lands nowhere, by where in the file it begins.
export function partLabel( part: PartNode, container: string ): string
{
  const runs = `${String( part.runs.length )} run${part.runs.length === 1 ? "" : "s"}`;
  if ( container === "xex" )
  {
    return `segment ${hex( part.start )}–${hex( part.end )}, ${runs}`;
  }
  return `part at byte ${String( part.at )}, ${runs}`;
}

export function runLabel( run: RunNode ): string
{
  const bytes = ( run.data?.match( /../g ) ?? [] ).slice( 0, MOST_BYTES ).join( " " ).toUpperCase();
  const what = run.section === undefined ? run.what : `${run.module ?? ""}.${run.section}`;
  const landing = run.address === 0 && run.what !== "section" ? "" : `${hex( run.address )} `;
  return `${landing}${String( run.length )} B  ${what}${
    bytes === "" ? "" : `  ${bytes}${run.length > MOST_BYTES ? " …" : ""}`
  }`;
}
