import * as path from "node:path";
import { pathToFileURL } from "node:url";
import { DiagnosticSeverity, DiagnosticTag } from "vscode-languageserver/node";
import type { Diagnostic, DiagnosticRelatedInformation, Range } from "vscode-languageserver/node";

import { isWritten, writtenUri } from "../Written.ts";
import type { FactsDocument, Finding, Location } from "./Facts.ts";
import { Text } from "./Text.ts";

// Where each finding of the site's catalogue is described: the chapter on
// diagnostics, which gives every identifier an anchor of its own.
const CATALOGUE = "https://laoo.github.io/NGA/diagnostics/";

// The text of a file on disk as the build read it: the buffer where the reader
// holds one, the disk where not, and nothing for a file that is neither.
export type TextOf = ( file: string ) => string | undefined;

// Where a file the facts name is shown: its own URI for a file on disk, and a
// written document of the Project for text the tool wrote.
export function uriOf( project: string, file: string ): string
{
  return isWritten( file ) ? writtenUri( project, file ) : pathToFileURL( file ).toString();
}

// The findings of one build of `project`, by the URI each is shown at.
//
// A finding is shown where it is located. Where that is what a `.ngc` compiled
// to, it is shown in the `.ngc` on the line its `.source` names instead, as the
// suite matches it — see docs/spec/testing.md — and where it is other text the
// tool wrote, in the written document of that text, which `sources` holds. A
// finding with no location at all, such as a Phase that does not fit, is the
// Project's, and goes on its first line, as does one whose text is nowhere to
// be had, with where it was in its message. A note that can be shown is
// related information, and any other note is a line of the message.
//
// What Prune dropped is shown too, faded as an editor fades what nothing
// reaches: a hint, which no list of problems counts, on each piece of the
// dropped Section's own text, saying so where the reader rests on it.
export function findingsOf( facts: FactsDocument, project: string, textOf: TextOf ): Map<string, Diagnostic[]>
{
  const written = new Map( ( facts.sources ?? [] ).map( ( one ) => [ one.path, one.text ] ) );
  const positions = new Positions( ( file ) => isWritten( file ) ? written.get( file ) : textOf( file ) );
  const shown = new Map<string, Diagnostic[]>();
  const show = ( file: string, diagnostic: Diagnostic ) =>
  {
    const uri = uriOf( project, file );
    const list = shown.get( uri );
    if ( list === undefined )
    {
      shown.set( uri, [ diagnostic ] );
    }
    else
    {
      list.push( diagnostic );
    }
  };

  for ( const finding of facts.diagnostics )
  {
    const related: DiagnosticRelatedInformation[] = [];
    const lines = [ finding.message ];
    for ( const note of finding.notes ?? [] )
    {
      const range = note.location === undefined ? undefined : positions.rangeOf( note.location );
      if ( note.location !== undefined && range !== undefined )
      {
        related.push( { location: { uri: uriOf( project, note.location.file ), range }, message: note.message } );
      }
      else
      {
        lines.push( note.location === undefined ? note.message : `${note.message} (${placeOf( note.location )})` );
      }
    }

    let file = project;
    let range: Range = { start: { line: 0, character: 0 }, end: { line: 0, character: 0 } };
    const located = finding.location === undefined ? undefined : positions.rangeOf( finding.location );
    const marked = finding.source === undefined ? undefined : resolved( finding.source.file, project );
    if ( finding.location !== undefined && located !== undefined && !isWritten( finding.location.file ) )
    {
      file = finding.location.file;
      range = located;
    }
    else if ( finding.source !== undefined && marked !== undefined && positions.canShow( marked ) )
    {
      file = marked;
      range = positions.wholeLine( marked, finding.source.line );
    }
    else if ( finding.location !== undefined && located !== undefined )
    {
      file = finding.location.file;
      range = located;
    }
    else if ( finding.location !== undefined )
    {
      lines[0] = `${finding.message} (${placeOf( finding.location )})`;
    }

    const diagnostic: Diagnostic = {
      range,
      severity: severityOf( finding.severity ),
      code: finding.id,
      codeDescription: { href: CATALOGUE + "#" + finding.id.toLowerCase() },
      source: "nga",
      message: lines.join( "\n" )
    };
    if ( related.length > 0 )
    {
      diagnostic.relatedInformation = related;
    }
    show( file, diagnostic );
  }

  const texts = new Map<string, Text | undefined>();
  for ( const dropped of facts.map?.pruned ?? [] )
  {
    for ( const piece of dropped.text ?? [] )
    {
      if ( !texts.has( piece.path ) )
      {
        const text = isWritten( piece.path ) ? written.get( piece.path ) : textOf( piece.path );
        texts.set( piece.path, text === undefined ? undefined : new Text( text ) );
      }
      const text = texts.get( piece.path );
      if ( text !== undefined )
      {
        show( piece.path, {
          range: text.rangeOf( piece.offset, piece.length ),
          severity: DiagnosticSeverity.Hint,
          tags: [ DiagnosticTag.Unnecessary ],
          source: "nga",
          message: `\`${dropped.name}\` is dropped by Prune: nothing reaches it, so the build holds none of it`
        } );
      }
    }
  }
  return shown;
}

