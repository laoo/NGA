// The language server: a Node process speaking the Language Server Protocol
// over standard input and output, which learns everything it says by running
// `nga` — see docs/decisions/0258-an-editor-reads-the-facts.md.

import * as fs from "node:fs";
import * as path from "node:path";
import { fileURLToPath } from "node:url";
import { TextDocument } from "vscode-languageserver-textdocument";
import {
  CodeLensRefreshRequest,
  CompletionItemKind,
  createConnection,
  LSPErrorCodes,
  MarkupKind,
  MessageType,
  ProposedFeatures,
  ResponseError,
  ShowMessageNotification,
  SymbolKind,
  TextDocuments,
  TextDocumentSyncKind
} from "vscode-languageserver/node";
import type {
  CodeLens,
  CompletionItem,
  DocumentSymbol,
  Hover,
  InlayHint,
  Location,
  Position,
  SymbolInformation,
  TextEdit,
  WorkspaceEdit
} from "vscode-languageserver/node";

import { linkKey, mapPageOf } from "../MapPage.ts";
import type { Link } from "../MapPage.ts";
import {
  BUILT,
  COMPILED_OF,
  CONTAINER_BYTES,
  LISTING,
  MAP_PAGE,
  PROJECT_OF,
  USE_PROJECT,
  WRITTEN_FILES,
  WRITTEN_TEXT
} from "../Protocol.ts";
import type {
  BuiltParams,
  CompiledOfParams,
  CompiledOfResult,
  ContainerBytesParams,
  ContainerBytesResult,
  InitializationOptions,
  ListedLine,
  ListingMode,
  ListingParams,
  MapPageParams,
  MapPageResult,
  ProjectOfParams,
  ProjectOfResult,
  RunNode,
  UseProjectParams,
  WrittenFilesParams,
  WrittenTextParams
} from "../Protocol.ts";
import { isWritten, writtenOf, writtenUri } from "../Written.ts";
import { Buffers } from "./Buffers.ts";
import { Builds } from "./Builds.ts";
import { namedIn, semanticTokensOf, TOKEN_MODIFIERS, TOKEN_TYPES } from "./Colours.ts";
import type { Named } from "./Colours.ts";
import { compiledLinesOf } from "./Compiled.ts";
import { assemblerPlace, projectPlace } from "./Completion.ts";
import type { ByteRun, FactsDocument, Site, SymbolFacts, Vocabulary } from "./Facts.ts";
import { uriOf } from "./Findings.ts";
import { labelOf, listingOf } from "./Listing.ts";
import { localsOf } from "./Locals.ts";
import type { Local, Locals } from "./Locals.ts";
import { sectionKindsOf, Sites, symbolKindOf } from "./Navigation.ts";
import { classify, findNga, runFacts, vocabularyOf } from "./Nga.ts";
import type { Run } from "./Nga.ts";
import { describe, entriesOf, sectionLine } from "./Placement.ts";
import { Projects, realPathOf } from "./Projects.ts";
import { changesOf, isName } from "./Rename.ts";
import { Text } from "./Text.ts";

const connection = createConnection( ProposedFeatures.all );
const documents = new TextDocuments( TextDocument );

let nga: string | undefined;
let configured = "";
let listing: ListingMode = "hover";

// How long typing has to pause before what the buffers hold is built.
const TYPING_PAUSE = 300;

function runOn( project: string, sets: string[], overlays: [ string, string ][] = [] ): Run
{
  if ( nga === undefined )
  {
    return { facts: Promise.reject( new Error( "no nga to run" ) ), cancel: () => undefined };
  }
  return runFacts( nga, project, sets, overlays );
}

const buffers = new Buffers( textOnDisk );

// Which files a `.ngp` reads is in `tokens`, whose every entry is a file of the
// build, and is read off the disk: a Project is found by what is saved. The
// findings are in the default sets, which are what an editor wants after every
// save — see docs/spec/facts.md — and are read off what the buffers hold.
let projects = new Projects( ( project ) => runOn( project, [ "project", "tokens" ] ), [] );
//
// `symbols` and `map` are what a name is coloured by: what it resolves to, and
// what kind of Section a Label stands in. `sources` is the text the tool wrote,
// which is shown as a document of its own, and `bytes` the listing.
const builds = new Builds(
  ( project ) =>
    runOn( project, [ "diagnostics", "project", "symbols", "map", "sources", "bytes" ], buffers.overlays() ),
  ( uri, diagnostics ) => void connection.sendDiagnostics( { uri, diagnostics } ),
  ( file ) => buffers.textOf( file ),
  ( project, error ) =>
    void connection.sendNotification( ShowMessageNotification.type, {
      type: MessageType.Error,
      message: `NGA: ${project} could not be built: ${String( error )}`
    } ),
  ( project ) =>
  {
    // What a name means may have changed with the build, and a client that can
    // be told so asks for the colours again; the text the tool wrote may have
    // changed with it too.
    if ( refreshesColours )
    {
      void connection.languages.semanticTokens.refresh();
    }
    if ( refreshesLenses )
    {
      void connection.sendRequest( CodeLensRefreshRequest.type );
    }
    if ( refreshesHints )
    {
      void connection.languages.inlayHint.refresh();
    }
    const built: BuiltParams = { project };
    void connection.sendNotification( BUILT, built );
  }
);

let refreshesColours = false;
let refreshesLenses = false;
let refreshesHints = false;

// What the last build of each Project says its names mean, read once per build.
const namedByFacts = new WeakMap<FactsDocument, Map<string, Named[]>>();

