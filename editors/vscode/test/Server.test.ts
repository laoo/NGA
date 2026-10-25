// The server, started as an editor starts it and spoken to over the protocol,
// against a real `nga`: no Visual Studio Code in the loop, which is what makes
// these cheap enough for CI. `NGA_BINARY` names the `nga`; the default is the
// debug build of this tree.

import * as assert from "node:assert/strict";
import { spawn } from "node:child_process";
import type { ChildProcess } from "node:child_process";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
// Called through the namespace: dprint lays out a bare `describe` and `it` in a
// style of its own, with no space inside the parentheses.
import * as spec from "node:test";
import { pathToFileURL } from "node:url";
import { createMessageConnection, StreamMessageReader, StreamMessageWriter } from "vscode-jsonrpc/node";

import type { MessageConnection } from "vscode-jsonrpc/node";
import { writtenUri } from "../src/Written.ts";

const HERE = import.meta.dirname;
const ROOT = path.resolve( HERE, "..", "..", ".." );
const SERVER = path.resolve( HERE, "..", "dist", "Server.cjs" );
const NGA = process.env["NGA_BINARY"] ?? path.join( ROOT, "build", "debug", "src", "nga_cli", "nga" );

interface ProjectOfResult
{
  project: string | null;
  candidates: string[];
}

interface Diagnostic
{
  range: { start: { line: number; character: number; }; end: { line: number; character: number; }; };
  code: string;
  relatedInformation?: { location: { uri: string; }; }[];
}

interface Initialized
{
  capabilities: { semanticTokensProvider?: { legend: { tokenTypes: string[]; }; }; };
}

interface Server
{
  connection: MessageConnection;
  child: ChildProcess;
  messages: string[];
  published: Map<string, Diagnostic[]>;
  refreshes: number;
  legend: string[];
  waiting: Array<() => void>;
}

async function start( folder: string, capabilities: object = {} ): Promise<Server>
{
  const child = spawn( process.execPath, [ SERVER, "--stdio" ], { stdio: [ "pipe", "pipe", "inherit" ] } );
  if ( child.stdout === null || child.stdin === null )
  {
    throw new Error( "the server has no standard streams" );
  }
  const connection = createMessageConnection(
    new StreamMessageReader( child.stdout ),
    new StreamMessageWriter( child.stdin )
  );
  const server: Server = {
    connection,
    child,
    messages: [],
    published: new Map(),
    refreshes: 0,
    legend: [],
    waiting: []
  };
  const wakeAll = () =>
  {
    for ( const wake of server.waiting.splice( 0 ) )
    {
      wake();
    }
  };
  connection.onNotification( "window/showMessage", ( params: { message: string; } ) =>
  {
    server.messages.push( params.message );
  } );
  connection.onNotification(
    "textDocument/publishDiagnostics",
    ( params: { uri: string; diagnostics: Diagnostic[]; } ) =>
    {
      server.published.set( params.uri, params.diagnostics );
      wakeAll();
    }
  );
  connection.onRequest( "workspace/semanticTokens/refresh", () =>
  {
    ++server.refreshes;
    wakeAll();
    return null;
  } );
  connection.listen();
  const initialized = await connection.sendRequest<Initialized>( "initialize", {
    processId: process.pid,
    rootUri: pathToFileURL( folder ).toString(),
    workspaceFolders: [ { uri: pathToFileURL( folder ).toString(), name: path.basename( folder ) } ],
    capabilities,
    initializationOptions: { ngaPath: NGA }
  } );
  server.legend = initialized.capabilities.semanticTokensProvider?.legend.tokenTypes ?? [];
  await connection.sendNotification( "initialized", {} );
  return server;
}

// Once the server has asked for colours to be fetched again `count` times,
// which it does after every build.
async function refreshed( server: Server, count: number ): Promise<void>
{
  while ( server.refreshes < count )
  {
    await new Promise<void>( ( resolve ) => server.waiting.push( resolve ) );
  }
}

// What the server last published for a file, once it satisfies `wanted`.
async function publishedFor(
  server: Server,
  file: string,
  wanted: ( diagnostics: Diagnostic[] ) => boolean
): Promise<Diagnostic[]>
{
  const uri = pathToFileURL( file ).toString();
  for ( ;; )
  {
    const diagnostics = server.published.get( uri );
    if ( diagnostics !== undefined && wanted( diagnostics ) )
    {
      return diagnostics;
    }
    await new Promise<void>( ( resolve ) => server.waiting.push( resolve ) );
  }
}

async function stop( server: Server ): Promise<void>
{
  await server.connection.sendRequest( "shutdown" );
  await server.connection.sendNotification( "exit" );
  server.connection.dispose();
  server.child.kill();
}

function projectOf( server: Server, file: string ): Promise<ProjectOfResult>
{
  return server.connection.sendRequest( "nga/projectOf", { uri: pathToFileURL( file ).toString() } );
}

