// The part of the extension that is Visual Studio Code's own: it starts the
// language server, gives an `.asm` the NGA language where a Project names it,
// and says in the status bar which Project builds the file in front of the
// reader. Everything it knows it asks the server.

import { randomBytes } from "node:crypto";
import * as path from "node:path";
import * as vscode from "vscode";
import { LanguageClient, TransportKind } from "vscode-languageclient/node";

import { containerLabel, partLabel, runLabel } from "../ByteTree.ts";
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
  CompiledLines,
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
  PartNode,
  ProjectOfParams,
  ProjectOfResult,
  RunNode,
  RunPlace,
  UseProjectParams,
  WrittenFilesParams,
  WrittenTextParams
} from "../Protocol.ts";
import { WRITTEN_SCHEME, writtenOf, writtenUri } from "../Written.ts";

const LANGUAGES = [ "nga-project", "nga-c", "nga-asm" ];

// Where the reader's choices among a file's candidates are kept between two
// sessions, and handed to every server this client starts.
const CHOICES_KEY = "nga.choices";

let client: LanguageClient | undefined;
let status: vscode.StatusBarItem | undefined;
let shown: ProjectOfResult | undefined;

export async function activate( context: vscode.ExtensionContext ): Promise<void>
{
  const server = context.asAbsolutePath( path.join( "dist", "Server.cjs" ) );
  const options: InitializationOptions = {
    ngaPath: vscode.workspace.getConfiguration( "nga" ).get<string>( "path" ) ?? "",
    listing: listingMode()
  };
  client = new LanguageClient(
    "nga",
    "NGA",
    {
      run: { module: server, transport: TransportKind.stdio },
      debug: { module: server, transport: TransportKind.stdio }
    },
    {
      documentSelector: LANGUAGES.map( ( language ) => ( { language } ) ),
      initializationOptions: options,
      synchronize: {
        configurationSection: "nga",
        fileEvents: vscode.workspace.createFileSystemWatcher( "**/*.ngp" )
      }
    }
  );
  await client.start();

  const choices = context.workspaceState.get<UseProjectParams[]>( CHOICES_KEY ) ?? [];
  for ( const choice of choices )
  {
    await client.sendNotification( USE_PROJECT, choice );
  }

  // The text the tool wrote, read-only, as the last build of its Project wrote
  // it, and read again when that Project is built.
  const changed = new vscode.EventEmitter<vscode.Uri>();
  const written: vscode.TextDocumentContentProvider = {
    onDidChange: changed.event,
    provideTextDocumentContent: ( uri ) =>
    {
      const params: WrittenTextParams = { uri: uri.toString() };
      return client?.sendRequest<string>( WRITTEN_TEXT, params ) ?? "";
    }
  };
  client.onNotification( BUILT, ( params: BuiltParams ) =>
  {
    forgetListing();
    void refreshCompiled();
    if ( shownBytes?.project === params.project )
    {
      void loadBytes( true );
    }
    void refreshMap( params.project );
    for ( const document of vscode.workspace.textDocuments )
    {
      if ( writtenOf( document.uri.toString() )?.project === params.project )
      {
        changed.fire( document.uri );
      }
    }
  } );

  status = vscode.window.createStatusBarItem( vscode.StatusBarAlignment.Left );
  status.command = "nga.chooseProject";
  context.subscriptions.push(
    status,
    changed,
    vscode.workspace.registerTextDocumentContentProvider( WRITTEN_SCHEME, written ),
    vscode.commands.registerCommand( "nga.chooseProject", () => chooseProject( context ) ),
    vscode.commands.registerCommand( "nga.openWritten", () => openWritten() ),
    vscode.commands.registerCommand( "nga.showCompiled", () => showCompiled() ),
    vscode.commands.registerCommand( "nga.showMap", () => showMap() ),
    vscode.commands.registerCommand( "nga.openRun", ( place: RunPlace ) => openRun( place ) ),
    listingLook,
    vscode.languages.registerHoverProvider( LANGUAGES.map( ( language ) => ( { language } ) ), {
      provideHover: ( document, position ) =>
      {
        void showListing( document, position.line );
        return undefined;
      }
    } ),
    vscode.window.onDidChangeTextEditorSelection( ( change ) => change.textEditor.setDecorations( listingLook, [] ) ),
    vscode.workspace.onDidChangeTextDocument( ( change ) => forgetListing( change.document.uri.toString() ) ),
    vscode.workspace.onDidChangeConfiguration( ( change ) =>
    {
      if ( change.affectsConfiguration( "nga.listing" ) )
      {
        forgetListing();
      }
    } ),
    bytesView,
    changedBytes,
    vscode.window.onDidChangeActiveTextEditor( () => void loadBytes() ),
    vscode.window.onDidChangeTextEditorSelection( ( change ) => void selectRun( change.textEditor ) ),
    vscode.window.onDidChangeTextEditorSelection( ( change ) => lightCorresponding( change.textEditor ) ),
    lit,
    vscode.workspace.onDidOpenTextDocument( ( document ) => void claim( document ) ),
    vscode.window.onDidChangeActiveTextEditor( () => void showProject() )
  );
  for ( const document of vscode.workspace.textDocuments )
  {
    await claim( document );
  }
  await showProject();
  await vscode.commands.executeCommand( "setContext", "nga.active", true );
  await loadBytes();
}

