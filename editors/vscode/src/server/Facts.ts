// The parts of what `--facts` writes that the server reads, and nothing else:
// a field nobody reads is not declared, so that the types say what the
// extension depends on. The contract is docs/spec/facts.md.

// One-based, in bytes, and the end exclusive; a point has no end. See
// docs/spec/diagnostics.md.
export interface Location
{
  file: string;
  line: number;
  column: number;
  endLine?: number;
  endColumn?: number;
}

// Where a generator that wrote the text read from: for a `.ngc`, the line of the
// C a statement of what it compiled to came from.
export interface SourceMark
{
  file: string;
  line: number;
}

// A finding, or a note of one, which has the same shape.
export interface Finding
{
  id: string;
  severity: "error" | "warning" | "note";
  message: string;
  location?: Location;
  source?: SourceMark;
  notes?: Finding[];
}

export interface ProjectFacts
{
  path: string;
  cpu?: string;
  library?: string;
  phases?: { name: string; entry?: boolean; then?: number[]; }[];
  modules?: { name: string; path: string; origin: string; }[];
  groups?: { name: string; }[];
  regions?: { name: string; begin: number; end: number; property: string; }[];
  unitSets?: UnitSetFacts[];
  windows?: WindowFacts[];
  panes?: { name: string; window: string; state?: number; count?: number; }[];
}

export interface UnitSetFacts
{
  name: string;
  count: number;
  linear?: [ number, number ];
}

// A state is named by its index; a unit set is `count` of them from `index`.
export interface WindowFacts
{
  name: string;
  ranges: [ number, number ][];
  states?: { index: number; units?: string; count?: number; name?: string; }[];
}

// `project` for the `.ngp` and what it includes, `source` for a Module's own
// file, `compiled` for what a `.ngc` Module became.
export type Role = "project" | "source" | "compiled";

export interface FileTokens
{
  module: string;
  path: string;
  role: Role;
}

// Where a name is written, in bytes of the file the path names.
export interface Site
{
  path: string;
  offset: number;
  length: number;
}

export interface SymbolFacts
{
  name: string;
  module: string;
  kind: string;
  exported?: boolean;
  section?: string;
  address?: number;
  value?: number;
  pane?: string;
  paneState?: number;
  units?: string;
  definition: Site;
  uses: Site[];
}

export interface MapSection
{
  name: string;
  module: string;
  kind: "proc" | "section" | "temporary";
  placement?: string;
  begin?: number;
  end?: number;
  initialised?: [ number, number ];
  firstPhase?: number | null;
  lastPhase?: number | null;
  root?: boolean;
  kept?: boolean;
  hidden?: boolean;
  pinned?: boolean;
  align?: number;
  within?: number;
  then?: string;
  in?: string[];
  shares?: string[];
  pane?: string;
  paneState?: number;
  units?: string;
  waits?: {
    as: string;
    liveFirst: number | null;
    liveLast: number | null;
    pieces: { unit: number; offset: number; size: number; }[];
  };
  text?: Site[];
}

export interface MapFacts
{
  unit?: string;
  unitSize?: number;
  pools?: { zeroPage: [ number, number ][]; general: [ number, number ][]; readOnly: [ number, number ][]; };
  phases?: { name: string; zeroPageUsed?: number; memoryUsed?: number; }[];
  sections: MapSection[];
  pruned?: MapSection[];
  frames?: {
    name: string;
    unit: number;
    offset: number;
    size: number;
    liveFirst: number | null;
    liveLast: number | null;
  }[];
  units?: { index: number; used: number; }[];
}

// A set is present only where it was asked for and the build got far enough to
// say it; `diagnostics` is always there.
export interface FactsDocument
{
  schema: number;
  nga: string;
  diagnostics: Finding[];
  project?: ProjectFacts;
  tokens?: FileTokens[];
  symbols?: SymbolFacts[];
  map?: MapFacts;
  sources?: WrittenFile[];
  bytes?: BytesFacts;
}

// A run of the Container's bytes: where it lands, what wrote it, and for a
// statement's own, the bytes as hex.
export interface ByteRun
{
  at?: number;
  address: number;
  length: number;
  what: string;
  section?: string;
  module?: string;
  source?: Site;
  data?: string;
}

export interface BytesFacts
{
  container: string;
  size?: number;
  header?: ByteRun[];
  segments?: { at?: number; start?: number; end?: number; runs: ByteRun[]; }[];
}

// The text of a file the tool wrote, under the path every other set names it
// by — see docs/spec/facts.md.
export interface WrittenFile
{
  module: string;
  path: string;
  role: Role;
  text: string;
}

// What `--vocabulary` writes: the words of the languages the tool reads.
export interface Vocabulary
{
  cpus: { name: string; mnemonics: string[]; }[];
  directives: string[];
  projectWords: string[];
}

// What `--classify` writes: one entry per file, its spans `[offset, length,
// class]` in bytes.
export interface Classification
{
  tokens: { path: string; spans: [ number, number, string ][]; }[];
}