spec.describe( "which Project builds a file", () =>
{
  let directory = "";
  let server: Server;

  // Two Projects that include one fragment, which names the Module both build,
  // and a file nothing names.
  spec.before( async () =>
  {
    directory = fs.realpathSync( fs.mkdtempSync( path.join( os.tmpdir(), "nga-projects-" ) ) );
    const target = "target { region $0080 .. $00FF ram  region $2000 .. $2FFF ram }\n";
    const phase = "phase main { needs one  entry start }\nentry main\n";
    fs.writeFileSync( path.join( directory, "part.ngp" ), 'modules { "one.asm" }\n' );
    fs.writeFileSync( path.join( directory, "main.ngp" ), 'include "part.ngp"\n' + phase + target );
    fs.writeFileSync( path.join( directory, "other.ngp" ), 'include "part.ngp"\n' + phase + target );
    fs.writeFileSync( path.join( directory, "one.asm" ), ".proc start\n        rts\n.endp\n" );
    fs.writeFileSync( path.join( directory, "stray.asm" ), ".proc stray\n        rts\n.endp\n" );
    server = await start( directory );
  } );

  spec.after( async () =>
  {
    await stop( server );
    fs.rmSync( directory, { recursive: true, force: true } );
  } );

  spec.it( "is a Project of the nearest directory, never a fragment it includes", async () =>
  {
    const owner = await projectOf( server, path.join( directory, "one.asm" ) );
    assert.deepEqual( owner.candidates, [ path.join( directory, "main.ngp" ), path.join( directory, "other.ngp" ) ] );
    assert.equal( owner.project, path.join( directory, "main.ngp" ) );
  } );

  spec.it( "is the one the reader chose among the candidates", async () =>
  {
    const candidates = [ path.join( directory, "main.ngp" ), path.join( directory, "other.ngp" ) ];
    await server.connection.sendNotification( "nga/useProject", { candidates, project: candidates[1] } );
    const owner = await projectOf( server, path.join( directory, "one.asm" ) );
    assert.equal( owner.project, candidates[1] );
  } );

  spec.it( "is nothing for a file no Project names", async () =>
  {
    const owner = await projectOf( server, path.join( directory, "stray.asm" ) );
    assert.deepEqual( owner, { project: null, candidates: [] } );
  } );

  spec.it( "is the Project of a fragment that Project includes", async () =>
  {
    const owner = await projectOf( server, path.join( directory, "part.ngp" ) );
    assert.equal( owner.candidates.length, 2 );
  } );
} );

spec.describe( "a program of the tutorial", () =>
{
  spec.it( "is built by the Project beside it", async () =>
  {
    const program = path.join( ROOT, "tutorial", "01-the-project-and-modules", "sum" );
    const server = await start( ROOT );
    try
    {
      const owner = await projectOf( server, path.join( program, "sum.asm" ) );
      assert.equal( owner.project, path.join( program, "main.ngp" ) );
      assert.deepEqual( server.messages, [] );
    }
    finally
    {
      await stop( server );
    }
  } );
} );

spec.describe( "an nga that is not there", () =>
{
  spec.it( "is said once, as the server starts", async () =>
  {
    const child = spawn( process.execPath, [ SERVER, "--stdio" ], { stdio: [ "pipe", "pipe", "inherit" ] } );
    if ( child.stdout === null || child.stdin === null )
    {
      throw new Error( "the server has no standard streams" );
    }
    const connection = createMessageConnection(
      new StreamMessageReader( child.stdout ),
      new StreamMessageWriter( child.stdin )
    );
    const said = new Promise<string>( ( resolve ) =>
    {
      connection.onNotification( "window/showMessage", ( params: { message: string; } ) => resolve( params.message ) );
    } );
    connection.listen();
    await connection.sendRequest( "initialize", {
      processId: process.pid,
      rootUri: null,
      capabilities: {},
      initializationOptions: { ngaPath: path.join( os.tmpdir(), "no-such-nga" ) }
    } );
    assert.match( await said, /is not a file/ );
    connection.dispose();
    child.kill();
  } );
} );

