// The memory map as a page, composed from the facts — the one composition of
// it, which the editor shows beside the code and `dist/MapPage.cjs` writes to a
// file. What the page is, is docs/spec/memory-map.md.
//
// One self-contained document: inline style and script, nothing fetched.
// Address runs across and Phase runs down, a rectangle per Section coloured by
// its Module, the pools the solver allocates from shaded behind them, and a
// Window's ranges, which the pools leave out, shaded as the Window's. A
// Temporary is drawn hollow, since the whole point of one is that the bytes
// under it are shared, and a solid block would say the opposite.

import type { FactsDocument, MapSection, UnitSetFacts, WindowFacts } from "./server/Facts.ts";

const ZERO_PAGE = 0x100;

// Wide enough that a byte is visible in the zero page panel; the memory panel
// scales to the same width however much of the address space is used.
const WIDTH = 1200;
const ROW = 26;
const GAP = 4;

// Where the editor opens a Section, by its Module and its name.
export interface Link
{
  uri: string;
  line: number;
  character: number;
}

export interface PageOptions
{
  // Where each Section is written, which a ⌘ or Ctrl click opens; none in a
  // page written to a file, which no editor reads.
  links?: Map<string, Link>;

  // The nonce of the page's script, for a host that admits no other.
  nonce?: string;
}

export function linkKey( module: string, name: string ): string
{
  return `${module}\n${name}`;
}

export function mapPageOf( facts: FactsDocument, options: PageOptions = {} ): string
{
  const map = facts.map;
  if ( map === undefined )
  {
    return pageAround( "<p>The last build reached no layout, so there is no map to show.</p>", options );
  }
  const phases = ( map.phases ?? [] ).map( ( phase, i ) => phase.name === "" ? String( i ) : phase.name );
  const entries = map.sections;
  // A Section in a unit set stands at a linear address, which no CPU reads:
  // in the CPU's panels it would stretch memory to the medium's size.
  const cpu = entries.filter( ( one ) => one.units === undefined );
  const top = Math.max(
    ZERO_PAGE,
    ...cpu.filter( ( one ) => ( one.begin ?? 0 ) >= ZERO_PAGE ).map( ( one ) => one.end ?? 0 )
  );

  const rows = ( map.phases ?? [] )
    .map( ( phase, i ) =>
      `<li>${escape( phases[i] ?? "" )}<span>${String( phase.zeroPageUsed ?? 0 )} zp, ${
        String( phase.memoryUsed ?? 0 )
      } bytes</span></li>`
    )
    .join( "" );
  const legend = [ ...new Set( entries.map( ( one ) => one.module ) ) ]
    .sort()
    .map( ( module ) => `<span class="key"><i style="background:${colourOf( module )}"></i>${escape( module )}</span>` )
    .join( "" );
  const pools = map.pools;
  const windows = facts.project?.windows ?? [];

  return pageAround(
    [
      `<ol class="phases">${rows}</ol>`,
      `<div class="legend">${legend}</div>`,
      graph( facts ),
      panel( "Zero page", cpu, phases, 0, ZERO_PAGE, pools?.zeroPage ?? [], windows, options ),
      panel(
        "Memory",
        cpu,
        phases,
        ZERO_PAGE,
        top,
        [ ...pools?.general ?? [], ...pools?.readOnly ?? [] ],
        windows,
        options
      ),
      ...( facts.project?.unitSets ?? [] ).map( ( set ) => unitSetPanel( facts, set, phases, options ) ),
      storage( facts )
    ].join( "\n" ),
    options
  );
}

// A stable colour per Module, so that one Module reads as one colour on every
// page: a checksum of its name turned into a hue, as `zlib.crc32` and
// `colorsys` once made it, so that no page changed colour when this moved.
export function colourOf( module: string ): string
{
  const spun = ( crc32( Buffer.from( module, "utf8" ) ) % 360 ) / 360;
  const [ red, green, blue ] = hsvToRgb( spun, 0.45, 0.85 );
  const byte = ( value: number ) => Math.trunc( value * 255 ).toString( 16 ).padStart( 2, "0" );
  return `#${byte( red )}${byte( green )}${byte( blue )}`;
}

