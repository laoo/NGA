// Local labels, resolved from a file's tokens without the tool — see
// docs/decisions/0258-an-editor-reads-the-facts.md. The facts carry none: a
// local label names a position in its own Module and has no Symbol. Resolving
// one needs nothing the tool alone has, because its direction is by position
// in the source, never by address — docs/spec/lexical-syntax.md#local-labels.
//
// The rules, all of them positional:
//
// - A `@` label is local to the `.proc` or the macro body around it, and a
//   `.section` block inside a Proc is a scope of its own (NGA2210).
// - A definition in a branch of `.if` or `.match` is visible to a reference in
//   the same branch or one it contains, and to nothing else (NGA2215); a
//   reference in a branch sees what stands outside it.
// - `@name` is the one definition of `name` in the scope, `@+name` the nearest
//   after the reference and `@-name` the nearest before; `@+` and `@-` are the
//   nearest anonymous `@` either way. Named and anonymous labels never answer
//   for each other.
//
// A label this resolves wrongly is a build error the same save reports.

import type { Span } from "./Colours.ts";

export interface Local
{
  offset: number;
  length: number;
}

// Every local reference of a file, by its offset, with the definitions it
// resolves to — several where `@name` is ambiguous, which the tool refuses —
// and every local definition, by its offset.
export interface Locals
{
  references: Map<number, { reference: Local; definitions: Local[]; }>;
  definitions: Map<number, Local>;
}

interface Definition extends Local
{
  name: string;
  scope: number;
  branches: number[];
}

// The scopes a `@` label may stand in, and the branches of a conditional.
type Open = { kind: "scope"; id: number; } | { kind: "section"; id: number | undefined; } | {
  kind: "branch";
  id: number;
} | { kind: "conditional"; };

export function localsOf( text: string, spans: Span[] ): Locals
{
  const bytes = Buffer.from( text, "utf8" );
  const word = ( span: Span ) => bytes.subarray( span[0], span[0] + span[1] ).toString( "utf8" );

  const open: Open[] = [];
  let scopes = 0;
  let branches = 0;
  const scope = () =>
  {
    for ( let i = open.length - 1; i >= 0; --i )
    {
      const one = open[i];
      if ( one?.kind === "scope" || ( one?.kind === "section" && one.id !== undefined ) )
      {
        return one.id;
      }
    }
    return undefined;
  };
  // The branches between the innermost scope and here, outermost first.
  const branchPath = () =>
  {
    const path: number[] = [];
    for ( let i = open.length - 1; i >= 0; --i )
    {
      const one = open[i];
      if ( one?.kind === "scope" || one?.kind === "section" )
      {
        break;
      }
      if ( one?.kind === "branch" )
      {
        path.unshift( one.id );
      }
    }
    return path;
  };
  const closeTo = ( kinds: Open["kind"][] ) =>
  {
    while ( open.length > 0 && !kinds.includes( open[open.length - 1]?.kind ?? "conditional" ) )
    {
      open.pop();
    }
    open.pop();
  };
  const nextBranch = () =>
  {
    while ( open.length > 0 && open[open.length - 1]?.kind === "branch" )
    {
      open.pop();
    }
    open.push( { kind: "branch", id: ++branches } );
  };

  const definitions: Definition[] = [];
  const references: {
    reference: Local;
    name: string;
    direction: "+" | "-" | "";
    scope: number;
    branches: number[];
  }[] = [];

  for ( let i = 0; i < spans.length; ++i )
  {
    const span = spans[i];
    if ( span === undefined )
    {
      continue;
    }
    const [ offset, length, kind ] = span;
    if ( kind === "directive" && word( span ) !== "." )
    {
      switch ( word( span ) )
      {
        case "proc":
        case "macro":
          open.push( { kind: "scope", id: ++scopes } );
          break;
        case "section":
          // Its own scope inside a Proc, and none outside one.
          open.push( { kind: "section", id: scope() === undefined ? undefined : ++scopes } );
          break;
        case "if":
        case "match":
          open.push( { kind: "conditional" } );
          open.push( { kind: "branch", id: ++branches } );
          break;
        case "elsif":
        case "else":
        case "case":
          nextBranch();
          break;
        case "endif":
        case "endi":
        case "endmatch":
          closeTo( [ "conditional" ] );
          break;
        case "endp":
        case "endproc":
        case "endm":
        case "endmacro":
          closeTo( [ "scope" ] );
          break;
        case "ends":
        case "endsection":
          closeTo( [ "section" ] );
          break;
        default:
          break;
      }
      continue;
    }

    const written = word( span );
    const inScope = scope();
    if ( !written.startsWith( "@" ) || inScope === undefined )
    {
      continue;
    }
    if ( kind === "label" )
    {
      definitions.push( { offset, length, name: written.slice( 1 ), scope: inScope, branches: branchPath() } );
    }
    else if ( kind === "name" )
    {
      const direction = written[1] === "+" || written[1] === "-" ? written[1] : "";
      references.push( {
        reference: { offset, length },
        name: written.slice( direction === "" ? 1 : 2 ),
        direction,
        scope: inScope,
        branches: branchPath()
      } );
    }
  }

  const found: Locals = { references: new Map(), definitions: new Map() };
  for ( const one of definitions )
  {
    found.definitions.set( one.offset, { offset: one.offset, length: one.length } );
  }
  for ( const one of references )
  {
    const visible = definitions.filter( ( definition ) =>
      definition.scope === one.scope
      && definition.name === one.name
      && definition.branches.every( ( branch, at ) => one.branches[at] === branch )
    );
    let chosen: Definition[];
    if ( one.direction === "+" )
    {
      chosen = visible.filter( ( definition ) => definition.offset > one.reference.offset ).slice( 0, 1 );
    }
    else if ( one.direction === "-" )
    {
      chosen = visible.filter( ( definition ) => definition.offset < one.reference.offset ).slice( -1 );
    }
    else
    {
      chosen = visible;
    }
    found.references.set( one.reference.offset, {
      reference: one.reference,
      definitions: chosen.map( ( definition ) => ( { offset: definition.offset, length: definition.length } ) )
    } );
  }
  return found;
}