spec.describe( "the findings of a build", () =>
{
  let directory = "";
  let server: Server;
  const file = ( name: string ) => path.join( directory, name );

  spec.before( async () =>
  {
    directory = fs.realpathSync( fs.mkdtempSync( path.join( os.tmpdir(), "nga-findings-" ) ) );
    fs.writeFileSync(
      file( "main.ngp" ),
      'modules { "one.asm"  "two.asm" }\n'
        + "phase main { needs one, two  entry start }\n"
        + "entry main\n"
        + "target { region $0080 .. $00FF ram  region $2000 .. $20FF ram }\n"
    );
    fs.writeFileSync(
      file( "one.asm" ),
      ".export start, big\n.proc start\n        jsr helper\n        rts\n.endp\n.section\nbig     .res 10\n.ends\n"
    );
    fs.writeFileSync(
      file( "two.asm" ),
      ".export helper\n.proc helper\n        lda big\n        rts\n.endp\n.proc helper\n        rts\n.endp\n"
    );
    server = await start( directory );
  } );

  spec.after( async () =>
  {
    await stop( server );
    fs.rmSync( directory, { recursive: true, force: true } );
  } );

  spec.it( "are shown when a file of the Project is opened, with their notes", async () =>
  {
    await server.connection.sendNotification( "textDocument/didOpen", {
      textDocument: {
        uri: pathToFileURL( file( "two.asm" ) ).toString(),
        languageId: "nga-asm",
        version: 1,
        text: fs.readFileSync( file( "two.asm" ), "utf8" )
      }
    } );
    const [ redefined ] = await publishedFor( server, file( "two.asm" ), ( shown ) => shown.length > 0 );
    assert.equal( redefined?.code, "NGA2203" );
    assert.deepEqual( redefined?.range, { start: { line: 5, character: 6 }, end: { line: 5, character: 12 } } );
    assert.equal( redefined?.relatedInformation?.[0]?.location.uri, pathToFileURL( file( "two.asm" ) ).toString() );
  } );

  spec.it( "are cleared from a file when a save leaves nothing to say about it", async () =>
  {
    fs.writeFileSync( file( "two.asm" ), ".export helper\n.proc helper\n        lda big\n        rts\n.endp\n" );
    await server.connection.sendNotification( "textDocument/didSave", {
      textDocument: { uri: pathToFileURL( file( "two.asm" ) ).toString() }
    } );
    await publishedFor( server, file( "two.asm" ), ( shown ) => shown.length === 0 );
  } );

  spec.it( "follow the text as it is typed, before anything is saved", async () =>
  {
    const uri = pathToFileURL( file( "two.asm" ) ).toString();
    const onDisk = fs.readFileSync( file( "two.asm" ), "utf8" );
    await server.connection.sendNotification( "textDocument/didChange", {
      textDocument: { uri, version: 2 },
      contentChanges: [ { text: ".export helper\n.proc helper\n        lda gone\n        rts\n.endp\n" } ]
    } );
    const [ unknown ] = await publishedFor(
      server,
      file( "two.asm" ),
      ( shown ) => shown.some( ( one ) => one.code === "NGA2206" )
    );
    assert.deepEqual( unknown?.range, { start: { line: 2, character: 12 }, end: { line: 2, character: 16 } } );
    assert.equal( fs.readFileSync( file( "two.asm" ), "utf8" ), onDisk );

    // Typed back to what the disk holds, the buffer is no overlay any more.
    await server.connection.sendNotification( "textDocument/didChange", {
      textDocument: { uri, version: 3 },
      contentChanges: [ { text: onDisk } ]
    } );
    await publishedFor( server, file( "two.asm" ), ( shown ) => shown.length === 0 );
  } );

  spec.it( "with no location are the Project's, pointing at what they are about", async () =>
  {
    // An editor says a file was saved only of a file it has open.
    await server.connection.sendNotification( "textDocument/didOpen", {
      textDocument: {
        uri: pathToFileURL( file( "one.asm" ) ).toString(),
        languageId: "nga-asm",
        version: 1,
        text: fs.readFileSync( file( "one.asm" ), "utf8" )
      }
    } );
    fs.writeFileSync(
      file( "one.asm" ),
      ".export start, big\n.proc start\n        jsr helper\n        rts\n.endp\n.section\nbig     .res 300\n.ends\n"
    );
    await server.connection.sendNotification( "textDocument/didSave", {
      textDocument: { uri: pathToFileURL( file( "one.asm" ) ).toString() }
    } );
    const shown = await publishedFor( server, file( "main.ngp" ), ( found ) => found.length > 0 );
    const phase = shown.find( ( one ) => one.code === "NGA5631" );
    assert.deepEqual( phase?.range, { start: { line: 0, character: 0 }, end: { line: 0, character: 0 } } );
    assert.ok(
      phase?.relatedInformation?.some( ( one ) => one.location.uri === pathToFileURL( file( "one.asm" ) ).toString() )
    );
  } );
} );

spec.describe( "the colours of a file", () =>
{
  let directory = "";
  let server: Server;
  const file = ( name: string ) => path.join( directory, name );

  spec.before( async () =>
  {
    directory = fs.realpathSync( fs.mkdtempSync( path.join( os.tmpdir(), "nga-colours-" ) ) );
    fs.writeFileSync(
      file( "main.ngp" ),
      'modules { "one.asm" }\n'
        + "phase main { needs one  entry start }\n"
        + "entry main\n"
        + "target { region $0080 .. $00FF ram  region $2000 .. $2FFF ram }\n"
    );
    fs.writeFileSync(
      file( "one.asm" ),
      "step = 3\ncount .ztemp 1\n.proc start\nloop    lda #step ; comment\n        sta count\n        jmp loop\n.endp\n"
    );
    server = await start( directory, { workspace: { semanticTokens: { refreshSupport: true } } } );
  } );

  spec.after( async () =>
  {
    await stop( server );
    fs.rmSync( directory, { recursive: true, force: true } );
  } );

  spec.it( "say what each name means once the Project is built", async () =>
  {
    const uri = pathToFileURL( file( "one.asm" ) ).toString();
    await server.connection.sendNotification( "textDocument/didOpen", {
      textDocument: { uri, languageId: "nga-asm", version: 1, text: fs.readFileSync( file( "one.asm" ), "utf8" ) }
    } );
    await refreshed( server, 1 );
    const tokens = await server.connection.sendRequest<{ data: number[]; }>( "textDocument/semanticTokens/full", {
      textDocument: { uri }
    } );

    const seen: string[] = [];
    let line = 0;
    let character = 0;
    for ( let i = 0; i + 4 < tokens.data.length; i += 5 )
    {
      const [ deltaLine = 0, deltaCharacter = 0, length = 0, type = 0, modifiers = 0 ] = tokens.data.slice( i, i + 5 );
      line += deltaLine;
      character = deltaLine === 0 ? character + deltaCharacter : deltaCharacter;
      const text = fs.readFileSync( file( "one.asm" ), "utf8" ).split( "\n" )[line]?.slice(
        character,
        character + length
      );
      seen.push(
        `${text ?? ""}:${server.legend[type] ?? String( type )}${( modifiers & 1 ) !== 0 ? "+declaration" : ""}`
      );
    }
    assert.deepEqual( seen, [
      "step:variable+declaration",
      "3:number",
      "count:variable+declaration",
      ".:directive",
      "ztemp:directive",
      "1:number",
      ".:directive",
      "proc:directive",
      "start:function+declaration",
      "loop:label+declaration",
      "lda:keyword",
      "step:variable",
      "; comment:comment",
      "sta:keyword",
      "count:variable",
      "jmp:keyword",
      "loop:label",
      ".:directive",
      "endp:directive"
    ] );
  } );
} );