// `file` is the path the facts name it by: a file on disk as the file system
// holds it, and text the tool wrote as the facts write it.
function namedFor( project: string | undefined, file: string ): Named[]
{
  const facts = project === undefined ? undefined : builds.factsOf( project );
  if ( facts === undefined )
  {
    return [];
  }
  let named = namedByFacts.get( facts );
  if ( named === undefined )
  {
    // By the path the file system holds, as a Project is found by.
    named = new Map( [ ...namedIn( facts ) ].map( ( [ path, names ] ) => [ realPathOf( path ), names ] ) );
    namedByFacts.set( facts, named );
  }
  return named.get( file ) ?? [];
}

// The text the last build of a Project wrote at a path, if it wrote any.
function writtenText( project: string, file: string ): string | undefined
{
  return builds.factsOf( project )?.sources?.find( ( one ) => one.path === file )?.text;
}

// The text the build read: a file on disk, read when its findings are placed,
// and nothing for the tool's own `<nga>/…` and `<generated>/…`.
function textOnDisk( file: string ): string | undefined
{
  if ( file.startsWith( "<" ) )
  {
    return undefined;
  }
  try
  {
    return fs.readFileSync( file, "utf8" );
  }
  catch
  {
    return undefined;
  }
}

// Builds the Project of a file: on a save, always, and on an open, unless the
// Project has been built already since the server started.
async function buildOwnerOf( uri: string, always: boolean ): Promise<void>
{
  const project = await ownerOf( uri );
  if ( project !== undefined && ( always || !builds.isKnown( project ) ) )
  {
    clearTimeout( pausing.get( project ) );
    pausing.delete( project );
    await builds.build( project );
  }
}

// The builds waiting for typing to pause, by Project: a keystroke inside the
// pause starts it again rather than adding a build.
const pausing = new Map<string, NodeJS.Timeout>();

async function buildAfterPause( uri: string ): Promise<void>
{
  const project = await ownerOf( uri );
  if ( project === undefined )
  {
    return;
  }
  clearTimeout( pausing.get( project ) );
  pausing.set(
    project,
    setTimeout( () =>
    {
      pausing.delete( project );
      void builds.build( project );
    }, TYPING_PAUSE )
  );
}

async function ownerOf( uri: string ): Promise<string | undefined>
{
  if ( nga === undefined || !uri.startsWith( "file:" ) )
  {
    return undefined;
  }
  return ( await projects.ownerOf( fileURLToPath( uri ) ) ).project ?? undefined;
}

// Said once per `nga` that cannot be found, rather than on every file opened.
function locateNga(): void
{
  // The server runs as the CommonJS bundle esbuild makes of it, where
  // `__dirname` is the directory of the bundle, beside which a VSIX keeps `bin/`.
  nga = findNga( configured, __dirname );
  if ( nga === undefined )
  {
    // A notification and not `showErrorMessage`, which is a request: there is
    // nothing to answer, and a client that does not answer it would leave a
    // rejection nobody handles.
    void connection.sendNotification( ShowMessageNotification.type, {
      type: MessageType.Error,
      message: configured !== ""
        ? `NGA: ${configured} is not a file. Set nga.path to the nga to run.`
        : "NGA: no nga was found beside the extension or on PATH. Set nga.path to the nga to run."
    } );
  }
}

connection.onInitialize( ( params ) =>
{
  const options = params.initializationOptions as Partial<InitializationOptions> | undefined;
  configured = options?.ngaPath ?? "";
  listing = options?.listing ?? "hover";
  const folders = ( params.workspaceFolders ?? [] ).map( ( folder ) => fileURLToPath( folder.uri ) );
  workspaceFolders = folders.map( realPathOf );
  projects = new Projects( ( project ) => runOn( project, [ "project", "tokens" ] ), folders );
  refreshesColours = params.capabilities.workspace?.semanticTokens?.refreshSupport === true;
  refreshesLenses = params.capabilities.workspace?.codeLens?.refreshSupport === true;
  refreshesHints = params.capabilities.workspace?.inlayHint?.refreshSupport === true;
  locateNga();
  // `save` is what makes a client say a file was saved, which is when a build
  // runs; the kind of change alone would not.
  return {
    capabilities: {
      textDocumentSync: { openClose: true, change: TextDocumentSyncKind.Incremental, save: { includeText: false } },
      semanticTokensProvider: { legend: { tokenTypes: TOKEN_TYPES, tokenModifiers: TOKEN_MODIFIERS }, full: true },
      definitionProvider: true,
      renameProvider: { prepareProvider: true },
      completionProvider: { triggerCharacters: [ ".", '"', "@", "/" ] },
      hoverProvider: true,
      codeLensProvider: {},
      inlayHintProvider: true,
      referencesProvider: true,
      documentSymbolProvider: true,
      workspaceSymbolProvider: true
    }
  };
} );

// The client says the section again whenever the reader changes it.
connection.onDidChangeConfiguration( ( change ) =>
{
  const settings = ( change.settings as { nga?: { path?: string; listing?: ListingMode; }; } | undefined )?.nga;
  if ( settings?.listing !== undefined && settings.listing !== listing )
  {
    listing = settings.listing;
    if ( refreshesHints )
    {
      void connection.languages.inlayHint.refresh();
    }
  }
  if ( settings?.path !== undefined && settings.path !== configured )
  {
    configured = settings.path;
    locateNga();
    projects.forget();
  }
} );

// A `.ngp` that changed may name other files now, and a new one may be the
// nearest Project of files another one owned.
connection.onDidChangeWatchedFiles( ( change ) =>
{
  if ( change.changes.some( ( one ) => one.uri.endsWith( ".ngp" ) ) )
  {
    projects.forget();
  }
} );

documents.onDidOpen( ( opened ) =>
{
  void buildOwnerOf( opened.document.uri, false );
} );