export async function deactivate(): Promise<void>
{
  await client?.stop();
}

async function projectOf( document: vscode.TextDocument ): Promise<ProjectOfResult | undefined>
{
  if ( client === undefined || document.uri.scheme !== "file" )
  {
    return undefined;
  }
  const params: ProjectOfParams = { uri: document.uri.toString() };
  return client.sendRequest<ProjectOfResult>( PROJECT_OF, params );
}

// Every assembler writes `.asm`, so an `.asm` is NGA's only where a Project
// names it, and anything else is left to whatever else claims it. Text the tool
// wrote is assembler whatever its name says.
async function claim( document: vscode.TextDocument ): Promise<void>
{
  if ( document.uri.scheme === WRITTEN_SCHEME && document.languageId !== "nga-asm" )
  {
    await vscode.languages.setTextDocumentLanguage( document, "nga-asm" );
    return;
  }
  if ( document.languageId === "nga-asm" || !document.fileName.endsWith( ".asm" ) )
  {
    return;
  }
  const owner = await projectOf( document );
  if ( owner?.project !== null && owner?.project !== undefined )
  {
    await vscode.languages.setTextDocumentLanguage( document, "nga-asm" );
  }
}

async function showProject(): Promise<void>
{
  const document = vscode.window.activeTextEditor?.document;
  if ( status === undefined || document === undefined || !LANGUAGES.includes( document.languageId ) )
  {
    status?.hide();
    shown = undefined;
    return;
  }
  shown = await projectOf( document );
  if ( shown === undefined || shown.project === null )
  {
    status.text = "$(warning) NGA: no Project";
    status.tooltip = "No .ngp above this file names it.";
  }
  else
  {
    const others = shown.candidates.length > 1 ? ` (1 of ${String( shown.candidates.length )})` : "";
    status.text = `$(project) NGA: ${path.basename( shown.project )}${others}`;
    status.tooltip = shown.project;
  }
  status.show();
}

async function chooseProject( context: vscode.ExtensionContext ): Promise<void>
{
  if ( client === undefined || shown === undefined || shown.candidates.length === 0 )
  {
    return;
  }
  const picked = await vscode.window.showQuickPick(
    shown.candidates.map( ( candidate ) => ( { label: path.basename( candidate ), description: candidate } ) ),
    { placeHolder: "The Project that builds this file" }
  );
  if ( picked === undefined )
  {
    return;
  }
  const choice: UseProjectParams = { candidates: shown.candidates, project: picked.description };
  await client.sendNotification( USE_PROJECT, choice );
  const choices = ( context.workspaceState.get<UseProjectParams[]>( CHOICES_KEY ) ?? [] ).filter(
    ( one ) => one.candidates.join( "\n" ) !== choice.candidates.join( "\n" )
  );
  await context.workspaceState.update( CHOICES_KEY, [ ...choices, choice ] );
  await showProject();
}