spec.describe( "the text the tool wrote", () =>
{
  const program = path.join( ROOT, "tutorial", "01-the-project-and-modules", "sum-in-c" );
  const project = path.join( program, "main.ngp" );
  let server: Server;

  spec.before( async () =>
  {
    server = await start( program, { workspace: { semanticTokens: { refreshSupport: true } } } );
    await server.connection.sendNotification( "textDocument/didOpen", {
      textDocument: {
        uri: pathToFileURL( path.join( program, "sum.ngc" ) ).toString(),
        languageId: "nga-c",
        version: 1,
        text: fs.readFileSync( path.join( program, "sum.ngc" ), "utf8" )
      }
    } );
    await refreshed( server, 1 );
  } );

  spec.after( async () =>
  {
    await stop( server );
  } );

  spec.it( "is listed for the Project, by the paths the facts use", async () =>
  {
    const files = await server.connection.sendRequest<string[]>( "nga/writtenFiles", { project } );
    assert.deepEqual( files, [ "<generated>/sum.asm", "<nga>/runtime.asm" ] );
  } );

  spec.it( "is read as the last build wrote it, and coloured as assembler", async () =>
  {
    const uri = writtenUri( project, "<generated>/sum.asm" );
    const text = await server.connection.sendRequest<string>( "nga/writtenText", { uri } );
    assert.match( text, /^\.source "sum\.ngc", 1\n/ );

    await server.connection.sendNotification( "textDocument/didOpen", {
      textDocument: { uri, languageId: "nga-asm", version: 1, text }
    } );
    const tokens = await server.connection.sendRequest<{ data: number[]; }>( "textDocument/semanticTokens/full", {
      textDocument: { uri }
    } );
    const types = new Set<string>();
    for ( let i = 3; i < tokens.data.length; i += 5 )
    {
      types.add( server.legend[tokens.data[i] ?? 0] ?? "" );
    }
    assert.ok( types.has( "keyword" ) && types.has( "directive" ) && types.has( "function" ) );
  } );
} );