// Opening a file says what it holds too, and a buffer an editor restored with
// unsaved text in it differs from the disk from the start.
documents.onDidChangeContent( ( changed ) =>
{
  const uri = changed.document.uri;
  if ( uri.startsWith( "file:" ) && buffers.hold( fileURLToPath( uri ), changed.document.getText() ) )
  {
    void buildAfterPause( uri );
  }
} );

documents.onDidSave( ( saved ) =>
{
  const uri = saved.document.uri;
  if ( uri.startsWith( "file:" ) )
  {
    buffers.release( fileURLToPath( uri ) );
  }
  if ( uri.endsWith( ".ngp" ) )
  {
    projects.forget();
  }
  void buildOwnerOf( uri, true );
} );

// A buffer closed without saving leaves the file as the disk has it.
documents.onDidClose( ( closed ) =>
{
  const uri = closed.document.uri;
  if ( uri.startsWith( "file:" ) && buffers.release( fileURLToPath( uri ) ) )
  {
    void buildAfterPause( uri );
  }
} );

connection.onExit( () => buffers.dispose() );

// The colours of a file as it stands: classified through its buffer where it
// holds one, so that they follow the typing at once, and its names coloured by
// what the last build said they mean. A name that build did not see keeps the
// colour of its class until the next one.
//
// Text the tool wrote is classified from a copy, since the tool classifies
// files, and its names are what the build that wrote it says they are.
connection.languages.semanticTokens.on( async ( params ) =>
{
  const uri = params.textDocument.uri;
  const written = writtenOf( uri );
  if ( nga === undefined || ( written === undefined && !uri.startsWith( "file:" ) ) )
  {
    return { data: [] };
  }
  try
  {
    if ( written !== undefined )
    {
      const text = documents.get( uri )?.getText() ?? writtenText( written.project, written.file ) ?? "";
      const copy = buffers.copyOf( written.project, written.file, text );
      const spans = ( await classify( nga, copy, undefined ) ).tokens[0]?.spans ?? [];
      return { data: semanticTokensOf( text, spans, namedFor( written.project, written.file ) ) };
    }
    const file = fileURLToPath( uri );
    const text = documents.get( uri )?.getText() ?? textOnDisk( file );
    if ( text === undefined )
    {
      return { data: [] };
    }
    const spans = ( await classify( nga, file, buffers.overlayOf( file ) ) ).tokens[0]?.spans ?? [];
    return { data: semanticTokensOf( text, spans, namedFor( await ownerOf( uri ), realPathOf( file ) ) ) };
  }
  catch
  {
    return { data: [] };
  }
} );

// Built first where the Project has not been: the reader asking what the tool
// wrote for it is asking for a build.
connection.onRequest( WRITTEN_FILES, async ( params: WrittenFilesParams ): Promise<string[]> =>
{
  if ( nga !== undefined && builds.factsOf( params.project ) === undefined )
  {
    await builds.build( params.project );
  }
  return ( builds.factsOf( params.project )?.sources ?? [] ).map( ( one ) => one.path );
} );

connection.onRequest( WRITTEN_TEXT, ( params: WrittenTextParams ): string =>
{
  const written = writtenOf( params.uri );
  return written === undefined ? "" : writtenText( written.project, written.file ) ?? "";
} );

connection.onRequest(
  PROJECT_OF,
  ( params: ProjectOfParams ): Promise<ProjectOfResult> => projects.ownerOf( fileURLToPath( params.uri ) )
);

connection.onNotification( USE_PROJECT, ( params: UseProjectParams ) =>
{
  projects.choose( params.candidates, params.project );
  if ( nga !== undefined && !builds.isKnown( params.project ) )
  {
    void builds.build( params.project );
  }
} );

// The document a request is about: its text, the Project whose last build says
// what its names are, and the path that build names it by — the file system's
// for a file on disk, the facts' own for text the tool wrote.
interface Subject
{
  uri: string;
  text: string;
  project: string | undefined;
  path: string;
}

async function subjectOf( uri: string ): Promise<Subject | undefined>
{
  const written = writtenOf( uri );
  if ( written !== undefined )
  {
    const text = documents.get( uri )?.getText() ?? writtenText( written.project, written.file ) ?? "";
    return { uri, text, project: written.project, path: written.file };
  }
  if ( !uri.startsWith( "file:" ) )
  {
    return undefined;
  }
  const file = fileURLToPath( uri );
  const text = documents.get( uri )?.getText() ?? textOnDisk( file );
  return text === undefined ? undefined : { uri, text, project: await ownerOf( uri ), path: realPathOf( file ) };
}

// The text of a file the facts name, in whichever form the reader sees it.
function textAt( project: string, file: string ): string
{
  return ( isWritten( file ) ? writtenText( project, file ) : buffers.textOf( file ) ) ?? "";
}

const sitesByFacts = new WeakMap<FactsDocument, Sites>();

function sitesOf( project: string | undefined ): Sites | undefined
{
  const facts = project === undefined ? undefined : builds.factsOf( project );
  if ( facts === undefined )
  {
    return undefined;
  }
  let sites = sitesByFacts.get( facts );
  if ( sites === undefined )
  {
    sites = new Sites( facts, ( file ) => isWritten( file ) ? file : realPathOf( file ) );
    sitesByFacts.set( facts, sites );
  }
  return sites;
}

// Where a site of the facts is, as the protocol names a place. Each file's
// text is measured once per request.
function locator( project: string ): ( site: Site ) => Location
{
  const texts = new Map<string, Text>();
  return ( site ) =>
  {
    let text = texts.get( site.path );
    if ( text === undefined )
    {
      text = new Text( textAt( project, site.path ) );
      texts.set( site.path, text );
    }
    return { uri: uriOf( project, site.path ), range: text.rangeOf( site.offset, site.length ) };
  };
}