// The text the tool wrote for the Project of the file in front of the reader:
// the runtime, a Generator's Module, what a `.ngc` compiled to.
async function openWritten(): Promise<void>
{
  const document = vscode.window.activeTextEditor?.document;
  const owner = document === undefined ? undefined : await projectOf( document );
  const project = owner?.project ?? writtenOf( document?.uri.toString() ?? "" )?.project ?? null;
  if ( client === undefined || project === null )
  {
    void vscode.window.showInformationMessage( "NGA: open a file of a Project first." );
    return;
  }
  const params: WrittenFilesParams = { project };
  const files = await client.sendRequest<string[]>( WRITTEN_FILES, params );
  const picked = await vscode.window.showQuickPick( files, {
    placeHolder: "The text the tool wrote for this Project"
  } );
  if ( picked !== undefined )
  {
    await vscode.window.showTextDocument( vscode.Uri.parse( writtenUri( project, picked ) ) );
  }
}

// A `.ngc` shown beside what it compiled to, and which lines of one came from
// which line of the other — read again after every build.
interface Shown
{
  source: string;
  compiled: string;
  lines: CompiledLines[];
}

const shownPairs: Shown[] = [];

const lit = vscode.window.createTextEditorDecorationType( {
  backgroundColor: new vscode.ThemeColor( "editor.rangeHighlightBackground" ),
  isWholeLine: true
} );

async function showCompiled(): Promise<void>
{
  const editor = vscode.window.activeTextEditor;
  if ( client === undefined || editor === undefined || editor.document.languageId !== "nga-c" )
  {
    return;
  }
  const params: CompiledOfParams = { uri: editor.document.uri.toString() };
  const found = await client.sendRequest<CompiledOfResult | null>( COMPILED_OF, params );
  if ( found === null )
  {
    void vscode.window.showInformationMessage( "NGA: the last build compiled no Module from this file." );
    return;
  }
  const source = editor.document.uri.toString();
  const known = shownPairs.find( ( one ) => one.source === source );
  if ( known === undefined )
  {
    shownPairs.push( { source, compiled: found.uri, lines: found.lines } );
  }
  else
  {
    known.compiled = found.uri;
    known.lines = found.lines;
  }
  await vscode.window.showTextDocument( vscode.Uri.parse( found.uri ), {
    viewColumn: vscode.ViewColumn.Beside,
    preserveFocus: true
  } );
  lightCorresponding( editor );
}

async function refreshCompiled(): Promise<void>
{
  for ( const pair of shownPairs )
  {
    const params: CompiledOfParams = { uri: pair.source };
    const found = await client?.sendRequest<CompiledOfResult | null>( COMPILED_OF, params );
    if ( found !== null && found !== undefined )
    {
      pair.compiled = found.uri;
      pair.lines = found.lines;
    }
  }
}

// The cursor in the C lights what its line became; the cursor in what it
// became lights the line of the C it came from.
function lightCorresponding( editor: vscode.TextEditor ): void
{
  const here = editor.document.uri.toString();
  const line = editor.selection.active.line;
  for ( const pair of shownPairs )
  {
    const other = ( uri: string ) =>
      vscode.window.visibleTextEditors.find( ( one ) => one.document.uri.toString() === uri );
    if ( here === pair.source )
    {
      const target = other( pair.compiled );
      const ranges = pair.lines
        .filter( ( one ) => one.source === line )
        .map( ( one ) => new vscode.Range( one.from, 0, Math.max( one.from, one.to - 1 ), 0 ) );
      target?.setDecorations( lit, ranges );
      const first = ranges[0];
      if ( target !== undefined && first !== undefined )
      {
        target.revealRange( first, vscode.TextEditorRevealType.InCenterIfOutsideViewport );
      }
    }
    else if ( here === pair.compiled )
    {
      const run = pair.lines.find( ( one ) => one.from <= line && line < one.to );
      other( pair.source )?.setDecorations(
        lit,
        run === undefined ? [] : [ new vscode.Range( run.source, 0, run.source, 0 ) ]
      );
    }
  }
}

// The memory map of a Project, one page for each, beside the code and drawn
// again after every build of it.
const maps = new Map<string, { panel: vscode.WebviewPanel; uri: string; }>();

async function mapPage( uri: string ): Promise<MapPageResult | null>
{
  const params: MapPageParams = { uri, nonce: randomBytes( 16 ).toString( "base64" ) };
  return ( await client?.sendRequest<MapPageResult | null>( MAP_PAGE, params ) ) ?? null;
}

