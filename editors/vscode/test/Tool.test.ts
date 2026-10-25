// What the extension relies on the tool for and the suite cannot hold, since
// the suite does not run the command line: how `--overlay` is read. See
// docs/spec/project-file.md.

import * as assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import * as fs from "node:fs";
import * as os from "node:os";
import * as path from "node:path";
// Called through the namespace: dprint lays out a bare `describe` and `it` in a
// style of its own, with no space inside the parentheses.
import * as spec from "node:test";

const ROOT = path.resolve( import.meta.dirname, "..", "..", ".." );
const NGA = process.env["NGA_BINARY"] ?? path.join( ROOT, "build", "debug", "src", "nga_cli", "nga" );

interface Finding
{
  id: string;
}

function run( directory: string, args: string[] ): { status: number | null; stdout: string; }
{
  const done = spawnSync( NGA, args, { cwd: directory, encoding: "utf8" } );
  return { status: done.status, stdout: done.stdout };
}

function findingsOf( stdout: string ): string[]
{
  return ( JSON.parse( stdout ) as { diagnostics: Finding[]; } ).diagnostics.map( ( one ) => one.id );
}

spec.describe( "an overlay", () =>
{
  let directory = "";
  const file = ( name: string ) => path.join( directory, name );

  spec.before( () =>
  {
    directory = fs.realpathSync( fs.mkdtempSync( path.join( os.tmpdir(), "nga-overlay-" ) ) );
    fs.writeFileSync(
      file( "main.ngp" ),
      'modules { "one.asm" }\n'
        + "phase main { needs one  entry start }\n"
        + "entry main\n"
        + "target { region $0080 .. $00FF ram  region $2000 .. $2FFF ram }\n"
    );
    fs.writeFileSync( file( "one.asm" ), ".proc start\n        rts\n.endp\n" );
    fs.writeFileSync( file( "buffer" ), ".proc start\n        lda missing\n        rts\n.endp\n" );
  } );

  spec.after( () =>
  {
    fs.rmSync( directory, { recursive: true, force: true } );
  } );

  spec.it( "is read wherever the build reads its path", () =>
  {
    const done = run( directory, [ "main.ngp", "--facts", "-", "--overlay", file( "one.asm" ), "buffer" ] );
    assert.equal( done.status, 1 );
    assert.deepEqual( findingsOf( done.stdout ), [ "NGA2206" ] );
  } );

  spec.it( "that cannot be read stops the run rather than reading the disk", () =>
  {
    const done = run( directory, [ "main.ngp", "--facts", "-", "--overlay", "one.asm", "no-such-buffer" ] );
    assert.deepEqual( findingsOf( done.stdout ), [ "NGA1246" ] );
  } );

  spec.it( "refuses to write a binary", () =>
  {
    const done = run( directory, [ "main.ngp", "-o", "out.bin", "--facts", "-", "--overlay", "one.asm", "buffer" ] );
    assert.deepEqual( findingsOf( done.stdout ), [ "NGA1247" ] );
    assert.equal( fs.existsSync( file( "out.bin" ) ), false );
  } );

  spec.it( "is what `--classify` colours, under the path it stands for", () =>
  {
    const plain = run( directory, [ "--classify", "one.asm" ] );
    const overlaid = run( directory, [ "--classify", "one.asm", "--overlay", "one.asm", "buffer" ] );
    const spans = ( stdout: string ) =>
      ( JSON.parse( stdout ) as { tokens: { path: string; spans: unknown[]; }[]; } ).tokens[0];
    assert.equal( spans( overlaid.stdout )?.path, "one.asm" );
    assert.notEqual( spans( overlaid.stdout )?.spans.length, spans( plain.stdout )?.spans.length );
  } );
} );

spec.describe( "the memory map as a file", () =>
{
  let directory = "";
  const file = ( name: string ) => path.join( directory, name );
  const pageScript = path.resolve( import.meta.dirname, "..", "dist", "MapPage.cjs" );

  spec.before( () =>
  {
    directory = fs.realpathSync( fs.mkdtempSync( path.join( os.tmpdir(), "nga-map-" ) ) );
    fs.writeFileSync(
      file( "main.ngp" ),
      'modules { "one.asm" }\n'
        + "phase main { needs one  entry start }\n"
        + "entry main\n"
        + "target { region $0080 .. $00FF ram  region $2000 .. $2FFF ram }\n"
    );
    fs.writeFileSync( file( "one.asm" ), ".proc start\n        rts\n.endp\n" );
  } );

  spec.after( () =>
  {
    fs.rmSync( directory, { recursive: true, force: true } );
  } );

  spec.it( "is written from a file of facts", () =>
  {
    const facts = run( directory, [ "main.ngp", "--facts", "facts.json", "--fact", "map,project" ] );
    assert.equal( facts.status, 0 );
    const done = spawnSync( process.execPath, [ pageScript, file( "facts.json" ), "-o", file( "map.html" ) ], {
      encoding: "utf8"
    } );
    assert.equal( done.status, 0 );
    const page = fs.readFileSync( file( "map.html" ), "utf8" );
    assert.ok( page.startsWith( "<!DOCTYPE html>" ) && page.includes( "one.start" ) );
  } );

  spec.it( "is written straight from a Project, by the tool it names", () =>
  {
    const done = spawnSync( process.execPath, [ pageScript, file( "main.ngp" ) ], {
      encoding: "utf8",
      env: { ...process.env, NGA_BINARY: NGA }
    } );
    assert.equal( done.status, 0 );
    assert.ok( done.stdout.includes( '<svg class="panel"' ) );
  } );
} );