// The local labels of a document, from its tokens as the tool classifies them.
async function localsIn( subject: Subject ): Promise<Locals>
{
  if ( nga === undefined )
  {
    return { references: new Map(), definitions: new Map() };
  }
  const written = writtenOf( subject.uri );
  const file = written === undefined
    ? fileURLToPath( subject.uri )
    : buffers.copyOf( written.project, written.file, subject.text );
  const overlay = written === undefined ? buffers.overlayOf( file ) : undefined;
  const spans = ( await classify( nga, file, overlay ) ).tokens[0]?.spans ?? [];
  return localsOf( subject.text, spans );
}

const covers = ( local: Local, offset: number ) => local.offset <= offset && offset <= local.offset + local.length;

connection.onDefinition( async ( params ): Promise<Location[]> =>
{
  const subject = await subjectOf( params.textDocument.uri );
  if ( subject === undefined )
  {
    return [];
  }
  const text = new Text( subject.text );
  const offset = text.offsetAt( params.position );
  const here = ( local: Local ): Location => ( {
    uri: subject.uri,
    range: text.rangeOf( local.offset, local.length )
  } );

  const locals = await localsIn( subject );
  for ( const one of locals.references.values() )
  {
    if ( covers( one.reference, offset ) )
    {
      return one.definitions.map( here );
    }
  }
  for ( const one of locals.definitions.values() )
  {
    if ( covers( one, offset ) )
    {
      return [ here( one ) ];
    }
  }

  const written = sitesOf( subject.project )?.at( subject.path, offset );
  if ( written === undefined || subject.project === undefined )
  {
    return [];
  }
  return [ locator( subject.project )( written.symbol.definition ) ];
} );

connection.onReferences( async ( params ): Promise<Location[]> =>
{
  const subject = await subjectOf( params.textDocument.uri );
  if ( subject === undefined )
  {
    return [];
  }
  const text = new Text( subject.text );
  const offset = text.offsetAt( params.position );
  const here = ( local: Local ): Location => ( {
    uri: subject.uri,
    range: text.rangeOf( local.offset, local.length )
  } );

  // A local label's references are the ones that resolve to it, and are all in
  // its own file.
  const locals = await localsIn( subject );
  const reached = [ ...locals.references.values() ].find( ( one ) => covers( one.reference, offset ) );
  const definitions = reached?.definitions
    ?? [ ...locals.definitions.values() ].filter( ( one ) => covers( one, offset ) );
  if ( definitions.length > 0 )
  {
    const uses = [ ...locals.references.values() ]
      .filter( ( one ) =>
        one.definitions.some( ( definition ) => definitions.some( ( d ) => d.offset === definition.offset ) )
      )
      .map( ( one ) => here( one.reference ) );
    return [ ...( params.context.includeDeclaration ? definitions.map( here ) : [] ), ...uses ];
  }

  const written = sitesOf( subject.project )?.at( subject.path, offset );
  if ( written === undefined || subject.project === undefined )
  {
    return [];
  }
  const locate = locator( subject.project );
  const symbol = written.symbol;
  return [ ...( params.context.includeDeclaration ? [ symbol.definition ] : [] ), ...symbol.uses ].map( locate );
} );

// The outline of a file: every Symbol it defines, a Proc holding the Labels
// written inside it, whose names its own name prefixes.
connection.onDocumentSymbol( async ( params ): Promise<DocumentSymbol[]> =>
{
  const subject = await subjectOf( params.textDocument.uri );
  const facts = subject?.project === undefined ? undefined : builds.factsOf( subject.project );
  const sites = sitesOf( subject?.project );
  if ( subject === undefined || facts === undefined || sites === undefined )
  {
    return [];
  }
  const text = new Text( subject.text );
  const kinds = sectionKindsOf( facts );
  const outline: DocumentSymbol[] = [];
  const procs: { name: string; symbol: DocumentSymbol; }[] = [];
  for ( const symbol of sites.definedIn( subject.path ) )
  {
    const kind = symbolKindOf(
      symbol,
      symbol.section === undefined ? undefined : kinds.get( `${symbol.module}\n${symbol.section}` )
    );
    const range = text.rangeOf( symbol.definition.offset, symbol.definition.length );
    const parent = procs.find( ( one ) => symbol.name.startsWith( one.name + "." ) );
    const shown: DocumentSymbol = {
      name: parent === undefined ? symbol.name : symbol.name.slice( parent.name.length + 1 ),
      detail: symbol.kind,
      kind,
      range,
      selectionRange: range
    };
    if ( parent === undefined )
    {
      outline.push( shown );
    }
    else
    {
      ( parent.symbol.children ??= [] ).push( shown );
    }
    if ( kind === SymbolKind.Function )
    {
      procs.push( { name: symbol.name, symbol: shown } );
    }
  }
  return outline;
} );

// Every name of every Project built so far whose name holds what was typed.
const MOST_NAMES_FOUND = 500;

connection.onWorkspaceSymbol( ( params ): SymbolInformation[] =>
{
  const wanted = params.query.toLowerCase();
  const found: SymbolInformation[] = [];
  const seen = new Set<string>();
  for ( const [ project, facts ] of builds.everyBuild() )
  {
    const kinds = sectionKindsOf( facts );
    const locate = locator( project );
    for ( const symbol of facts.symbols ?? [] )
    {
      const key = `${uriOf( project, symbol.definition.path )}\n${String( symbol.definition.offset )}`;
      if ( !symbol.name.toLowerCase().includes( wanted ) || symbol.definition.path === "" || seen.has( key ) )
      {
        continue;
      }
      seen.add( key );
      found.push( {
        name: symbol.name,
        kind: symbolKindOf(
          symbol,
          symbol.section === undefined ? undefined : kinds.get( `${symbol.module}\n${symbol.section}` )
        ),
        location: locate( symbol.definition ),
        containerName: symbol.module
      } );
      if ( found.length >= MOST_NAMES_FOUND )
      {
        return found;
      }
    }
  }
  return found;
} );