function crc32( bytes: Uint8Array ): number
{
  let crc = 0xffffffff;
  for ( const byte of bytes )
  {
    crc ^= byte;
    for ( let bit = 0; bit < 8; ++bit )
    {
      crc = ( crc >>> 1 ) ^ ( 0xedb88320 & -( crc & 1 ) );
    }
  }
  return ( crc ^ 0xffffffff ) >>> 0;
}

function hsvToRgb( hue: number, saturation: number, value: number ): [ number, number, number ]
{
  const sector = Math.trunc( hue * 6 );
  const fraction = hue * 6 - sector;
  const p = value * ( 1 - saturation );
  const q = value * ( 1 - saturation * fraction );
  const t = value * ( 1 - saturation * ( 1 - fraction ) );
  switch ( sector % 6 )
  {
    case 0:
      return [ value, t, p ];
    case 1:
      return [ q, value, p ];
    case 2:
      return [ p, value, t ];
    case 3:
      return [ p, q, value ];
    case 4:
      return [ t, p, value ];
    default:
      return [ value, p, q ];
  }
}

function hex( value: number ): string
{
  return "$" + value.toString( 16 ).toUpperCase().padStart( 4, "0" );
}

function escape( text: string ): string
{
  return text.replace( /&/g, "&amp;" ).replace( /</g, "&lt;" ).replace( />/g, "&gt;" ).replace( /"/g, "&quot;" );
}

// What the text map says of an entry, which the page shows where the pointer
// rests on it.
function tooltip( entry: MapSection ): string
{
  const said = [
    `${entry.module}.${entry.name}`,
    `${hex( entry.begin ?? 0 )}-${hex( ( entry.end ?? 1 ) - 1 )}`,
    entry.kind
  ];
  if ( entry.root === true )
  {
    said.push( "root" );
  }
  if ( entry.pane !== undefined )
  {
    said.push( `in ${entry.pane}, state ${String( entry.paneState ?? 0 )}` );
  }
  // What the text asked for, so that what the solver chose is what is left.
  if ( entry.pinned === true )
  {
    said.push( "pinned" );
  }
  if ( entry.align !== undefined )
  {
    said.push( `align ${String( entry.align )}` );
  }
  if ( entry.within !== undefined )
  {
    said.push( `within ${String( entry.within )}` );
  }
  if ( entry.in !== undefined )
  {
    said.push( `in ${entry.in.join( ", " )}` );
  }
  if ( entry.then !== undefined )
  {
    said.push( `then ${entry.then}` );
  }
  const [ first, last ] = entry.initialised ?? [ entry.begin ?? 0, entry.end ?? 0 ];
  if ( first === last )
  {
    said.push( "reserved only" );
  }
  else if ( first !== entry.begin || last !== entry.end )
  {
    said.push( `initialised ${hex( first )}-${hex( last - 1 )}` );
  }
  if ( ( entry.shares ?? [] ).length > 0 )
  {
    said.push( "shares with " + ( entry.shares ?? [] ).join( ", " ) );
  }
  // One piece unless no unit of storage held the whole of the Payload, and
  // then one for each form it was cut into, each placed on its own.
  for ( const piece of entry.waits?.pieces ?? [] )
  {
    said.push(
      `waits in unit ${String( piece.unit )} at ${hex( piece.offset )} (${entry.waits?.as ?? ""}, ${
        String( piece.size )
      } bytes)`
    );
  }
  return said.join( "  " );
}

// The rows an entry covers: its run of Phases, or all of them for an entry in
// no Phase, which stands somewhere all the same.
function rowsOf( entry: MapSection, count: number ): number[]
{
  const first = entry.firstPhase;
  const last = entry.lastPhase;
  if ( first === null || first === undefined || last === null || last === undefined )
  {
    return [ ...Array( count ).keys() ];
  }
  return [ ...Array( last - first + 1 ).keys() ].map( ( i ) => first + i ).filter( ( row ) => row < count );
}

// One band of the address space, the Phases as its rows. Every rectangle
// carries the addresses it covers, so that the script lays the band out again
// for whatever part of it is zoomed into.
function panel(
  title: string,
  entries: MapSection[],
  phases: string[],
  begin: number,
  end: number,
  pools: [ number, number ][],
  windows: { name: string; ranges: [ number, number ][]; }[],
  options: PageOptions
): string
{
  if ( end <= begin )
  {
    return "";
  }
  const height = Math.max( phases.length, 1 ) * ( ROW + GAP );
  const out = [
    `<h2>${escape( title )} <span class="range">${hex( begin )}-${hex( end - 1 )}</span>`
    + ` <button class="out">zoom out</button> <button class="all">all</button></h2>`,
    `<svg class="panel" viewBox="0 0 ${String( WIDTH )} ${String( height )}" width="100%" height="${
      String( height )
    }" preserveAspectRatio="none" role="img" data-begin="${String( begin )}" data-end="${String( end )}">`
  ];
  for ( let row = 0; row < phases.length; ++row )
  {
    out.push(
      `<rect class="bed" x="0" y="${String( row * ( ROW + GAP ) )}" width="${String( WIDTH )}" height="${
        String( ROW )
      }"/>`
    );
  }
  for ( const [ from, to ] of pools )
  {
    if ( to > begin && from < end )
    {
      out.push(
        `<rect class="pool" data-from="${String( from )}" data-to="${String( to )}" y="0" height="${
          String( height )
        }"/>`
      );
    }
  }
  // A Window's ranges are cut out of the pools, since what stands there is
  // whatever state is shown; without its own shade it would read as no memory
  // at all, with the Panes' Sections drawn on nothing.
  for ( const window of windows )
  {
    for ( const [ from, to ] of window.ranges )
    {
      if ( to > begin && from < end )
      {
        out.push(
          `<rect class="window" data-from="${String( from )}" data-to="${String( to )}" y="0" height="${
            String( height )
          }"><title>${escape( `window ${window.name}  ${hex( from )}-${hex( to - 1 )}` )}</title></rect>`
        );
      }
    }
  }
  for ( const entry of entries )
  {
    const from = entry.begin ?? 0;
    const to = entry.end ?? 0;
    if ( to <= begin || from >= end )
    {
      continue;
    }
    const colour = colourOf( entry.module );
    const hollow = entry.kind === "temporary";
    const link = options.links?.get( linkKey( entry.module, entry.name ) );
    const linked = link === undefined
      ? ""
      : ` data-uri="${escape( link.uri )}" data-line="${String( link.line )}" data-character="${
        String( link.character )
      }"`;
    for ( const row of rowsOf( entry, phases.length ) )
    {
      out.push(
        `<rect class="entry" data-from="${String( from )}" data-to="${String( to )}"${linked} y="${
          String( row * ( ROW + GAP ) )
        }" height="${String( ROW )}" fill="${hollow ? "none" : colour}" stroke="${colour}" stroke-width="${
          hollow ? "2" : "0.5"
        }"><title>${escape( tooltip( entry ) )}</title></rect>`
      );
    }
  }
  out.push( "</svg>" );
  return out.join( "\n" );
}

// One unit set as the medium sees it, at linear addresses: what stands in the
// set itself, and every Pane's Section on a Bank of it, at that Bank's start
// plus its offset in the Window — the ranges laid end to end — once for each
// member of a family. Bank 0 starts at `linear`, or at zero where the set
// declares none, and each next one a Window's size further on; the Banks are
// shaded alternately, since a Bank is what a switch shows.
function unitSetPanel( facts: FactsDocument, set: UnitSetFacts, phases: string[], options: PageOptions ): string
{
  const project = facts.project;
  const base = set.linear?.[0] ?? 0;
  const shown = ( project?.windows ?? [] ).find( ( window ) =>
    window.states?.some( ( state ) => state.units === set.name )
  );
  const bank = shown === undefined ? 0 : sizeOf( shown );

  const placed: MapSection[] = [];
  for ( const entry of facts.map?.sections ?? [] )
  {
    if ( entry.units === set.name )
    {
      placed.push( entry );
      continue;
    }
    const pane = project?.panes?.find( ( one ) => one.name === entry.pane );
    const window = project?.windows?.find( ( one ) => one.name === pane?.window );
    const state = window?.states?.find( ( one ) => one.units === set.name );
    if ( pane === undefined || window === undefined || state === undefined )
    {
      continue;
    }
    const first = ( entry.paneState ?? 0 ) - state.index;
    const offset = offsetIn( window, entry.begin ?? 0 );
    if ( first < 0 || first >= ( state.count ?? 0 ) || offset === undefined )
    {
      continue;
    }
    const size = ( entry.end ?? 0 ) - ( entry.begin ?? 0 );
    for ( let member = 0; member < ( pane.count ?? 1 ); ++member )
    {
      const begin = base + ( first + member ) * sizeOf( window ) + offset;
      placed.push( { ...entry, begin, end: begin + size } );
    }
  }
  if ( placed.length === 0 )
  {
    return "";
  }

  const end = set.linear?.[1]
    ?? Math.max( base + set.count * bank, ...placed.map( ( one ) => one.end ?? 0 ) );
  const banks: [ number, number ][] = [];
  for ( let index = 0; bank > 0 && index < set.count; index += 2 )
  {
    banks.push( [ base + index * bank, base + ( index + 1 ) * bank ] );
  }
  return panel( set.name, placed, phases, base, end, banks, [], options );
}

function sizeOf( window: WindowFacts ): number
{
  return window.ranges.reduce( ( sum, [ from, to ] ) => sum + to - from, 0 );
}

// How far into the Window an address is, its ranges laid end to end; nothing
// for one it does not cover.
function offsetIn( window: WindowFacts, address: number ): number | undefined
{
  let before = 0;
  for ( const [ from, to ] of window.ranges )
  {
    if ( address >= from && address < to )
    {
      return before + address - from;
    }
    before += to - from;
  }
  return undefined;
}

// The Phases and the edges between them, each Phase where its row of the
// panels is named: an edge forward arcs above, an edge back arcs below, and
// the entry Phase is drawn with a heavier border.
function graph( facts: FactsDocument ): string
{
  const phases = facts.project?.phases ?? [];
  if ( phases.length < 2 )
  {
    return "";
  }
  const spacing = WIDTH / phases.length;
  const middle = 70;
  const x = ( i: number ) => spacing * ( i + 0.5 );
  const out = [
    `<h2>Phases</h2>`,
    `<svg class="graph" viewBox="0 0 ${String( WIDTH )} 140" width="100%" height="140" role="img">`,
    `<defs><marker id="arrow" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto">`
    + `<path d="M0,0 L10,5 L0,10 z" class="arrow"/></marker></defs>`
  ];
  phases.forEach( ( phase, from ) =>
  {
    for ( const to of phase.then ?? [] )
    {
      if ( to === from )
      {
        out.push(
          `<path class="edge" d="M${String( x( from ) - 8 )},${
            String( middle - 14 )
          } a 10 12 0 1 1 16 0" marker-end="url(#arrow)"/>`
        );
        continue;
      }
      const above = to > from;
      const lift = Math.min( 60, 18 + Math.abs( to - from ) * 12 );
      const y = above ? middle - 14 : middle + 14;
      const bend = above ? y - lift : y + lift;
      out.push(
        `<path class="edge" d="M${String( x( from ) )},${String( y )} Q${String( ( x( from ) + x( to ) ) / 2 )},${
          String( bend )
        } ${String( x( to ) )},${String( y )}" marker-end="url(#arrow)"/>`
      );
    }
  } );
  phases.forEach( ( phase, i ) =>
  {
    out.push(
      `<g class="node${phase.entry === true ? " entry" : ""}"><rect x="${String( x( i ) - spacing * 0.4 )}" y="${
        String( middle - 14 )
      }" width="${String( spacing * 0.8 )}" height="28" rx="6"/>`
        + `<text x="${String( x( i ) )}" y="${String( middle + 5 )}">${escape( phase.name )}</text></g>`
    );
  } );
  out.push( "</svg>" );
  return out.join( "\n" );
}

// What waits in each unit, and every Frame, as rows of a table. A row per
// piece: a Payload is one stored form unless no unit held the whole of it, and
// then the parts are placed apart, and one row naming a unit and an offset
// would be a run of the medium that is not all of it.
function storage( facts: FactsDocument ): string
{
  const map = facts.map;
  const units = map?.units ?? [];
  const frames = map?.frames ?? [];
  if ( map === undefined || ( units.length === 0 && frames.length === 0 ) )
  {
    return "";
  }
  const word = map.unit ?? "unit";
  const waiting: [ number, number, string, number, string, number | null, number | null ][] = [];
  for ( const entry of map.sections )
  {
    for ( const piece of entry.waits?.pieces ?? [] )
    {
      waiting.push( [
        piece.unit,
        piece.offset,
        `${entry.module}.${entry.name}`,
        piece.size,
        entry.waits?.as ?? "",
        entry.waits?.liveFirst ?? null,
        entry.waits?.liveLast ?? null
      ] );
    }
  }
  for ( const frame of frames )
  {
    waiting.push( [ frame.unit, frame.offset, frame.name, frame.size, "frame", frame.liveFirst, frame.liveLast ] );
  }
  waiting.sort( ( a, b ) => a[0] - b[0] || a[1] - b[1] || ( a[2] < b[2] ? -1 : a[2] > b[2] ? 1 : 0 ) );
  const rows = waiting.map( ( [ unit, offset, name, size, kind, first, last ] ) =>
  {
    const live = first === null ? "" : first === last ? String( first ) : `${String( first )}..${String( last )}`;
    return `<tr><td>${escape( word )} ${String( unit )} ${hex( offset )}</td><td>${escape( name )}${
      kind === "" ? "" : ` <em>${escape( kind )}</em>`
    }</td><td>${String( size )}</td><td>${live}</td></tr>`;
  } );
  return [
    `<h2>Storage <span>${String( units.length )} ${escape( word )}(s) of ${
      String( map.unitSize ?? 0 )
    } bytes</span></h2>`,
    `<table><tr><th>Where</th><th>What</th><th>Size</th><th>Live</th></tr>`,
    ...rows,
    `</table>`
  ].join( "\n" );
}

// The page around a body: its style, which takes the editor's colours where an
// editor shows it and its own elsewhere, and its script.
function pageAround( body: string, options: PageOptions ): string
{
  const nonce = options.nonce === undefined ? "" : ` nonce="${escape( options.nonce )}"`;
  const policy = options.nonce === undefined
    ? ""
    : `<meta http-equiv="Content-Security-Policy" content="default-src 'none'; style-src 'unsafe-inline'; `
      + `script-src 'nonce-${escape( options.nonce )}';">\n`;
  const opening = options.links === undefined
    ? "click to zoom"
    : "click to zoom, ⌘ or Ctrl click to open where it is written";
  return `<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
${policy}<title>NGA memory map</title>
<style>
  :root { --bg: var(--vscode-editor-background, #14161a); --fg: var(--vscode-editor-foreground, #dcd9d3);
          --dim: var(--vscode-descriptionForeground, #85807a); --panel: #1b1e23; --line: #2b2f35; --bed: #202429;
          --pool: rgba(120, 140, 170, 0.12); --window: rgba(190, 150, 90, 0.14); }
  body { background: var(--bg); color: var(--fg); font: 13px/1.5 ui-monospace, Menlo, Consolas, monospace;
         margin: 0; padding: 16px 20px 48px; }
  h1 { font-size: 15px; font-weight: 600; margin: 0 0 4px; }
  h2 { font-size: 12px; font-weight: 600; margin: 22px 0 6px; }
  h2 span, .hint { color: var(--dim); font-weight: normal; }
  h2 button { font: inherit; font-size: 11px; color: var(--dim); background: none; border: 1px solid var(--line);
              border-radius: 3px; padding: 0 6px; cursor: pointer; }
  svg { background: var(--panel); border: 1px solid var(--line); display: block; }
  svg.panel { cursor: zoom-in; }
  rect.bed { fill: var(--bed); }
  rect.pool { fill: var(--pool); }
  rect.window { fill: var(--window); }
  rect[data-uri] { cursor: pointer; }
  ol.phases { list-style: none; padding: 0; margin: 8px 0 0; }
  ol.phases li { color: var(--dim); }
  ol.phases li span { color: var(--fg); margin-left: 8px; }
  .legend { margin: 10px 0 0; }
  .key { display: inline-block; margin: 0 12px 4px 0; color: var(--dim); }
  .key i { display: inline-block; width: 10px; height: 10px; margin-right: 5px; }
  .graph .edge { fill: none; stroke: var(--dim); stroke-width: 1.5; }
  .graph .arrow { fill: var(--dim); }
  .graph .node rect { fill: var(--bed); stroke: var(--dim); stroke-width: 1; }
  .graph .node.entry rect { stroke: var(--fg); stroke-width: 3; }
  .graph text { fill: var(--fg); font-size: 12px; text-anchor: middle; }
  table { border-collapse: collapse; margin-top: 6px; }
  th, td { border: 1px solid var(--line); padding: 3px 8px; text-align: left; }
  th { color: var(--dim); font-weight: 600; }
  em { color: var(--dim); font-style: normal; }
</style>
</head>
<body>
<h1>NGA memory map <span class="hint">${opening}</span></h1>
${body}
<script${nonce}>${SCRIPT}</script>
</body>
</html>
`;
}

// The page's own behaviour, as the spec has it: a panel is eight blocks, and a
// click zooms into the one under the pointer, down to 16 bytes across the whole
// width; `zoom out` goes back a step and `all` to the whole range. A rectangle
// under the pointer comes to the front, so that of two Temporaries at one
// address the one asked about is the one seen. A ⌘ or Ctrl click on a Section
// the page knows the place of asks the editor to open it.
const SCRIPT = `
const editor = typeof acquireVsCodeApi === "function" ? acquireVsCodeApi() : undefined;
const WIDTH = ${String( WIDTH )};
const hex = ( value ) => "$" + value.toString( 16 ).toUpperCase().padStart( 4, "0" );
for ( const svg of document.querySelectorAll( "svg.panel" ) )
{
  const whole = [ Number( svg.dataset.begin ), Number( svg.dataset.end ) ];
  const steps = [ whole ];
  const heading = svg.previousElementSibling;
  const lay = () =>
  {
    const [ begin, end ] = steps[ steps.length - 1 ];
    heading.querySelector( ".range" ).textContent = hex( begin ) + "-" + hex( end - 1 );
    for ( const rect of svg.querySelectorAll( "rect[data-from]" ) )
    {
      const from = Number( rect.dataset.from );
      const to = Number( rect.dataset.to );
      if ( to <= begin || from >= end )
      {
        rect.style.display = "none";
        continue;
      }
      rect.style.display = "";
      const left = ( Math.max( from, begin ) - begin ) * WIDTH / ( end - begin );
      const right = ( Math.min( to, end ) - begin ) * WIDTH / ( end - begin );
      rect.setAttribute( "x", left.toFixed( 2 ) );
      rect.setAttribute( "width", Math.max( right - left, 1 ).toFixed( 2 ) );
    }
  };
  svg.addEventListener( "click", ( event ) =>
  {
    const target = event.target;
    if ( ( event.metaKey || event.ctrlKey ) && target.dataset && target.dataset.uri !== undefined )
    {
      editor?.postMessage( {
        open: { uri: target.dataset.uri, line: Number( target.dataset.line ), character: Number( target.dataset.character ) }
      } );
      return;
    }
    const [ begin, end ] = steps[ steps.length - 1 ];
    if ( end - begin <= 16 )
    {
      return;
    }
    const at = begin + ( event.offsetX * WIDTH / svg.clientWidth ) * ( end - begin ) / WIDTH;
    const block = Math.max( Math.ceil( ( end - begin ) / 8 ), 16 );
    const from = Math.min( Math.max( begin + Math.floor( ( at - begin ) / block ) * block, begin ), end - block );
    steps.push( [ from, from + block ] );
    lay();
  } );
  for ( const rect of svg.querySelectorAll( "rect.entry" ) )
  {
    rect.addEventListener( "mouseenter", () => rect.parentNode.appendChild( rect ) );
  }
  heading.querySelector( ".out" ).addEventListener( "click", () =>
  {
    if ( steps.length > 1 )
    {
      steps.pop();
      lay();
    }
  } );
  heading.querySelector( ".all" ).addEventListener( "click", () =>
  {
    steps.length = 1;
    lay();
  } );
  lay();
}
`;