spec.describe( "navigation", () =>
{
  let directory = "";
  let server: Server;
  const file = ( name: string ) => path.join( directory, name );
  const uri = ( name: string ) => pathToFileURL( file( name ) ).toString();

  interface Place
  {
    uri: string;
    range: { start: { line: number; character: number; }; end: { line: number; character: number; }; };
  }

  const where = ( places: Place[] ) =>
    places.map( ( one ) =>
      `${path.basename( one.uri )}:${String( one.range.start.line )}:${String( one.range.start.character )}`
    );

  spec.before( async () =>
  {
    directory = fs.realpathSync( fs.mkdtempSync( path.join( os.tmpdir(), "nga-navigation-" ) ) );
    fs.writeFileSync(
      file( "main.ngp" ),
      'modules { "one.asm"  "two.asm" }\n'
        + "phase main { needs one, two  entry start }\n"
        + "entry main\n"
        + "target { region $0080 .. $00FF ram  region $2000 .. $2FFF ram }\n"
    );
    fs.writeFileSync(
      file( "one.asm" ),
      ".proc start\n        jsr helper\n@loop   dex\n        bne @-loop\n        rts\n.endp\n"
    );
    fs.writeFileSync(
      file( "two.asm" ),
      ".export helper\n.proc helper\n        lda #step\ninner   rts\n.endp\nstep = 3\n"
    );
    server = await start( directory, { workspace: { semanticTokens: { refreshSupport: true } } } );
    await server.connection.sendNotification( "textDocument/didOpen", {
      textDocument: {
        uri: uri( "one.asm" ),
        languageId: "nga-asm",
        version: 1,
        text: fs.readFileSync( file( "one.asm" ), "utf8" )
      }
    } );
    await refreshed( server, 1 );
  } );

  spec.after( async () =>
  {
    await stop( server );
    fs.rmSync( directory, { recursive: true, force: true } );
  } );

  spec.it( "goes from a use to the definition in another file", async () =>
  {
    const found = await server.connection.sendRequest<Place[]>( "textDocument/definition", {
      textDocument: { uri: uri( "one.asm" ) },
      position: { line: 1, character: 14 }
    } );
    assert.deepEqual( where( found ), [ "two.asm:1:6" ] );
  } );

  spec.it( "finds every place a name is written, its `.export` included", async () =>
  {
    const found = await server.connection.sendRequest<Place[]>( "textDocument/references", {
      textDocument: { uri: uri( "two.asm" ) },
      position: { line: 1, character: 8 },
      context: { includeDeclaration: true }
    } );
    assert.deepEqual( where( found ), [ "two.asm:1:6", "one.asm:1:12", "two.asm:0:8" ] );
  } );

  spec.it( "goes from a local reference to its label, which the tool does not say", async () =>
  {
    const found = await server.connection.sendRequest<Place[]>( "textDocument/definition", {
      textDocument: { uri: uri( "one.asm" ) },
      position: { line: 3, character: 14 }
    } );
    assert.deepEqual( where( found ), [ "one.asm:2:0" ] );
  } );

  spec.it( "outlines a file, a Proc holding its labels", async () =>
  {
    await server.connection.sendNotification( "textDocument/didOpen", {
      textDocument: {
        uri: uri( "two.asm" ),
        languageId: "nga-asm",
        version: 1,
        text: fs.readFileSync( file( "two.asm" ), "utf8" )
      }
    } );
    const outline = await server.connection.sendRequest<{ name: string; children?: { name: string; }[]; }[]>(
      "textDocument/documentSymbol",
      { textDocument: { uri: uri( "two.asm" ) } }
    );
    assert.deepEqual( outline.map( ( one ) => [ one.name, ( one.children ?? [] ).map( ( child ) => child.name ) ] ), [
      [ "helper", [ "inner" ] ],
      [ "step", [] ]
    ] );
  } );

  spec.it( "searches every name of the Projects built", async () =>
  {
    const found = await server.connection.sendRequest<{ name: string; }[]>( "workspace/symbol", { query: "HELP" } );
    assert.deepEqual( found.map( ( one ) => one.name ), [ "helper", "helper.inner" ] );
  } );
} );

spec.describe( "a rename", () =>
{
  let directory = "";
  let server: Server;
  const file = ( name: string ) => path.join( directory, name );
  const uri = ( name: string ) => pathToFileURL( file( name ) ).toString();

  interface Edit
  {
    range: { start: { line: number; character: number; }; end: { line: number; character: number; }; };
    newText: string;
  }

  // A file's text with a rename's edits made, from the last to the first.
  const applied = ( name: string, edits: Edit[] ) =>
  {
    const lines = fs.readFileSync( file( name ), "utf8" ).split( "\n" );
    for (
      const edit of [ ...edits ].sort( ( a, b ) =>
        b.range.start.line - a.range.start.line || b.range.start.character - a.range.start.character
      )
    )
    {
      const line = lines[edit.range.start.line] ?? "";
      lines[edit.range.start.line] = line.slice( 0, edit.range.start.character ) + edit.newText
        + line.slice( edit.range.end.character );
    }
    return lines.join( "\n" );
  };

  spec.before( async () =>
  {
    directory = fs.realpathSync( fs.mkdtempSync( path.join( os.tmpdir(), "nga-rename-" ) ) );
    fs.writeFileSync(
      file( "main.ngp" ),
      'include "atari/130xe.ngp"\n'
        + 'modules { "one.asm"  "two.asm" }\n'
        + "phase main { needs one, two  entry start }\n"
        + "entry main\n"
    );
    fs.writeFileSync(
      file( "one.asm" ),
      ".proc start\n        jsr helper\n@loop   dex\n        bne @-loop\n        lda RTCLOK\n        rts\n.endp\n"
    );
    fs.writeFileSync(
      file( "two.asm" ),
      ".export helper\n.proc helper\n        jmp helper.inner\ninner   rts\n.endp\n"
    );
    server = await start( directory );
    for ( const name of [ "one.asm", "two.asm" ] )
    {
      await server.connection.sendNotification( "textDocument/didOpen", {
        textDocument: {
          uri: uri( name ),
          languageId: "nga-asm",
          version: 1,
          text: fs.readFileSync( file( name ), "utf8" )
        }
      } );
    }
  } );

  spec.after( async () =>
  {
    await stop( server );
    fs.rmSync( directory, { recursive: true, force: true } );
  } );

  spec.it( "of a Proc reaches its `.export`, every use, and its Labels' qualified names", async () =>
  {
    const prepared = await server.connection.sendRequest<{ placeholder: string; }>( "textDocument/prepareRename", {
      textDocument: { uri: uri( "one.asm" ) },
      position: { line: 1, character: 14 }
    } );
    assert.equal( prepared.placeholder, "helper" );

    const edit = await server.connection.sendRequest<{ changes: Record<string, Edit[]>; }>( "textDocument/rename", {
      textDocument: { uri: uri( "one.asm" ) },
      position: { line: 1, character: 14 },
      newName: "assist"
    } );
    assert.equal( applied( "one.asm", edit.changes[uri( "one.asm" )] ?? [] ).split( "\n" )[1], "        jsr assist" );
    assert.equal(
      applied( "two.asm", edit.changes[uri( "two.asm" )] ?? [] ),
      ".export assist\n.proc assist\n        jmp assist.inner\ninner   rts\n.endp\n"
    );
  } );

  spec.it( "of a local label changes it and what reaches it, and nothing else", async () =>
  {
    const edit = await server.connection.sendRequest<{ changes: Record<string, Edit[]>; }>( "textDocument/rename", {
      textDocument: { uri: uri( "one.asm" ) },
      position: { line: 3, character: 14 },
      newName: "scan"
    } );
    assert.deepEqual( applied( "one.asm", edit.changes[uri( "one.asm" )] ?? [] ).split( "\n" ).slice( 2, 4 ), [
      "@scan   dex",
      "        bne @-scan"
    ] );
  } );

  spec.it( "is refused for a name the shipped library writes", async () =>
  {
    await assert.rejects(
      server.connection.sendRequest( "textDocument/prepareRename", {
        textDocument: { uri: uri( "one.asm" ) },
        position: { line: 4, character: 14 }
      } ),
      /outside the workspace/
    );
  } );

  spec.it( "is refused for what is not a name", async () =>
  {
    await assert.rejects(
      server.connection.sendRequest( "textDocument/rename", {
        textDocument: { uri: uri( "one.asm" ) },
        position: { line: 1, character: 14 },
        newName: "2nd"
      } ),
      /is not a name/
    );
  } );
} );