// The workspace's folders as the file system holds them: a rename changes
// nothing outside them, the shipped library above all.
let workspaceFolders: string[] = [];

function inWorkspace( file: string ): boolean
{
  const real = realPathOf( file );
  return workspaceFolders.some( ( folder ) => real === folder || real.startsWith( folder + path.sep ) );
}

// What a rename at a position renames: a local label of the file, or a Symbol
// of the last build of its Project, and the site the cursor stands on.
type Renamed =
  | { kind: "local"; subject: Subject; definition: Local; references: Local[]; }
  | { kind: "symbol"; subject: Subject; project: string; symbol: SymbolFacts; facts: FactsDocument; site: Site; };

async function renamedAt( uri: string, position: Position ): Promise<Renamed | undefined>
{
  const subject = await subjectOf( uri );
  if ( subject === undefined )
  {
    return undefined;
  }
  const text = new Text( subject.text );
  const offset = text.offsetAt( position );
  const locals = await localsIn( subject );
  const reached = [ ...locals.references.values() ].find( ( one ) => covers( one.reference, offset ) );
  const definition = reached?.definitions.length === 1
    ? reached.definitions[0]
    : [ ...locals.definitions.values() ].find( ( one ) => covers( one, offset ) );
  if ( reached !== undefined || definition !== undefined )
  {
    if ( definition === undefined )
    {
      return undefined;
    }
    const references = [ ...locals.references.values() ]
      .filter( ( one ) => one.definitions.some( ( d ) => d.offset === definition.offset ) )
      .map( ( one ) => one.reference );
    return { kind: "local", subject, definition, references };
  }

  const project = subject.project;
  const facts = project === undefined ? undefined : builds.factsOf( project );
  const written = sitesOf( project )?.at( subject.path, offset );
  if ( project === undefined || facts === undefined || written === undefined )
  {
    return undefined;
  }
  return { kind: "symbol", subject, project, symbol: written.symbol, facts, site: written.site };
}

function refused( message: string ): ResponseError<void>
{
  return new ResponseError( LSPErrorCodes.RequestFailed, `NGA: ${message}` );
}

// The written text of a site, from the file as the reader holds it. A name is
// ASCII, so its bytes and its characters are one count.
function writtenAt( project: string ): ( site: Site ) => string
{
  return ( site ) =>
    Buffer.from( textAt( project, site.path ), "utf8" ).subarray( site.offset, site.offset + site.length ).toString(
      "utf8"
    );
}

// Why a Symbol cannot be renamed, or nothing where it can: every place it is
// written must be a file of the workspace on disk.
function whyNot( renamed: Extract<Renamed, { kind: "symbol"; }> ): string | undefined
{
  for ( const site of [ renamed.symbol.definition, ...renamed.symbol.uses ] )
  {
    if ( site.path === "" || isWritten( site.path ) )
    {
      return `\`${renamed.symbol.name}\` is written in text the tool wrote${
        site.path === "" ? "" : `, ${site.path}`
      }, which a rename cannot change.`;
    }
    if ( !inWorkspace( site.path ) )
    {
      return `\`${renamed.symbol.name}\` is written in ${site.path}, outside the workspace.`;
    }
  }
  return undefined;
}

// Built first, so that what is renamed is where the screen shows it: what the
// last build said was said of the text it read.
connection.onPrepareRename( async ( params ) =>
{
  const before = await subjectOf( params.textDocument.uri );
  if ( before?.project !== undefined && nga !== undefined )
  {
    clearTimeout( pausing.get( before.project ) );
    pausing.delete( before.project );
    await builds.build( before.project );
  }
  const renamed = await renamedAt( params.textDocument.uri, params.position );
  if ( renamed === undefined )
  {
    return null;
  }
  const text = new Text( renamed.subject.text );
  if ( renamed.kind === "local" )
  {
    const name = Buffer.from( renamed.subject.text, "utf8" )
      .subarray( renamed.definition.offset, renamed.definition.offset + renamed.definition.length )
      .toString( "utf8" )
      .slice( 1 );
    if ( name === "" )
    {
      throw refused( "an anonymous `@` has no name to change." );
    }
    return { range: text.rangeOf( renamed.definition.offset + 1, name.length ), placeholder: name };
  }
  const why = whyNot( renamed );
  if ( why !== undefined )
  {
    throw refused( why );
  }
  const written = writtenAt( renamed.project )( renamed.site );
  const last = written.lastIndexOf( "." ) + 1;
  return {
    range: text.rangeOf( renamed.site.offset + last, written.length - last ),
    placeholder: written.slice( last )
  };
} );

