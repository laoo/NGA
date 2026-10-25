// Where a completion is asked for, read off the text before the cursor: what
// kind of word stands there, and how much of it is typed. What may stand there
// is the server's to say, from the tool's vocabulary and the last build.

// In an `.asm`: a directive after its dot, the head of a statement, where a
// mnemonic or a macro stands, or an operand, where a name does. A label being
// defined, a comment and a string take nothing.
export type AssemblerPlace =
  | { kind: "directive"; typed: string; }
  | { kind: "statement"; typed: string; }
  | { kind: "operand"; typed: string; }
  | { kind: "none"; };

export function assemblerPlace( line: string ): AssemblerPlace
{
  if ( inCommentOrString( line, ";" ) )
  {
    return { kind: "none" };
  }
  const head = /^([A-Za-z_@][\w.]*)?(\s*)(\.?)([\w@.]*)$/.exec( line );
  if ( head !== null )
  {
    const [ , label = "", space = "", dot = "", typed = "" ] = head;
    if ( dot === "." && ( label === "" || space !== "" ) )
    {
      return { kind: "directive", typed };
    }
    if ( dot === "" && space !== "" )
    {
      return { kind: "statement", typed };
    }
    return { kind: "none" };
  }
  // A direction belongs to a local label's name, and to nothing else's.
  return { kind: "operand", typed: /(@[+-]?[\w.]*|[A-Za-z_][\w.]*)$/.exec( line )?.[0] ?? "" };
}

// In a `.ngp`: a path to `include` from the library, a path of a Module, the
// names a block's clause takes — Modules, groups, Phases or Labels — or a word
// of the grammar.
export type ProjectPlace =
  | { kind: "include"; typed: string; }
  | { kind: "path"; typed: string; }
  | { kind: "names"; wanted: "modules" | "phases" | "labels"; typed: string; }
  | { kind: "word"; typed: string; }
  | { kind: "none"; };

// The clauses of a block that name something, by the word that opens them.
const NAMING = new Map<string, "modules" | "phases" | "labels">( [
  [ "needs", "modules" ],
  [ "without", "modules" ],
  [ "then", "phases" ]
] );

export function projectPlace( before: string ): ProjectPlace
{
  // Comments, strings and blocks, read from the start: a `;` to the line's end
  // is a comment, and a `{` opens a block named by the words before it.
  const blocks: string[] = [];
  let stringStart = -1;
  let statement = 0;
  for ( let i = 0; i < before.length; ++i )
  {
    const c = before[i];
    if ( stringStart >= 0 )
    {
      if ( c === "\\" )
      {
        ++i;
      }
      else if ( c === '"' )
      {
        stringStart = -1;
      }
      continue;
    }
    if ( c === ";" )
    {
      const end = before.indexOf( "\n", i );
      if ( end < 0 )
      {
        return { kind: "none" };
      }
      i = end;
      continue;
    }
    if ( c === '"' )
    {
      stringStart = i;
    }
    else if ( c === "{" )
    {
      blocks.push( /([A-Za-z_]\w*)[^{}]*$/.exec( before.slice( statement, i ) )?.[1] ?? "" );
      statement = i + 1;
    }
    else if ( c === "}" )
    {
      blocks.pop();
      statement = i + 1;
    }
    else if ( c === "\n" && blocks.length === 0 )
    {
      statement = i + 1;
    }
  }

  if ( stringStart >= 0 )
  {
    const typed = before.slice( stringStart + 1 );
    return /\binclude\s*$/.test( before.slice( 0, stringStart ) )
      ? { kind: "include", typed }
      : { kind: "path", typed };
  }

  const typed = /[A-Za-z_][\w.]*$/.exec( before )?.[0] ?? "";
  const words = before.slice( statement, before.length - typed.length ).match( /[A-Za-z_]\w*/g ) ?? [];
  const block = blocks[blocks.length - 1];
  if ( block === undefined )
  {
    // `entry PHASE` at the top level names the Phase a run starts in.
    return words.length === 0
      ? { kind: "word", typed }
      : words[words.length - 1] === "entry"
      ? { kind: "names", wanted: "phases", typed }
      : { kind: "none" };
  }
  if ( block === "resident" || block === "group" )
  {
    return { kind: "names", wanted: "modules", typed };
  }
  if ( block === "phase" )
  {
    // The clause a name stands in is the last word of the grammar before it.
    const clause = [ ...words ].reverse().find( ( word ) => NAMING.has( word ) || word === "entry" );
    if ( clause === "entry" && words[words.length - 1] === "entry" )
    {
      return { kind: "names", wanted: "labels", typed };
    }
    const wanted = clause === undefined ? undefined : NAMING.get( clause );
    return wanted === undefined ? { kind: "word", typed } : { kind: "names", wanted, typed };
  }
  return { kind: "word", typed };
}

// Whether the end of a line stands in a comment or in a string.
function inCommentOrString( line: string, comment: string ): boolean
{
  let quote = "";
  for ( let i = 0; i < line.length; ++i )
  {
    const c = line[i];
    if ( quote !== "" )
    {
      if ( c === "\\" )
      {
        ++i;
      }
      else if ( c === quote )
      {
        quote = "";
      }
    }
    else if ( c === '"' || c === "'" )
    {
      quote = c;
    }
    else if ( line.startsWith( comment, i ) )
    {
      return true;
    }
  }
  return quote !== "";
}
