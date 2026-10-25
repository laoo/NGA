import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";

// The text the reader holds that differs from its file, written where the tool
// can read it, and handed to every build as an overlay — see
// docs/decisions/0258-an-editor-reads-the-facts.md.
//
// Every buffer that differs is handed over, whichever Project it belongs to:
// the tool ignores an overlay its build never reads, and a buffer of one
// Project may be a file of the library another one reads too.
export class Buffers
{
  private mDirectory: string | undefined;
  private mWritten = new Map<string, string>();
  private mDiffering = new Map<string, string>();
  private mOnDisk: ( file: string ) => string | undefined;

  constructor( onDisk: ( file: string ) => string | undefined )
  {
    this.mOnDisk = onDisk;
  }

  // Records what a file holds now, and answers whether that changes what a
  // build would read: a buffer that differs from its file, or one that has
  // just stopped differing.
  hold( file: string, text: string ): boolean
  {
    const wasDiffering = this.mDiffering.has( file );
    if ( text === this.mOnDisk( file ) )
    {
      this.mDiffering.delete( file );
      return wasDiffering;
    }
    this.mDiffering.set( file, text );
    return true;
  }

  // A buffer saved or closed holds nothing the disk does not, and answers as
  // `hold` does.
  release( file: string ): boolean
  {
    return this.mDiffering.delete( file );
  }

  // Written now, so that a build reads what the buffers hold as it starts.
  overlays(): [ string, string ][]
  {
    const given: [ string, string ][] = [];
    for ( const [ file, text ] of this.mDiffering )
    {
      const written = this.fileFor( file );
      fs.writeFileSync( written, text );
      given.push( [ file, written ] );
    }
    return given;
  }

  // The overlay of one file, written now, or nothing where the buffer is what
  // the disk holds.
  overlayOf( file: string ): [ string, string ] | undefined
  {
    const text = this.mDiffering.get( file );
    if ( text === undefined )
    {
      return undefined;
    }
    const written = this.fileFor( file );
    fs.writeFileSync( written, text );
    return [ file, written ];
  }

  // A copy of text that is on no disk, for the tool to read where it reads only
  // files, named after the path it stands for so that its grammar is that
  // path's. One copy per `key`, written again each time.
  copyOf( key: string, name: string, text: string ): string
  {
    const written = this.fileFor( `${key}\n${name}`, path.basename( name ) );
    fs.writeFileSync( written, text );
    return written;
  }

  // What a file holds for the reader: the buffer where it differs, and the disk
  // where it does not.
  textOf( file: string ): string | undefined
  {
    return this.mDiffering.get( file ) ?? this.mOnDisk( file );
  }

  dispose(): void
  {
    if ( this.mDirectory !== undefined )
    {
      fs.rmSync( this.mDirectory, { recursive: true, force: true } );
    }
  }

  // One file per buffer, named after it so that a run repeated by hand from a
  // terminal reads plainly, and kept for as long as the server runs.
  private fileFor( key: string, name: string = path.basename( key ) ): string
  {
    let written = this.mWritten.get( key );
    if ( written === undefined )
    {
      this.mDirectory ??= fs.mkdtempSync( path.join( os.tmpdir(), "nga-buffers-" ) );
      written = path.join( this.mDirectory, `${String( this.mWritten.size )}-${name}` );
      this.mWritten.set( key, written );
    }
    return written;
  }
}