connection.onRenameRequest( async ( params ): Promise<WorkspaceEdit | null> =>
{
  const renamed = await renamedAt( params.textDocument.uri, params.position );
  if ( renamed === undefined )
  {
    return null;
  }
  const name = params.newName.startsWith( "@" ) ? params.newName.slice( 1 ) : params.newName;
  if ( !isName( name ) )
  {
    throw refused( `\`${params.newName}\` is not a name: a letter or \`_\`, then letters, digits and \`_\`.` );
  }

  if ( renamed.kind === "local" )
  {
    // `@name`, `@+name` and `@-name` alike: the name follows the `@` and its
    // direction. Refused where the file already has a label of that name,
    // which a reference by direction might reach instead.
    const bytes = Buffer.from( renamed.subject.text, "utf8" );
    const locals = await localsIn( renamed.subject );
    const taken = [ ...locals.definitions.values() ].some( ( one ) =>
      bytes.subarray( one.offset, one.offset + one.length ).toString( "utf8" ) === `@${name}`
    );
    if ( taken )
    {
      throw refused( `\`@${name}\` is a label of this file already.` );
    }
    const text = new Text( renamed.subject.text );
    const edits = [ renamed.definition, ...renamed.references ].map( ( one ) =>
    {
      const written = bytes.subarray( one.offset, one.offset + one.length ).toString( "utf8" );
      const start = written[1] === "+" || written[1] === "-" ? 2 : 1;
      return { range: text.rangeOf( one.offset + start, one.length - start ), newText: name };
    } );
    return { changes: { [renamed.subject.uri]: edits } };
  }

  const why = whyNot( renamed );
  if ( why !== undefined )
  {
    throw refused( why );
  }
  const symbol = renamed.symbol;
  const qualified = symbol.name.includes( "." )
    ? `${symbol.name.slice( 0, symbol.name.lastIndexOf( "." ) )}.${name}`
    : name;
  if ( ( renamed.facts.symbols ?? [] ).some( ( one ) => one.module === symbol.module && one.name === qualified ) )
  {
    throw refused( `\`${qualified}\` already names a Symbol of ${symbol.module}.` );
  }
  const inner = ( renamed.facts.symbols ?? [] ).filter( ( one ) =>
    one.module === symbol.module && one.name.startsWith( symbol.name + "." )
  );
  for ( const label of inner )
  {
    const why = whyNot( { ...renamed, symbol: label } );
    if ( why !== undefined )
    {
      throw refused( why );
    }
  }

  const changes: { [uri: string]: TextEdit[]; } = {};
  const texts = new Map<string, Text>();
  for ( const change of changesOf( symbol, inner, name, writtenAt( renamed.project ) ) )
  {
    let text = texts.get( change.path );
    if ( text === undefined )
    {
      text = new Text( textAt( renamed.project, change.path ) );
      texts.set( change.path, text );
    }
    const uri = uriOf( renamed.project, change.path );
    ( changes[uri] ??= [] ).push( { range: text.rangeOf( change.offset, change.length ), newText: change.text } );
  }
  return { changes };
} );

// The tool's words, asked once of each `nga`.
const vocabularies = new Map<string, Promise<Vocabulary>>();

function vocabulary(): Promise<Vocabulary | undefined>
{
  if ( nga === undefined )
  {
    return Promise.resolve( undefined );
  }
  let asked = vocabularies.get( nga );
  if ( asked === undefined )
  {
    asked = vocabularyOf( nga );
    vocabularies.set( nga, asked );
  }
  return asked.catch( () => undefined );
}

const COMPLETION_KINDS = new Map<SymbolKind, CompletionItemKind>( [
  [ SymbolKind.Function, CompletionItemKind.Function ],
  [ SymbolKind.Variable, CompletionItemKind.Variable ],
  [ SymbolKind.Field, CompletionItemKind.Field ],
  [ SymbolKind.Constant, CompletionItemKind.Constant ],
  [ SymbolKind.Property, CompletionItemKind.Property ],
  [ SymbolKind.Method, CompletionItemKind.Method ],
  [ SymbolKind.Interface, CompletionItemKind.Interface ],
  [ SymbolKind.Struct, CompletionItemKind.Struct ]
] );

// The most files a path is offered from, and how deep a directory is looked
// into for them: a Project's tree, not the disk.
const MOST_FILES = 500;
const DEEPEST = 6;

// Every file under a directory with one of the extensions, by its path
// relative to the directory, `/` between its parts as a document writes it.
function filesUnder( root: string, extensions: string[] ): string[]
{
  const found: string[] = [];
  const visit = ( directory: string, depth: number ) =>
  {
    let entries: fs.Dirent[];
    try
    {
      entries = fs.readdirSync( directory, { withFileTypes: true } );
    }
    catch
    {
      return;
    }
    for ( const entry of entries )
    {
      if ( found.length >= MOST_FILES || entry.name.startsWith( "." ) || entry.name === "node_modules" )
      {
        continue;
      }
      const full = path.join( directory, entry.name );
      if ( entry.isDirectory() && depth < DEEPEST )
      {
        visit( full, depth + 1 );
      }
      else if ( entry.isFile() && extensions.some( ( extension ) => entry.name.endsWith( extension ) ) )
      {
        found.push( path.relative( root, full ).split( path.sep ).join( "/" ) );
      }
    }
  };
  visit( root, 0 );
  return found.sort();
}