function severityOf( severity: Finding["severity"] ): DiagnosticSeverity
{
  switch ( severity )
  {
    case "error":
      return DiagnosticSeverity.Error;
    case "warning":
      return DiagnosticSeverity.Warning;
    case "note":
      return DiagnosticSeverity.Information;
  }
}

function placeOf( location: Location ): string
{
  return `${location.file}:${String( location.line )}:${String( location.column )}`;
}

// A `.source` mark names a file as the generator wrote it, which for a `.ngc`
// is the path the tool was given; a relative one is the Project's.
function resolved( file: string, project: string ): string
{
  return path.isAbsolute( file ) ? file : path.join( path.dirname( project ), file );
}

// Positions the protocol can use, from the tool's: lines are the same, but a
// column is in bytes there and in UTF-16 code units here, which part ways at
// the first em dash of a comment. Each file's text is read once per build.
class Positions
{
  private mLines = new Map<string, string[] | undefined>();
  private mTextOf: TextOf;

  constructor( textOf: TextOf )
  {
    this.mTextOf = textOf;
  }

  // Whether the file is one whose lines can be shown.
  canShow( file: string ): boolean
  {
    return this.linesOf( file ) !== undefined;
  }

  rangeOf( location: Location ): Range | undefined
  {
    const lines = this.linesOf( location.file );
    if ( lines === undefined )
    {
      return undefined;
    }
    const start = { line: location.line - 1, character: characterOf( lines[location.line - 1], location.column ) };
    if ( location.endLine === undefined || location.endColumn === undefined )
    {
      return { start, end: start };
    }
    return {
      start,
      end: { line: location.endLine - 1, character: characterOf( lines[location.endLine - 1], location.endColumn ) }
    };
  }

  wholeLine( file: string, line: number ): Range
  {
    const text = this.linesOf( file )?.[line - 1] ?? "";
    return { start: { line: line - 1, character: 0 }, end: { line: line - 1, character: text.length } };
  }

  private linesOf( file: string ): string[] | undefined
  {
    if ( !this.mLines.has( file ) )
    {
      // The tool takes `\n`, `\r\n` and a lone `\r` each for one line ending,
      // as the protocol does.
      this.mLines.set( file, this.mTextOf( file )?.split( /\r\n|\r|\n/ ) );
    }
    return this.mLines.get( file );
  }
}

// The UTF-16 character a one-based byte column of a line stands at. A column
// past the end of the line is its end.
export function characterOf( line: string | undefined, column: number ): number
{
  if ( line === undefined )
  {
    return 0;
  }
  return Buffer.from( line, "utf8" ).subarray( 0, Math.max( 0, column - 1 ) ).toString( "utf8" ).length;
}
