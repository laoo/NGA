// The text the tool wrote rather than read — the runtime, a Generator's Module,
// what a `.ngc` compiled to — exists on no disk, and is shown as a read-only
// document under a scheme of the extension's own. Its URI carries the path the
// facts name the text by, `<nga>/runtime.asm` or `<generated>/NAME.asm`, and
// the Project whose build wrote it, since two Projects write two different
// texts under one such path.

export const WRITTEN_SCHEME = "nga-text";

// Whether a path the facts name is text the tool wrote: one on no disk is
// named in angle brackets, which no file's path begins with.
export function isWritten( file: string ): boolean
{
  return file.startsWith( "<" );
}

export function writtenUri( project: string, file: string ): string
{
  const encoded = file.split( "/" ).map( encodeURIComponent ).join( "/" );
  return `${WRITTEN_SCHEME}:/${encoded}?${encodeURIComponent( project )}`;
}

// The Project and the path a written document's URI carries, however an
// editor has re-encoded it on the way back.
export function writtenOf( uri: string ): { project: string; file: string; } | undefined
{
  if ( !uri.startsWith( WRITTEN_SCHEME + ":" ) )
  {
    return undefined;
  }
  const rest = uri.slice( WRITTEN_SCHEME.length + 1 );
  const query = rest.indexOf( "?" );
  if ( query < 0 )
  {
    return undefined;
  }
  return {
    file: decodeURIComponent( rest.slice( 0, query ).replace( /^\/+/, "" ) ),
    project: decodeURIComponent( rest.slice( query + 1 ) )
  };
}