spec.describe( "completion", () =>
{
  let directory = "";
  let server: Server;
  const file = ( name: string ) => path.join( directory, name );
  const uri = ( name: string ) => pathToFileURL( file( name ) ).toString();

  interface Item
  {
    label: string;
    detail?: string;
  }

  const labelsAt = async ( name: string, line: number, character: number ) =>
    ( await server.connection.sendRequest<Item[]>( "textDocument/completion", {
      textDocument: { uri: uri( name ) },
      position: { line, character }
    } ) ).map( ( one ) => one.label );

  spec.before( async () =>
  {
    directory = fs.realpathSync( fs.mkdtempSync( path.join( os.tmpdir(), "nga-completion-" ) ) );
    fs.writeFileSync(
      file( "main.ngp" ),
      'include "atari/130xe.ngp"\n'
        + 'modules { "one.asm"  "two.asm" }\n'
        + "group both { one, two }\n"
        + "phase main { needs both  entry start }\n"
        + "entry main\n"
    );
    fs.writeFileSync(
      file( "one.asm" ),
      "step = 3\n.proc start\n        jsr helper\n        ld\n        rts\n.endp\n"
    );
    fs.writeFileSync( file( "two.asm" ), ".export helper\n.proc helper\n        rts\n.endp\nprivate = 1\n" );
    server = await start( directory, { workspace: { semanticTokens: { refreshSupport: true } } } );
    await server.connection.sendNotification( "textDocument/didOpen", {
      textDocument: {
        uri: uri( "one.asm" ),
        languageId: "nga-asm",
        version: 1,
        text: fs.readFileSync( file( "one.asm" ), "utf8" )
      }
    } );
    await refreshed( server, 1 );
  } );

  spec.after( async () =>
  {
    await stop( server );
    fs.rmSync( directory, { recursive: true, force: true } );
  } );

  spec.it( "offers the mnemonics of the Project's processor where a statement starts", async () =>
  {
    const labels = await labelsAt( "one.asm", 3, 10 );
    assert.ok( labels.includes( "lda" ) && labels.includes( "jeq" ) );
    assert.ok( !labels.includes( "stz" ) );
  } );

  spec.it( "offers the names an operand can reach, and not another Module's own", async () =>
  {
    const labels = await labelsAt( "one.asm", 2, 14 );
    assert.ok( labels.includes( "helper" ) && labels.includes( "step" ) && labels.includes( "start" ) );
    assert.ok( !labels.includes( "private" ) );
  } );

  spec.it( "offers the Modules and groups a Phase needs, and the library to include", async () =>
  {
    assert.deepEqual(
      ( await labelsAt( "main.ngp", 3, 19 ) ).filter( ( one ) => [ "one", "two", "both" ].includes( one ) ),
      [
        "one",
        "two",
        "both"
      ]
    );
    assert.ok( ( await labelsAt( "main.ngp", 0, 9 ) ).includes( "atari/130xe.ngp" ) );
  } );
} );

