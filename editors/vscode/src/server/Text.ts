import type { Position, Range } from "vscode-languageserver/node";

// A file's text with its positions in both of the measures in play: the tool
// counts bytes, the protocol counts UTF-16 code units, and the two part ways at
// the first character outside ASCII. Lines end at `\n`, `\r\n` and a lone `\r`,
// as both count them.
export class Text
{
  private mBytes: Buffer;
  private mLineStarts: number[];

  constructor( text: string )
  {
    this.mBytes = Buffer.from( text, "utf8" );
    this.mLineStarts = [ 0 ];
    for ( let i = 0; i < this.mBytes.length; ++i )
    {
      if ( this.mBytes[i] === 0x0a || ( this.mBytes[i] === 0x0d && this.mBytes[i + 1] !== 0x0a ) )
      {
        this.mLineStarts.push( i + 1 );
      }
    }
  }

  positionAt( offset: number ): Position
  {
    let low = 0;
    let high = this.mLineStarts.length - 1;
    while ( low < high )
    {
      const middle = ( low + high + 1 ) >> 1;
      if ( ( this.mLineStarts[middle] ?? 0 ) <= offset )
      {
        low = middle;
      }
      else
      {
        high = middle - 1;
      }
    }
    const start = this.mLineStarts[low] ?? 0;
    return { line: low, character: this.mBytes.subarray( start, offset ).toString( "utf8" ).length };
  }

  rangeOf( offset: number, length: number ): Range
  {
    return { start: this.positionAt( offset ), end: this.positionAt( offset + length ) };
  }

  // The byte a position stands at: its line's start, and as many bytes as the
  // characters before it on that line take.
  offsetAt( position: Position ): number
  {
    const start = this.mLineStarts[position.line] ?? this.mBytes.length;
    const end = this.mLineStarts[position.line + 1] ?? this.mBytes.length;
    const line = this.mBytes.subarray( start, end ).toString( "utf8" );
    return start + Buffer.byteLength( line.slice( 0, position.character ), "utf8" );
  }
}
