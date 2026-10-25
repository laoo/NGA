// What the client and the server say to each other beyond the Language Server
// Protocol. The server is reachable from any editor that starts one; these are
// what only this client asks, and a client that never asks loses nothing else.

// Which Project builds a file, and which others could have. `project` is the one
// the server builds the file with; `candidates` are every Project of the
// nearest directory that names the file and that no other one includes, and
// hold `project` whenever it is not null.
export const PROJECT_OF = "nga/projectOf";

export interface ProjectOfParams
{
  uri: string;
}

export interface ProjectOfResult
{
  project: string | null;
  candidates: string[];
}

// The reader chose one of a file's candidates. The choice holds for every file
// with the same candidates, which is what makes it the choice of a directory
// rather than of one file, and the client says it again to a server it starts.
export const USE_PROJECT = "nga/useProject";

export interface UseProjectParams
{
  candidates: string[];
  project: string;
}

// The paths of the text the last build of a Project wrote rather than read,
// which the client offers to open — see Written.ts.
export const WRITTEN_FILES = "nga/writtenFiles";

export interface WrittenFilesParams
{
  project: string;
}

// The text at a written document's URI, as the last build of its Project wrote
// it, and empty where that build wrote nothing there.
export const WRITTEN_TEXT = "nga/writtenText";

export interface WrittenTextParams
{
  uri: string;
}

// A Project was built: a written document of it may read differently now.
export const BUILT = "nga/built";

export interface BuiltParams
{
  project: string;
}

// What a `.ngc` compiled to: the written document of it, and which of its
// lines came from which line of the C. Nothing for a file that is no `.ngc` of
// a Project built so far.
export const COMPILED_OF = "nga/compiledOf";

export interface CompiledOfParams
{
  uri: string;
}

// Lines of the compiled text, `from` up to `to`, that line `source` of the C
// became; every number counts from zero.
export interface CompiledLines
{
  source: number;
  from: number;
  to: number;
}

export interface CompiledOfResult
{
  uri: string;
  lines: CompiledLines[];
}

// The memory map of the Project of a file, as a page for a webview whose
// script runs under `nonce`; nothing where no build of it has finished.
export const MAP_PAGE = "nga/mapPage";

export interface MapPageParams
{
  uri: string;
  nonce: string;
}

export interface MapPageResult
{
  project: string;
  html: string;
}

// The Container the last build of a file's Project wrote, read back: its
// header, and its parts — the segments of a `.xex`, the banks and the fixed
// part of a cartridge, the parts of a diskette — each a list of runs, a run
// with the place of the statement that wrote it. Nothing where the facts read
// back no such Container.
export const CONTAINER_BYTES = "nga/containerBytes";

export interface ContainerBytesParams
{
  uri: string;
}

export interface RunPlace
{
  uri: string;
  range: { start: { line: number; character: number; }; end: { line: number; character: number; }; };
}

export interface RunNode
{
  at: number;
  length: number;
  address: number;
  what: string;
  section?: string;
  module?: string;
  data?: string;
  place?: RunPlace;
}

export interface PartNode
{
  at: number;
  start: number;
  end: number;
  runs: RunNode[];
}

export interface ContainerBytesResult
{
  project: string;
  container: string;
  size: number;
  header: RunNode[];
  parts: PartNode[];
}

// What the client hands the server as it starts.
export interface InitializationOptions
{
  ngaPath: string;

  // How the listing is shown: beside the statement the pointer rests on,
  // beside every statement as an inlay hint, or not at all.
  listing?: ListingMode;
}

export type ListingMode = "hover" | "always" | "off";

// The listing of a document: for each line a statement wrote bytes from, what
// the margin shows there — see src/server/Listing.ts.
export const LISTING = "nga/listing";

export interface ListingParams
{
  uri: string;
}

export interface ListedLine
{
  line: number;
  label: string;
}