spec.describe( "where a name stands", () =>
{
  let directory = "";
  let server: Server;
  const file = ( name: string ) => path.join( directory, name );
  const uri = ( name: string ) => pathToFileURL( file( name ) ).toString();

  spec.before( async () =>
  {
    directory = fs.realpathSync( fs.mkdtempSync( path.join( os.tmpdir(), "nga-placement-" ) ) );
    fs.writeFileSync(
      file( "main.ngp" ),
      'modules { "one.asm" }\n'
        + "phase main { needs one  entry start }\n"
        + "entry main\n"
        + "target { region $0080 .. $00FF ram  region $2000 .. $2FFF ram }\n"
    );
    fs.writeFileSync(
      file( "one.asm" ),
      "a1 .ztemp 2\nb1 .ztemp 2\n.proc start\n        jsr a\n        jsr b\n        rts\n.endp\n"
        + ".proc a\n        sta a1\n        rts\n.endp\n.proc b\n        sta b1\n        rts\n.endp\n"
    );
    server = await start( directory, { workspace: { semanticTokens: { refreshSupport: true } } } );
    await server.connection.sendNotification( "textDocument/didOpen", {
      textDocument: {
        uri: uri( "one.asm" ),
        languageId: "nga-asm",
        version: 1,
        text: fs.readFileSync( file( "one.asm" ), "utf8" )
      }
    } );
    await refreshed( server, 1 );
  } );

  spec.after( async () =>
  {
    await stop( server );
    fs.rmSync( directory, { recursive: true, force: true } );
  } );

  spec.it( "is said by a hover, with the Temporaries that share a byte", async () =>
  {
    const hover = await server.connection.sendRequest<{ contents: { value: string; }; }>( "textDocument/hover", {
      textDocument: { uri: uri( "one.asm" ) },
      position: { line: 8, character: 13 }
    } );
    assert.match( hover.contents.value, /^\*\*a1\*\* — temporary of `one`/ );
    assert.match( hover.contents.value, /shares with b1/ );
  } );

  spec.it( "is said above every Section a file opens", async () =>
  {
    const lenses = await server.connection.sendRequest<
      { range: { start: { line: number; }; }; command: { title: string; }; }[]
    >(
      "textDocument/codeLens",
      { textDocument: { uri: uri( "one.asm" ) } }
    );
    assert.deepEqual( lenses.map( ( one ) => one.range.start.line ), [ 0, 1, 2, 7, 11 ] );
    assert.match( lenses[2]?.command.title ?? "", /^\$2\d{3}–\$2\d{3} · \d+ B · main$/ );
  } );
} );

spec.describe( "the listing in the margin", () =>
{
  let directory = "";
  let server: Server;
  const file = ( name: string ) => path.join( directory, name );
  const uri = ( name: string ) => pathToFileURL( file( name ) ).toString();

  spec.before( async () =>
  {
    directory = fs.realpathSync( fs.mkdtempSync( path.join( os.tmpdir(), "nga-listing-" ) ) );
    fs.writeFileSync(
      file( "main.ngp" ),
      'include "atari/130xe.ngp"\n'
        + "container xex\n"
        + 'modules { "one.asm" }\n'
        + "phase main { needs one  entry start }\n"
        + "entry main\n"
    );
    fs.writeFileSync(
      file( "one.asm" ),
      ".proc start\n        lda #1\n        lda table\n        rts\n.endp\n"
        + ".section\ntable   .byte 1, 2, 3, 4, 5, 6, 7, 8, 9, 10\n.ends\n"
    );
    server = await start( directory, { workspace: { semanticTokens: { refreshSupport: true } } } );
    await server.connection.sendNotification( "textDocument/didOpen", {
      textDocument: {
        uri: uri( "one.asm" ),
        languageId: "nga-asm",
        version: 1,
        text: fs.readFileSync( file( "one.asm" ), "utf8" )
      }
    } );
    await refreshed( server, 1 );
  } );

  spec.after( async () =>
  {
    await stop( server );
    fs.rmSync( directory, { recursive: true, force: true } );
  } );

  const hintsOf = () =>
    server.connection.sendRequest<{ position: { line: number; character: number; }; label: string; }[]>(
      "textDocument/inlayHint",
      {
        textDocument: { uri: uri( "one.asm" ) },
        range: { start: { line: 0, character: 0 }, end: { line: 9, character: 0 } }
      }
    );

  spec.it( "says for each statement where it landed and what it wrote", async () =>
  {
    const lines = await server.connection.sendRequest<{ line: number; label: string; }[]>( "nga/listing", {
      uri: uri( "one.asm" )
    } );
    const byLine = new Map( lines.map( ( one ) => [ one.line, one.label ] ) );
    assert.match( byLine.get( 1 ) ?? "", /^\$[0-9A-F]{4}: A9 01$/ );
    assert.match( byLine.get( 3 ) ?? "", /: 60$/ );
    assert.match( byLine.get( 6 ) ?? "", /: 01 02 03 04 05 06 07 08 … 10 B$/ );
  } );

  spec.it( "stands beside every statement only where the reader asks for it", async () =>
  {
    assert.deepEqual( await hintsOf(), [] );
    await server.connection.sendNotification( "workspace/didChangeConfiguration", {
      settings: { nga: { listing: "always" } }
    } );
    const hints = await hintsOf();
    assert.match( hints.find( ( one ) => one.position.line === 1 )?.label ?? "", /: A9 01$/ );
    assert.equal( hints.find( ( one ) => one.position.line === 1 )?.position.character, 14 );
  } );

  spec.it( "is the tree of the Container too, each run with the statement that wrote it", async () =>
  {
    const bytes = await server.connection.sendRequest<
      {
        container: string;
        parts: { runs: { what: string; place?: { uri: string; range: { start: { line: number; }; }; }; }[]; }[];
      } | null
    >( "nga/containerBytes", { uri: uri( "one.asm" ) } );
    assert.equal( bytes?.container, "xex" );
    const lines = ( bytes?.parts ?? [] )
      .flatMap( ( part ) => part.runs )
      .filter( ( run ) => run.place?.uri === uri( "one.asm" ) )
      .map( ( run ) => run.place?.range.start.line );
    assert.deepEqual( [ ...new Set( lines ) ].sort(), [ 1, 2, 3, 6 ] );
  } );
} );