async function showMap(): Promise<void>
{
  const uri = vscode.window.activeTextEditor?.document.uri.toString();
  const page = uri === undefined ? null : await mapPage( uri );
  if ( uri === undefined || page === null )
  {
    void vscode.window.showInformationMessage( "NGA: no build of this file's Project has finished yet." );
    return;
  }
  const shown = maps.get( page.project );
  if ( shown !== undefined )
  {
    shown.panel.webview.html = page.html;
    shown.panel.reveal( vscode.ViewColumn.Beside, true );
    return;
  }
  const panel = vscode.window.createWebviewPanel(
    "ngaMap",
    `Memory map: ${path.basename( page.project )}`,
    { viewColumn: vscode.ViewColumn.Beside, preserveFocus: true },
    { enableScripts: true, retainContextWhenHidden: true }
  );
  panel.webview.html = page.html;
  // A ⌘ or Ctrl click on a Section asks for where its name is written.
  panel.webview.onDidReceiveMessage( async ( message: { open?: { uri: string; line: number; character: number; }; } ) =>
  {
    if ( message.open === undefined )
    {
      return;
    }
    const at = new vscode.Position( message.open.line, message.open.character );
    await vscode.window.showTextDocument( vscode.Uri.parse( message.open.uri ), {
      viewColumn: vscode.ViewColumn.One,
      selection: new vscode.Range( at, at )
    } );
  } );
  panel.onDidDispose( () => maps.delete( page.project ) );
  maps.set( page.project, { panel, uri } );
}

async function refreshMap( project: string ): Promise<void>
{
  const shown = maps.get( project );
  const page = shown === undefined ? null : await mapPage( shown.uri );
  if ( shown !== undefined && page !== null )
  {
    shown.panel.webview.html = page.html;
  }
}

// The Container the last build of the Project in front of the reader wrote, as
// a tree of its parts and their runs: a run opens the statement that wrote
// it, and the cursor in a statement selects its run where the tree is shown.
type ByteNode =
  | { kind: "container"; bytes: ContainerBytesResult; }
  | { kind: "part"; part: PartNode; parent: ByteNode; }
  | { kind: "run"; run: RunNode; parent: ByteNode; };

let shownBytes: ContainerBytesResult | undefined;
let byteRoots: ByteNode[] = [];
const byteChildren = new Map<ByteNode, ByteNode[]>();
const changedBytes = new vscode.EventEmitter<undefined>();

const bytesView = vscode.window.createTreeView<ByteNode>( "nga.containerBytes", {
  treeDataProvider: {
    onDidChangeTreeData: changedBytes.event,
    getChildren: ( node?: ByteNode ) => node === undefined ? byteRoots : byteChildren.get( node ) ?? [],
    getParent: ( node: ByteNode ) => node.kind === "container" ? undefined : node.parent,
    getTreeItem: ( node: ByteNode ) => treeItemOf( node )
  }
} );

function treeItemOf( node: ByteNode ): vscode.TreeItem
{
  switch ( node.kind )
  {
    case "container":
      return new vscode.TreeItem( containerLabel( node.bytes ), vscode.TreeItemCollapsibleState.Expanded );
    case "part":
      return new vscode.TreeItem(
        partLabel( node.part, shownBytes?.container ?? "" ),
        vscode.TreeItemCollapsibleState.Collapsed
      );
    case "run":
    {
      const item = new vscode.TreeItem( runLabel( node.run ), vscode.TreeItemCollapsibleState.None );
      if ( node.run.place !== undefined )
      {
        item.command = { title: "Open", command: "nga.openRun", arguments: [ node.run.place ] };
      }
      return item;
    }
  }
}

// Built again for the Project of the active editor when it changes, and
// always when the Project was built.
async function loadBytes( always = false ): Promise<void>
{
  const document = vscode.window.activeTextEditor?.document;
  if ( client === undefined || document === undefined || !LANGUAGES.includes( document.languageId ) )
  {
    return;
  }
  const owner = await projectOf( document );
  if ( !always && owner?.project !== undefined && owner.project === shownBytes?.project )
  {
    return;
  }
  const params: ContainerBytesParams = { uri: document.uri.toString() };
  const bytes = await client.sendRequest<ContainerBytesResult | null>( CONTAINER_BYTES, params );
  shownBytes = bytes ?? undefined;
  byteChildren.clear();
  byteRoots = [];
  if ( bytes !== null )
  {
    const root: ByteNode = { kind: "container", bytes };
    const runs = ( parent: ByteNode, list: RunNode[] ): ByteNode[] =>
      list.map( ( run ) => ( { kind: "run", run, parent } ) );
    const parts: ByteNode[] = [ ...runs( root, bytes.header ) ];
    for ( const part of bytes.parts )
    {
      const node: ByteNode = { kind: "part", part, parent: root };
      byteChildren.set( node, runs( node, part.runs ) );
      parts.push( node );
    }
    byteChildren.set( root, parts );
    byteRoots = [ root ];
  }
  changedBytes.fire( undefined );
}