// What may complete the word the cursor stands at the end of: the tool's
// words, and the names the last build of the Project knows.
connection.onCompletion( async ( params ): Promise<CompletionItem[]> =>
{
  const subject = await subjectOf( params.textDocument.uri );
  if ( subject === undefined || writtenOf( subject.uri ) !== undefined )
  {
    return [];
  }
  const text = new Text( subject.text );
  const offset = text.offsetAt( params.position );
  const before = Buffer.from( subject.text, "utf8" ).subarray( 0, offset ).toString( "utf8" );
  const line = before.slice( before.lastIndexOf( "\n" ) + 1 );
  const facts = subject.project === undefined ? undefined : builds.factsOf( subject.project );
  const words = await vocabulary();

  // Each item replaces what is typed of its word.
  const replacing = ( typed: string ) => ( {
    start: { line: params.position.line, character: params.position.character - typed.length },
    end: params.position
  } );
  const item = ( label: string, kind: CompletionItemKind, typed: string, detail?: string ): CompletionItem => ( {
    label,
    kind,
    textEdit: { range: replacing( typed ), newText: label },
    ...( detail === undefined ? {} : { detail } )
  } );

  if ( subject.uri.endsWith( ".ngp" ) )
  {
    const place = projectPlace( before );
    const projectDirectory = path.dirname( subject.project ?? fileURLToPath( subject.uri ) );
    switch ( place.kind )
    {
      case "include":
      {
        const library = facts?.project?.library;
        const shipped = library === undefined ? [] : filesUnder( library, [ ".ngp" ] );
        const own = filesUnder( projectDirectory, [ ".ngp" ] );
        return [ ...own, ...shipped ].map( ( one ) => item( one, CompletionItemKind.File, place.typed ) );
      }
      case "path":
        return filesUnder( projectDirectory, [ ".asm", ".ngc" ] ).map( ( one ) =>
          item( one, CompletionItemKind.File, place.typed )
        );
      case "names":
      {
        const project = facts?.project;
        if ( place.wanted === "phases" )
        {
          return ( project?.phases ?? [] ).map( ( one ) => item( one.name, CompletionItemKind.Event, place.typed ) );
        }
        if ( place.wanted === "labels" )
        {
          return ( facts?.symbols ?? [] )
            .filter( ( one ) => one.kind === "label" && one.name === one.section )
            .map( ( one ) => item( one.name, CompletionItemKind.Function, place.typed, one.module ) );
        }
        return [
          ...( project?.modules ?? [] )
            .filter( ( one ) => one.origin !== "tool" )
            .map( ( one ) => item( one.name, CompletionItemKind.Module, place.typed, "module" ) ),
          ...( project?.groups ?? [] ).map( ( one ) =>
            item( one.name, CompletionItemKind.Folder, place.typed, "group" )
          )
        ];
      }
      case "word":
        return ( words?.projectWords ?? [] ).map( ( one ) => item( one, CompletionItemKind.Keyword, place.typed ) );
      default:
        return [];
    }
  }

  if ( subject.uri.endsWith( ".ngc" ) )
  {
    return [];
  }
  const place = assemblerPlace( line );
  switch ( place.kind )
  {
    case "directive":
      return ( words?.directives ?? [] ).map( ( one ) =>
        item( one, CompletionItemKind.Keyword, place.typed, "directive" )
      );
    case "statement":
    {
      const cpu = facts?.project?.cpu ?? "6502";
      const mnemonics = words?.cpus.find( ( one ) => one.name === cpu )?.mnemonics ?? [];
      const macros = ( facts?.symbols ?? [] ).filter( ( one ) => one.kind === "macro" );
      return [
        ...mnemonics.map( ( one ) => item( one, CompletionItemKind.Keyword, place.typed, cpu ) ),
        ...macros.map( ( one ) => item( one.name, CompletionItemKind.Method, place.typed, "macro" ) )
      ];
    }
    case "operand":
    {
      if ( place.typed.startsWith( "@" ) )
      {
        // A local label of this file, written with the direction typed.
        const direction = /^@[+-]?/.exec( place.typed )?.[0] ?? "@";
        const bytes = Buffer.from( subject.text, "utf8" );
        const names = new Set(
          [ ...( await localsIn( subject ) ).definitions.values() ]
            .map( ( one ) => bytes.subarray( one.offset + 1, one.offset + one.length ).toString( "utf8" ) )
            .filter( ( name ) => name !== "" )
        );
        return [ ...names ].map( ( name ) => item( direction + name, CompletionItemKind.Reference, place.typed ) );
      }
      // Its own Module's names, and what any other exports.
      const symbols = facts?.symbols ?? [];
      const own = new Set(
        symbols.filter( ( one ) => realPathOf( one.definition.path ) === subject.path ).map( ( one ) => one.module )
      );
      const kinds = facts === undefined ? new Map<string, string>() : sectionKindsOf( facts );
      return symbols
        .filter( ( one ) => one.kind !== "macro" && ( own.has( one.module ) || one.exported === true ) )
        .map( ( one ) =>
        {
          const kind = symbolKindOf(
            one,
            one.section === undefined ? undefined : kinds.get( `${one.module}\n${one.section}` )
          );
          const detail = one.address !== undefined
            ? `$${one.address.toString( 16 ).toUpperCase().padStart( 4, "0" )}`
            : one.value !== undefined
            ? `= ${String( one.value )}`
            : one.kind;
          return item( one.name, COMPLETION_KINDS.get( kind ) ?? CompletionItemKind.Text, place.typed, detail );
        } );
    }
    default:
      return [];
  }
} );

// Where the name under the cursor stands, as the last build placed it.
connection.onHover( async ( params ): Promise<Hover | null> =>
{
  const subject = await subjectOf( params.textDocument.uri );
  const facts = subject?.project === undefined ? undefined : builds.factsOf( subject.project );
  if ( subject === undefined || facts === undefined )
  {
    return null;
  }
  const text = new Text( subject.text );
  const written = sitesOf( subject.project )?.at( subject.path, text.offsetAt( params.position ) );
  if ( written === undefined )
  {
    return null;
  }
  return {
    contents: { kind: MarkupKind.Markdown, value: describe( written.symbol, facts ) },
    range: text.rangeOf( written.site.offset, written.site.length )
  };
} );

