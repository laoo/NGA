// The colours of a file as the protocol's semantic tokens: what the tool's
// classification says each token is, and, for a name, what the last build says
// it means. The classes are docs/spec/facts.md's.

import type { FactsDocument } from "./Facts.ts";
import { sectionKindsOf } from "./Navigation.ts";

// The legend, whose order is the protocol's encoding: a token's type is its
// index here. `directive` is the extension's own, declared in package.json
// over `macro`, so that a theme that knows nothing of it still colours it.
export const TOKEN_TYPES = [
  "comment",
  "string",
  "number",
  "keyword",
  "macro",
  "function",
  "label",
  "variable",
  "type",
  "directive"
];

export const TOKEN_MODIFIERS = [ "declaration", "readonly", "static" ];

// `[offset, length, class]`, both in bytes of the file.
export type Span = [ number, number, string ];

export interface Meaning
{
  type: string;
  modifiers: string[];
}

// Where a name is written in a file, in bytes, and what it means there.
export interface Named
{
  offset: number;
  length: number;
  meaning: Meaning;
}

// What each class is coloured as before anything is known of what a name
// means. A name is not coloured at all until a build says what it is, and
// punctuation never is.
const BY_CLASS = new Map<string, string>( [
  [ "comment", "comment" ],
  [ "label", "label" ],
  [ "directive", "directive" ],
  [ "mnemonic", "keyword" ],
  [ "macro", "macro" ],
  [ "keyword", "keyword" ],
  [ "number", "number" ],
  [ "string", "string" ],
  [ "character", "string" ]
] );

// The protocol's encoding of a file's tokens: five numbers each, the line and
// the character relative to the token before, the length, the type and the
// modifiers as bits. A token that runs over a line ending — a block comment of
// C — is one token per line, since a client is not obliged to take one that
// spans lines.
//
// A name takes its meaning where one ends where the name does: a use of
// `level.levelMap` is written as three tokens and means one Symbol, and the
// last of the three is the one that is coloured as it.
export function semanticTokensOf( text: string, spans: Span[], named: Named[] ): number[]
{
  const bytes = Buffer.from( text, "utf8" );
  const lineStarts = lineStartsOf( bytes );
  const meaningEndingAt = new Map<number, Named>( named.map( ( one ) => [ one.offset + one.length, one ] ) );
  const data: number[] = [];
  let previousLine = 0;
  let previousCharacter = 0;

  for ( const [ offset, length, kind ] of spans )
  {
    const meant = kind === "name" || kind === "label" ? meaningEndingAt.get( offset + length ) : undefined;
    const meaning = meant !== undefined && meant.offset <= offset ? meant.meaning : undefined;
    const type = meaning?.type ?? BY_CLASS.get( kind );
    if ( type === undefined )
    {
      continue;
    }
    const typeIndex = TOKEN_TYPES.indexOf( type );
    const modifiers = ( meaning?.modifiers ?? [] ).reduce(
      ( bits, modifier ) => bits | ( 1 << TOKEN_MODIFIERS.indexOf( modifier ) ),
      0
    );

    let line = lineOf( lineStarts, offset );
    let from = offset;
    const end = offset + length;
    while ( from < end )
    {
      const lineEnd = Math.min( end, contentEndOf( bytes, lineStarts, line ) );
      if ( lineEnd > from )
      {
        const lineStart = lineStarts[line] ?? 0;
        const character = utf16Length( bytes, lineStart, from );
        data.push(
          line - previousLine,
          line === previousLine ? character - previousCharacter : character,
          utf16Length( bytes, from, lineEnd ),
          typeIndex,
          modifiers
        );
        previousLine = line;
        previousCharacter = character;
      }
      ++line;
      from = lineStarts[line] ?? end;
    }
  }
  return data;
}

// What the last build says the names of each file mean, by the path the tool
// named the file by.
//
// A Label is a Proc's name where it names the Proc it stands at the start of,
// and a Temporary's where its Section is one; `map` is what says what kind of
// Section a Label stands in, and a Section Prune dropped is listed there too.
export function namedIn( facts: FactsDocument ): Map<string, Named[]>
{
  const kinds = sectionKindsOf( facts );

  const found = new Map<string, Named[]>();
  const add = ( file: string, offset: number, length: number, meaning: Meaning ) =>
  {
    if ( file === "" || length === 0 )
    {
      return;
    }
    const list = found.get( file );
    const one = { offset, length, meaning };
    if ( list === undefined )
    {
      found.set( file, [ one ] );
    }
    else
    {
      list.push( one );
    }
  };

  for ( const symbol of facts.symbols ?? [] )
  {
    const section = symbol.section === undefined ? undefined : kinds.get( `${symbol.module}\n${symbol.section}` );
    const meaning = meaningOf( symbol.kind, section, symbol.name === symbol.section );
    if ( meaning === undefined )
    {
      continue;
    }
    add( symbol.definition.path, symbol.definition.offset, symbol.definition.length, {
      type: meaning.type,
      modifiers: [ ...meaning.modifiers, "declaration" ]
    } );
    for ( const use of symbol.uses )
    {
      add( use.path, use.offset, use.length, meaning );
    }
  }
  return found;
}

function meaningOf( kind: string, section: string | undefined, namesSection: boolean ): Meaning | undefined
{
  switch ( kind )
  {
    case "label":
      if ( section === "proc" && namesSection )
      {
        return { type: "function", modifiers: [] };
      }
      return { type: section === "temporary" ? "variable" : "label", modifiers: [] };
    case "constant":
      return { type: "variable", modifiers: [ "readonly" ] };
    case "region":
      return { type: "variable", modifiers: [ "static" ] };
    case "macro":
      return { type: "macro", modifiers: [] };
    case "slot":
      return { type: "function", modifiers: [] };
    case "charset":
    case "window":
    case "pane":
      return { type: "type", modifiers: [] };
    default:
      return undefined;
  }
}

// Where every line starts, in bytes. `\n`, `\r\n` and a lone `\r` each end
// one, as the tool counts them.
function lineStartsOf( bytes: Buffer ): number[]
{
  const starts = [ 0 ];
  for ( let i = 0; i < bytes.length; ++i )
  {
    if ( bytes[i] === 0x0a || ( bytes[i] === 0x0d && bytes[i + 1] !== 0x0a ) )
    {
      starts.push( i + 1 );
    }
  }
  return starts;
}

function lineOf( lineStarts: number[], offset: number ): number
{
  let low = 0;
  let high = lineStarts.length - 1;
  while ( low < high )
  {
    const middle = ( low + high + 1 ) >> 1;
    if ( ( lineStarts[middle] ?? 0 ) <= offset )
    {
      low = middle;
    }
    else
    {
      high = middle - 1;
    }
  }
  return low;
}

// Where a line's text ends, before its line ending.
function contentEndOf( bytes: Buffer, lineStarts: number[], line: number ): number
{
  let end = lineStarts[line + 1] ?? bytes.length;
  while ( end > ( lineStarts[line] ?? 0 ) && ( bytes[end - 1] === 0x0a || bytes[end - 1] === 0x0d ) )
  {
    --end;
  }
  return end;
}

function utf16Length( bytes: Buffer, from: number, to: number ): number
{
  return bytes.subarray( from, to ).toString( "utf8" ).length;
}