async function openRun( place: RunPlace ): Promise<void>
{
  const range = new vscode.Range(
    place.range.start.line,
    place.range.start.character,
    place.range.end.line,
    place.range.end.character
  );
  await vscode.window.showTextDocument( vscode.Uri.parse( place.uri ), { selection: range, preserveFocus: false } );
}

// The run of the statement under the cursor, selected in the tree where the
// tree is shown and left alone where it is not, so that typing never opens it.
async function selectRun( editor: vscode.TextEditor ): Promise<void>
{
  if ( !bytesView.visible )
  {
    return;
  }
  const uri = editor.document.uri.toString();
  const at = editor.selection.active;
  for ( const [ , nodes ] of byteChildren )
  {
    for ( const node of nodes )
    {
      if ( node.kind !== "run" || node.run.place?.uri !== uri )
      {
        continue;
      }
      const { start, end } = node.run.place.range;
      const range = new vscode.Range( start.line, start.character, end.line, end.character );
      if ( range.contains( at ) )
      {
        await bytesView.reveal( node, { select: true, focus: false, expand: true } );
        return;
      }
    }
  }
}

// The listing beside the statement the pointer rests on: faded, and in one
// column for the whole file, a few characters past the longest line that has
// a listing, so that the bytes read as a column of their own rather than as
// more of the instruction. It goes when the cursor moves or the text changes.
const LISTING_GAP = 4;
const LISTING_COLUMN_MOST = 72;

const listingLook = vscode.window.createTextEditorDecorationType( {
  after: { color: new vscode.ThemeColor( "editorCodeLens.foreground" ) }
} );

const listings = new Map<string, Promise<ListedLine[]>>();

function listingMode(): ListingMode
{
  return vscode.workspace.getConfiguration( "nga" ).get<ListingMode>( "listing" ) ?? "hover";
}

// Asked again after a build, and after the text changes: a line number of an
// older text names a different statement.
function forgetListing( uri?: string ): void
{
  if ( uri === undefined )
  {
    listings.clear();
  }
  else
  {
    listings.delete( uri );
  }
  for ( const editor of vscode.window.visibleTextEditors )
  {
    if ( uri === undefined || editor.document.uri.toString() === uri )
    {
      editor.setDecorations( listingLook, [] );
    }
  }
}

async function showListing( document: vscode.TextDocument, line: number ): Promise<void>
{
  const editor = vscode.window.visibleTextEditors.find( ( one ) => one.document === document );
  if ( client === undefined || editor === undefined || listingMode() !== "hover" )
  {
    return;
  }
  const uri = document.uri.toString();
  let asked = listings.get( uri );
  if ( asked === undefined )
  {
    const params: ListingParams = { uri };
    asked = client.sendRequest<ListedLine[]>( LISTING, params );
    listings.set( uri, asked );
  }
  const lines = await asked;
  const here = lines.find( ( one ) => one.line === line );
  if ( here === undefined || line >= document.lineCount )
  {
    editor.setDecorations( listingLook, [] );
    return;
  }
  const widest = Math.max(
    ...lines.filter( ( one ) => one.line < document.lineCount ).map( ( one ) =>
      document.lineAt( one.line ).text.length
    )
  );
  const column = Math.min( LISTING_COLUMN_MOST, widest + LISTING_GAP );
  const end = document.lineAt( line ).range.end;
  const margin = Math.max( LISTING_GAP, column - end.character );
  editor.setDecorations( listingLook, [ {
    range: new vscode.Range( end, end ),
    renderOptions: { after: { contentText: here.label, margin: `0 0 0 ${String( margin )}ch` } }
  } ] );
}