// Above every Section a file opens, the line a hover on its name would give
// first: where it stands, how long it is, and the Phases that hold it.
connection.onCodeLens( async ( params ): Promise<CodeLens[]> =>
{
  const subject = await subjectOf( params.textDocument.uri );
  const facts = subject?.project === undefined ? undefined : builds.factsOf( subject.project );
  const sites = sitesOf( subject?.project );
  if ( subject === undefined || facts === undefined || sites === undefined )
  {
    return [];
  }
  const text = new Text( subject.text );
  return sites.definedIn( subject.path )
    .filter( ( symbol ) => symbol.kind === "label" && symbol.section === symbol.name )
    .map( ( symbol ) =>
    {
      const { entries, pruned } = entriesOf( facts, symbol.module, symbol.name );
      const line = text.positionAt( symbol.definition.offset ).line;
      return {
        range: { start: { line, character: 0 }, end: { line, character: 0 } },
        command: { title: sectionLine( entries, pruned, facts ), command: "" }
      };
    } );
} );

// The listing in the margin: at the end of each statement's first line, where
// it landed and what it wrote.
//
// Shown as inlay hints only where the reader asked for it beside every
// statement: by default the client shows one line of it, beside the statement
// the pointer rests on, from the same lines.
async function listedLines( uri: string ): Promise<ListedLine[]>
{
  const subject = await subjectOf( uri );
  const facts = subject?.project === undefined ? undefined : builds.factsOf( subject.project );
  if ( subject === undefined || facts === undefined )
  {
    return [];
  }
  const text = new Text( subject.text );
  return listingOf( facts, subject.path, ( file ) => isWritten( file ) ? file : realPathOf( file ) ).map( (
    listed
  ) => ( {
    line: text.positionAt( listed.offset ).line,
    label: labelOf( listed )
  } ) );
}

connection.languages.inlayHint.on( async ( params ): Promise<InlayHint[]> =>
{
  if ( listing !== "always" )
  {
    return [];
  }
  const lines = documents.get( params.textDocument.uri )?.getText().split( /\r\n|\r|\n/ ) ?? [];
  return ( await listedLines( params.textDocument.uri ) )
    .filter( ( { line } ) => line >= params.range.start.line && line <= params.range.end.line )
    .map( ( { line, label } ) => ( {
      position: { line, character: lines[line]?.length ?? 0 },
      label,
      paddingLeft: true
    } ) );
} );

connection.onRequest( LISTING, ( params: ListingParams ): Promise<ListedLine[]> => listedLines( params.uri ) );

// What a `.ngc` compiled to, and which of its lines came from which line of
// the C. The Module is the one whose path, read beside the Project, is the
// file; its text is the `<generated>/NAME.asm` the facts name it by.
connection.onRequest( COMPILED_OF, async ( params: CompiledOfParams ): Promise<CompiledOfResult | null> =>
{
  const subject = await subjectOf( params.uri );
  const project = subject?.project;
  const facts = project === undefined ? undefined : builds.factsOf( project );
  if ( subject === undefined || project === undefined || facts === undefined )
  {
    return null;
  }
  const module = ( facts.project?.modules ?? [] ).find( ( one ) =>
    one.origin === "compiled" && realPathOf( path.resolve( path.dirname( project ), one.path ) ) === subject.path
  );
  const file = module === undefined ? undefined : `<generated>/${module.name}.asm`;
  const text = file === undefined ? undefined : writtenText( project, file );
  if ( file === undefined || text === undefined )
  {
    return null;
  }
  return { uri: writtenUri( project, file ), lines: compiledLinesOf( text ) };
} );

// The memory map of a file's Project, every Section of it a link to where its
// name is written — see docs/spec/memory-map.md.
connection.onRequest( MAP_PAGE, async ( params: MapPageParams ): Promise<MapPageResult | null> =>
{
  const subject = await subjectOf( params.uri );
  const project = subject?.project ?? writtenOf( params.uri )?.project;
  const facts = project === undefined ? undefined : builds.factsOf( project );
  if ( project === undefined || facts === undefined )
  {
    return null;
  }
  const locate = locator( project );
  const links = new Map<string, Link>();
  for ( const symbol of facts.symbols ?? [] )
  {
    if ( symbol.kind === "label" && symbol.section === symbol.name && symbol.definition.path !== "" )
    {
      const where = locate( symbol.definition );
      links.set( linkKey( symbol.module, symbol.name ), {
        uri: where.uri,
        line: where.range.start.line,
        character: where.range.start.character
      } );
    }
  }
  return { project, html: mapPageOf( facts, { links, nonce: params.nonce } ) };
} );

// The Container the last build wrote, as the tree beside the source shows it:
// every run with the place of the statement that wrote it, where one did.
connection.onRequest( CONTAINER_BYTES, async ( params: ContainerBytesParams ): Promise<ContainerBytesResult | null> =>
{
  const subject = await subjectOf( params.uri );
  const project = subject?.project ?? writtenOf( params.uri )?.project;
  const bytes = project === undefined ? undefined : builds.factsOf( project )?.bytes;
  if ( project === undefined || bytes === undefined || ( bytes.segments ?? [] ).length === 0 )
  {
    return null;
  }
  const locate = locator( project );
  const node = ( run: ByteRun ): RunNode => ( {
    at: run.at ?? 0,
    length: run.length,
    address: run.address,
    what: run.what,
    ...( run.section === undefined ? {} : { section: run.section } ),
    ...( run.module === undefined ? {} : { module: run.module } ),
    ...( run.data === undefined ? {} : { data: run.data } ),
    ...( run.source === undefined || run.source.path === "" ? {} : { place: locate( run.source ) } )
  } );
  return {
    project,
    container: bytes.container,
    size: bytes.size ?? 0,
    header: ( bytes.header ?? [] ).map( node ),
    parts: ( bytes.segments ?? [] ).map( ( part ) => ( {
      at: part.at ?? 0,
      start: part.start ?? 0,
      end: part.end ?? 0,
      runs: part.runs.map( node )
    } ) )
  };
} );

documents.listen( connection );
connection.listen();