spec.describe( "what Prune dropped", () =>
{
  let directory = "";
  let server: Server;
  const file = ( name: string ) => path.join( directory, name );

  spec.before( async () =>
  {
    directory = fs.realpathSync( fs.mkdtempSync( path.join( os.tmpdir(), "nga-pruned-" ) ) );
    fs.writeFileSync(
      file( "main.ngp" ),
      'modules { "one.asm" }\n'
        + "phase main { needs one  entry start }\n"
        + "entry main\n"
        + "target { region $0080 .. $00FF ram  region $2000 .. $2FFF ram }\n"
    );
    fs.writeFileSync(
      file( "one.asm" ),
      ".proc start\n        rts\n.endp\n.proc unused\n        nop\n        rts\n.endp\n"
    );
    server = await start( directory );
    await server.connection.sendNotification( "textDocument/didOpen", {
      textDocument: {
        uri: pathToFileURL( file( "one.asm" ) ).toString(),
        languageId: "nga-asm",
        version: 1,
        text: fs.readFileSync( file( "one.asm" ), "utf8" )
      }
    } );
  } );

  spec.after( async () =>
  {
    await stop( server );
    fs.rmSync( directory, { recursive: true, force: true } );
  } );

  spec.it( "is faded from its `.proc` to its `.endp`", async () =>
  {
    const [ faded ] = await publishedFor( server, file( "one.asm" ), ( shown ) => shown.length > 0 );
    assert.deepEqual( faded?.range, { start: { line: 3, character: 0 }, end: { line: 6, character: 5 } } );
    assert.deepEqual( ( faded as { tags?: number[]; } | undefined )?.tags, [ 1 ] );
  } );
} );

spec.describe( "a `.ngc` beside what it became", () =>
{
  const program = path.join( ROOT, "tutorial", "01-the-project-and-modules", "sum-in-c" );
  let server: Server;

  spec.before( async () =>
  {
    server = await start( program, { workspace: { semanticTokens: { refreshSupport: true } } } );
    await server.connection.sendNotification( "textDocument/didOpen", {
      textDocument: {
        uri: pathToFileURL( path.join( program, "sum.ngc" ) ).toString(),
        languageId: "nga-c",
        version: 1,
        text: fs.readFileSync( path.join( program, "sum.ngc" ), "utf8" )
      }
    } );
    await refreshed( server, 1 );
  } );

  spec.after( async () =>
  {
    await stop( server );
  } );

  spec.it( "is the text it compiled to, with the lines each line of the C became", async () =>
  {
    const found = await server.connection.sendRequest<{ uri: string; lines: { source: number; }[]; } | null>(
      "nga/compiledOf",
      { uri: pathToFileURL( path.join( program, "sum.ngc" ) ).toString() }
    );
    assert.equal( found?.uri, writtenUri( path.join( program, "main.ngp" ), "<generated>/sum.asm" ) );
    assert.ok( ( found?.lines ?? [] ).length > 0 );
    assert.ok( ( found?.lines ?? [] ).every( ( one ) => one.source >= 0 ) );
  } );
} );

spec.describe( "the memory map in the editor", () =>
{
  let directory = "";
  let server: Server;
  const file = ( name: string ) => path.join( directory, name );

  spec.before( async () =>
  {
    directory = fs.realpathSync( fs.mkdtempSync( path.join( os.tmpdir(), "nga-map-page-" ) ) );
    fs.writeFileSync(
      file( "main.ngp" ),
      'modules { "one.asm" }\n'
        + "phase main { needs one  entry start }\n"
        + "entry main\n"
        + "target { region $0080 .. $00FF ram  region $2000 .. $2FFF ram }\n"
    );
    fs.writeFileSync( file( "one.asm" ), ".proc start\n        rts\n.endp\n" );
    server = await start( directory, { workspace: { semanticTokens: { refreshSupport: true } } } );
    await server.connection.sendNotification( "textDocument/didOpen", {
      textDocument: {
        uri: pathToFileURL( file( "one.asm" ) ).toString(),
        languageId: "nga-asm",
        version: 1,
        text: fs.readFileSync( file( "one.asm" ), "utf8" )
      }
    } );
    await refreshed( server, 1 );
  } );

  spec.after( async () =>
  {
    await stop( server );
    fs.rmSync( directory, { recursive: true, force: true } );
  } );

  spec.it( "links every Section to where it is written", async () =>
  {
    const page = await server.connection.sendRequest<{ project: string; html: string; } | null>( "nga/mapPage", {
      uri: pathToFileURL( file( "one.asm" ) ).toString(),
      nonce: "n0nce"
    } );
    assert.equal( page?.project, file( "main.ngp" ) );
    assert.ok( page?.html.includes( `data-uri="${pathToFileURL( file( "one.asm" ) ).toString()}" data-line="0"` ) );
  } );
} );
