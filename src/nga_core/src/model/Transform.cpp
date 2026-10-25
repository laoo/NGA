#include "nga/model/Transform.hpp"

#include "nga/model/Assemble.hpp"
#include "nga/model/InPlace.hpp"
#include "nga/model/Segments.hpp"
#include "nga/model/Transition.hpp"
#include "nga/model/Zx0.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nga::model
{

namespace
{

/// The longest run or literal one packet can carry: the count travels in seven
/// bits beside the flag that says which kind it is.
constexpr std::size_t MOST_PER_PACKET = 0x7F;

/// Shorter than this a run costs more as a run than as literal bytes, since a
/// run is a control byte and a value where a literal is a control byte shared
/// by everything in it.
constexpr std::size_t SHORTEST_RUN = 3;

constexpr std::uint8_t RUN_FLAG = 0x80;

std::vector<std::uint8_t> encodeCopy( std::span<std::uint8_t const> from )
{
  return { from.begin(), from.end() };
}

/// How many bytes equal to the first one begin at `at`, capped at a packet.
std::size_t runAt( std::span<std::uint8_t const> from, std::size_t at )
{
  std::size_t length = 1;
  while ( at + length < from.size() && from[at + length] == from[at] && length < MOST_PER_PACKET )
  {
    ++length;
  }
  return length;
}

std::vector<std::uint8_t> encodeRle( std::span<std::uint8_t const> from )
{
  std::vector<std::uint8_t> out;
  std::size_t at = 0;

  while ( at < from.size() )
  {
    if ( std::size_t const run = runAt( from, at ); run >= SHORTEST_RUN )
    {
      out.push_back( static_cast<std::uint8_t>( RUN_FLAG | run ) );
      out.push_back( from[at] );
      at += run;
      continue;
    }

    // Literal bytes up to the next run worth encoding, so that a run never
    // ends up inside a literal packet where it would cost its full length.
    std::size_t literal = 1;
    while ( at + literal < from.size() && literal < MOST_PER_PACKET && runAt( from, at + literal ) < SHORTEST_RUN )
    {
      ++literal;
    }
    out.push_back( static_cast<std::uint8_t>( literal ) );
    out.insert( out.end(),
                from.begin() + static_cast<std::ptrdiff_t>( at ),
                from.begin() + static_cast<std::ptrdiff_t>( at + literal ) );
    at += literal;
  }

  return out;
}

/// A stored form: its own size in two bytes, which the decoder reads before the
/// data, and then the bytes as the encoder left them — see
/// docs/spec/transition.md.
std::vector<std::uint8_t> formOf( std::span<std::uint8_t const> encoded )
{
  std::vector<std::uint8_t> form{ static_cast<std::uint8_t>( encoded.size() & 0xFF ),
                                  static_cast<std::uint8_t>( ( encoded.size() >> 8 ) & 0xFF ) };
  form.insert( form.end(), encoded.begin(), encoded.end() );
  return form;
}

/// The forms a Payload comes to. One, unless no unit of storage holds the whole
/// of it: then the **input** is cut and each part encoded on its own, so that
/// every piece is a form a decoder reads from its first byte, and the Frame
/// carries a block for each. Cutting the output instead would give a second
/// piece no decoder could start in the middle of.
///
/// Where to cut is found by asking the encoder rather than by knowing anything
/// about it: the whole is encoded, and where it does not fit, the input is
/// scaled by how much it overshot and encoded again, until it does. That
/// terminates because the length falls every time, and it is right for any
/// format because nothing here assumes one. A cut read off an optimal parse's
/// own arithmetic would be exact and take one encode rather than a few, and is
/// in the queue.
std::vector<Storage::StoredPiece> piecesOf( Format const& format,
                                            std::span<std::uint8_t const> initialised,
                                            std::uint32_t landing,
                                            std::uint32_t ceiling )
{
  std::vector<Storage::StoredPiece> pieces;
  std::size_t at = 0;
  while ( at < initialised.size() || pieces.empty() )
  {
    std::size_t take = initialised.size() - at;
    std::vector<std::uint8_t> encoded = format.encode( initialised.subspan( at, take ) );
    while ( ceiling > 0 && formOf( encoded ).size() > ceiling && take > 1 )
    {
      // Scaled by what it came to against what there is room for, and never by
      // less than one byte, so that a format which does not shrink at all still
      // gets there.
      std::size_t const wanted = ( take * ceiling ) / ( encoded.size() + 2 );
      take = std::min( take - 1, std::max<std::size_t>( wanted, 1 ) );
      encoded = format.encode( initialised.subspan( at, take ) );
    }
    pieces.push_back(
        Storage::StoredPiece{ .form = formOf( encoded ), .landing = landing + static_cast<std::uint32_t>( at ) } );
    at += take;
    if ( take == 0 )
    {
      break;
    }
  }
  return pieces;
}

/// A byte read and a byte written, the size bytes first, as `copy` reads it.
std::optional<std::uint32_t> copyMargin( std::span<std::uint8_t const> form )
{
  InPlace stream{ form };
  stream.read();
  stream.read();
  while ( stream.more() )
  {
    stream.read();
    stream.wrote( 1 );
  }
  return stream.margin();
}

/// As `lib/stream/rle.asm` reads it: the size, then a control byte per packet,
/// and a literal's bytes each written as it is read, or a run's one value
/// written as many times as the packet counts.
std::optional<std::uint32_t> rleMargin( std::span<std::uint8_t const> form )
{
  InPlace stream{ form };
  stream.read();
  stream.read();
  while ( stream.more() )
  {
    std::uint8_t const control = stream.read();
    std::uint32_t const count = control & MOST_PER_PACKET;
    if ( ( control & RUN_FLAG ) != 0 )
    {
      stream.read();
      stream.wrote( count );
      continue;
    }
    for ( std::uint32_t i = 0; i < count && stream.more(); ++i )
    {
      stream.read();
      stream.wrote( 1 );
    }
  }
  return stream.margin();
}

/// The formats the tool encodes, by name. The number a block carries is not
/// here: it is the position of the format's decoder in the Project's list,
/// which the end of Assemble decides.
constexpr std::array<Format, 3> FORMATS{ { { .name = "copy", .encode = &encodeCopy, .margin = &copyMargin },
                                           { .name = "rle", .encode = &encodeRle, .margin = &rleMargin },
                                           { .name = "zx0", .encode = &encodeZx0, .margin = &zx0InPlaceMargin } } };

} // namespace

std::span<Format const> formats()
{
  return FORMATS;
}

Format const* formatNamed( std::string_view name )
{
  for ( Format const& one : FORMATS )
  {
    if ( one.name == name )
    {
      return &one;
    }
  }
  return nullptr;
}

namespace
{

/// Numbers the decoders: `copy` first, then Project and declaration order,
/// holding a format to one decoder and every decoder to a format the tool
/// encodes. Only what the program can reach is numbered — `copy`, and every
/// format the Project named — so that a decoder a variant lists and nothing
/// uses is dispatched to by nothing, and Prune drops it.
void numberDecoders( Project& project, std::span<Module const> modules, diag::DiagnosticSink& sink )
{
  struct Declared
  {
    ModuleIndex module;
    TransformDeclaration const* declaration;
    std::uint32_t symbol;
  };

  std::vector<Declared> declared;

  for ( std::uint32_t index = 0; index < modules.size(); ++index )
  {
    for ( TransformDeclaration const& declaration : modules[index].transforms() )
    {
      std::optional<std::uint32_t> const symbol = modules[index].symbols().indexOf( declaration.label );
      if ( !symbol.has_value() || modules[index].symbols().symbols()[*symbol].kind != SymbolKind::LABEL )
      {
        // Reported by the Module, which found no Label of that name.
        continue;
      }
      if ( formatNamed( declaration.format ) == nullptr )
      {
        sink.add( diag::diagnostic( diag::DiagnosticId::UNKNOWN_FORMAT )
                      .at( declaration.formatSpan.begin, declaration.formatSpan.length )
                      .arg( "format", std::string{ declaration.format } ) );
        continue;
      }
      auto const same = [&declaration]( Declared const& other )
      { return other.declaration->format == declaration.format; };
      if ( auto const other = std::ranges::find_if( declared, same ); other != declared.end() )
      {
        // The copy a compressed file decodes with while it loads is the same
        // text as the resident decoder and is no second decoder of the
        // format — see docs/decisions/0259-a-xex-may-be-compressed-segment-by-segment.md.
        if ( project.loadModule == ModuleIndex{ index } )
        {
          continue;
        }
        sink.add( diag::diagnostic( diag::DiagnosticId::FORMAT_ALREADY_DECODED )
                      .at( declaration.formatSpan.begin, declaration.formatSpan.length )
                      .arg( "format", std::string{ declaration.format } )
                      .note( diag::diagnostic( diag::DiagnosticId::DECODER_IS_HERE )
                                 .at( other->declaration->span.begin, other->declaration->span.length )
                                 .arg( "format", std::string{ declaration.format } ) ) );
        continue;
      }
      declared.push_back( Declared{ .module = ModuleIndex{ index }, .declaration = &declaration, .symbol = *symbol } );
    }
  }

  // `copy` first: whether one exists at all is the driver's business, and
  // resolveDriver says so where a Transition is taken.
  auto const isCopy = []( Declared const& one ) { return one.declaration->format == "copy"; };
  std::ranges::stable_partition( declared, isCopy );

  auto const used = [&project]( Declared const& one )
  {
    return one.declaration->format == "copy" ||
           std::ranges::any_of( project.transforms,
                                [&one]( TransformRequest const& request )
                                { return request.transform == one.declaration->format; } );
  };
  project.decoders.clear();
  for ( Declared const& one : declared )
  {
    if ( !used( one ) )
    {
      continue;
    }
    project.decoders.push_back( Decoder{ .format = std::string{ one.declaration->format },
                                         .encode = formatNamed( one.declaration->format )->encode,
                                         .module = one.module,
                                         .symbol = one.symbol } );
  }
}

/// The number of the decoder of a format, or nothing.
std::optional<TransformId> decoderOf( Project const& project, std::string_view format )
{
  for ( std::size_t index = 0; index < project.decoders.size(); ++index )
  {
    if ( project.decoders[index].format == format )
    {
      return static_cast<TransformId>( index );
    }
  }
  return std::nullopt;
}

} // namespace

void resolveTransforms( diag::SourceManager const& /*sources*/,
                        Project& project,
                        std::span<Module> modules,
                        diag::DiagnosticSink& sink )
{
  numberDecoders( project, modules, sink );

  std::vector<SectionRef> transformed;
  std::vector<TransformRequest const*> sites;

  for ( TransformRequest const& request : project.transforms )
  {
    if ( formatNamed( request.transform ) == nullptr )
    {
      sink.add( diag::diagnostic( diag::DiagnosticId::NO_SUCH_TRANSFORM )
                    .at( request.formatSpan.begin, request.formatSpan.length )
                    .arg( "transform", request.transform ) );
      continue;
    }
    // Without storage only a `.xex` decodes anything, while it loads, so in
    // any other Container there is no decoder to look for.
    if ( !project.target.hasStorage() && project.container != Container::XEX && request.transform != "copy" )
    {
      sink.add( diag::diagnostic( diag::DiagnosticId::TRANSFORM_WITHOUT_LOADER )
                    .at( request.span.begin, request.span.length )
                    .arg( "section", request.section )
                    .arg( "module", std::string{ modules[request.module.value].name() } ) );
      continue;
    }
    if ( !decoderOf( project, request.transform ).has_value() )
    {
      sink.add( diag::diagnostic( diag::DiagnosticId::NO_DECODER )
                    .at( request.span.begin, request.span.length )
                    .arg( "transform", request.transform ) );
      continue;
    }

    // By a Label of the Section, which carries the Namespace it was declared
    // in: `one.screen` from the Project is the name the Module knows.
    Module& module = modules[request.module.value];
    std::optional<SectionIndex> found;
    if ( Symbol const* const symbol = module.symbols().find( request.section ); symbol != nullptr )
    {
      if ( auto const* const label = std::get_if<LabelPosition>( &symbol->value ); label != nullptr )
      {
        found = label->section;
      }
    }
    if ( !found.has_value() )
    {
      sink.add( diag::diagnostic( diag::DiagnosticId::NO_SUCH_SECTION )
                    .at( request.span.begin, request.span.length )
                    .arg( "section", request.section )
                    .arg( "module", std::string{ module.name() } ) );
      continue;
    }

    // Two Labels of one Section name one Section, and a Section is
    // transformed once.
    SectionRef const where{ .module = request.module, .section = *found };
    if ( auto const seen = std::ranges::find( transformed, where ); seen != transformed.end() )
    {
      sink.add( diag::diagnostic( diag::DiagnosticId::TRANSFORM_SECTION_TWICE )
                    .at( request.span.begin, request.span.length )
                    .arg( "section", request.section )
                    .arg( "previous", sites[static_cast<std::size_t>( seen - transformed.begin() )]->section ) );
      continue;
    }

    module.sectionAt( *found ).setFormat(
        static_cast<std::uint8_t>( formatNamed( request.transform ) - formats().data() ) );
    transformed.push_back( where );
    sites.push_back( &request );
  }

  // Nothing loads it, so nothing transforms it: a warning about the program
  // rather than about the Section. Which Sections have a Payload is the load
  // set of every edge, which is what PlaceStorage will ask the same question
  // of later.
  // And the cold start's, which loads the entry Phase from nowhere wherever a
  // Container takes one — see
  // docs/decisions/0236-a-xex-with-storage-brings-a-transformed-start-by-a-cold-start.md.
  std::vector<SectionRef> payloads;
  if ( takesColdStart( project ) )
  {
    payloads = loadSetOf( project.phases, std::nullopt, project.phases.entry, modules );
  }
  for ( std::uint32_t from = 0; from < project.phases.phases.size(); ++from )
  {
    for ( PhaseIndex const next : project.phases.phases[from].then )
    {
      for ( SectionRef const where : loadSetOf( project.phases, PhaseIndex{ from }, next, modules ) )
      {
        payloads.push_back( where );
      }
    }
  }

  for ( std::size_t index = 0; index < transformed.size(); ++index )
  {
    // Without storage nothing is loaded by an edge: a `.xex` decodes the
    // Section while it loads, which the load's own Step answers for, and no
    // other Container was let this far — see
    // docs/decisions/0235-a-xex-decodes-in-place-while-it-loads.md.
    if ( !project.target.hasStorage() )
    {
      continue;
    }
    if ( std::ranges::find( payloads, transformed[index] ) != payloads.end() )
    {
      continue;
    }
    sink.add( diag::diagnostic( diag::DiagnosticId::SECTION_HAS_NO_PAYLOAD )
                  .at( sites[index]->span.begin, sites[index]->span.length )
                  .arg( "section", sites[index]->section )
                  .arg( "module", std::string{ modules[sites[index]->module.value].name() } ) );
  }
}

void transformPayloads( Sized const& build, Storage& storage, Bytes const& bytes )
{
  Sizes const& sizes = build.sizes();
  std::span<Module const> const modules = build.modules();

  // What one form may come to, where images stay inside one unit: the widest
  // unit there is, since that is the ceiling an author is held to and the
  // placement will find it. Zero where images may cross, and then nothing is
  // ever cut.
  std::uint32_t const ceiling = imagesStayInOneUnit( build.project() ) ? build.project().target.widestUsableUnit() : 0;
  for ( std::uint32_t module = 0; module < modules.size(); ++module )
  {
    Module const& one = modules[module];
    for ( std::uint32_t index = 0; index < one.sections().size(); ++index )
    {
      SectionRef const where{ .module = ModuleIndex{ module }, .section = SectionIndex{ index } };
      if ( !storage.hasPayload( where ) || !sizes.isKnown( where ) )
      {
        continue;
      }
      // The host half is the format's, whether or not a decoder exists: a
      // Payload nothing decodes has been reported, and its form still lets
      // the Banks be packed and refused where they are too small. The number
      // a Frame carries is the decoder's, zero where there is none.
      Format const& format = formats()[one.sections()[index].format()];
      TransformId const id = decoderOf( build.project(), format.name ).value_or( TRANSFORM_COPY );
      InitialisedExtent const extent = sizes.initialisedExtentOf( where );
      std::span<std::uint8_t const> const initialised = contentOf( bytes, where, extent );
      storage.store( where, id, piecesOf( format, initialised, extent.begin, ceiling ) );
    }
  }
}

namespace
{

/// The glue a Container that loads through memory calls: a cell it writes a
/// state's index into, and INITAD at the Proc that hands it to `showAt`.
/// Roots, since nothing in the program names them, and only for the Container
/// that does: a raw image would carry the resident bytes for nothing, and a
/// Root where the program declared none would turn Prune on. Without a Window
/// the stream reads through there is nothing to fill, and the Container says
/// so when it is written. Nothing where there is nothing to write.
std::string loaderGlueOf( Project const& project )
{
  if ( project.container != Container::XEX || !project.driver.has_value() || !project.driver->stream.has_value() )
  {
    return {};
  }
  std::string const window = project.target.windows[project.driver->stream->value].name;
  bool const wide = stateBytesOf( project, *project.driver->stream ) > 1;
  std::string source = ".export ngaLoadUnit, ngaLoadMap\n"
                       ".section root\n"
                       "ngaLoadUnit\n";
  source += wide ? "        .res 2\n" : "        .res 1\n";
  source += ".ends\n"
            ".proc ngaLoadMap, root\n"
            "        ldx ngaLoadUnit\n";
  if ( wide )
  {
    source += "        ldy ngaLoadUnit+1\n";
  }
  source += "        nga.showAt " + window +
            "\n"
            "        rts\n"
            ".endp\n";
  return source;
}

} // namespace

void addTransformDispatcher( diag::SourceManager& sources,
                             Project& project,
                             std::vector<Module>& modules,
                             diag::DiagnosticSink& sink )
{
  // A chain of compares and jumps rather than a table of addresses, because
  // the chain is code, which the suite executes on a 6502. It ends in `brk`
  // for a number the tool did not write, which is unreachable for the reason
  // the routine's scan's is: nothing else writes the byte this dispatches on.
  // The driver's roles are macros of the driver's Module, used here as
  // `nga.show` and `nga.showAt` and expanded once this Module exists, which
  // is why it is assembled before the expansion. The Window the stream reads
  // through is the Target's; the driver has been resolved by now, so it is
  // known where this text is written.
  std::string source = "; The dispatcher over the decoders the program declared, in the order the\n"
                       "; tool numbered them. See docs/spec/transition.md.\n";

  // Whether the routine hands the driver the set of ROMs a Phase needs, which
  // its text asks under `.if` since it is written before the driver is read —
  // see docs/decisions/0229-no-window-has-a-base.md.
  bool const selects = project.driver.has_value() && project.driver->select.has_value();
  source += ".export ngaSelects\nngaSelects = " + std::string{ selects ? "1" : "0" } + "\n";
  source += loaderGlueOf( project );
  source += ".export ngaTransform\n.proc ngaTransform\n";
  for ( std::size_t index = 0; index < project.decoders.size(); ++index )
  {
    Decoder const& decoder = project.decoders[index];
    std::string const number = std::to_string( index );
    std::string_view const label = modules[decoder.module.value].symbols().symbols()[decoder.symbol].name;
    source += "        cmp #" + number + "\n";
    source += "        bne @not" + number + "\n";
    source += "        jmp " + std::string{ label } + "\n";
    source += "@not" + number + "\n";
  }
  source += "        brk\n.endp\n";

  std::size_t const phaseCount = project.phases.phases.size();
  addAssembledModule(
      sources, project, modules, "nga.transforms", std::move( source ), Residency::all( phaseCount ), sink );
}

void addDriverInit( diag::SourceManager& sources,
                    Project& project,
                    std::vector<Module>& modules,
                    diag::DiagnosticSink& sink )
{
  // Only for the Container that calls it: a raw image is started by nothing
  // of the tool's, and a Root nothing calls would turn Prune on in a program
  // that declared none.
  if ( !project.driver.has_value() || !project.driver->init.has_value() || project.container != Container::XEX )
  {
    return;
  }

  // A Module of its own and not the dispatcher's, because the dispatcher is
  // built only for a Project with Transitions and this is wanted without
  // one: a program that draws a frame and loops has no edge to take and still
  // has to reach the hardware before its first `.with`. A Root, since nothing
  // in the program names it — the Container calls it by address — and outside
  // the Window, as every generated Proc that the driver's macros land in is.
  std::string source = "; The driver's `init`, wrapped so that a Container has an address to call.\n"
                       "; See docs/spec/transition.md and docs/spec/xex.md.\n"
                       ".export ngaInit\n"
                       ".proc ngaInit, root\n"
                       "        nga.init\n"
                       "        rts\n"
                       ".endp\n";
  // Not in a `final` Phase other than the entry, which nothing enters from
  // the Container — see
  // docs/decisions/0245-a-final-phase-keeps-nothing-only-an-exit-runs.md.
  addAssembledModule( sources, project, modules, "nga.init", std::move( source ), notFinal( project.phases ), sink );
}

void addUnitsGlue( diag::SourceManager& sources,
                   Project& project,
                   std::vector<Module>& modules,
                   diag::DiagnosticSink& sink )
{
  // Where there is a dispatcher it holds the glue. A program with storage has
  // one only where it has a Transition, so one with a single Phase is here as
  // one without storage is. What has a Bank to be shown is a Pane the solver
  // gives one of the units the stream reads: the Container writes a Section
  // with bytes there, and a Pane of reservations alone is still a Bank whose
  // state only the driver can turn into hardware, which is what the glue asks
  // it — the suite included. A Pane pinned to a named state has no Bank, and
  // is filled through the same glue only where it holds bytes — see
  // docs/decisions/0234-without-storage-bytes-stay-where-they-are-live.md and
  // docs/decisions/0256-a-named-state-is-filled-where-a-xex-can-show-it.md.
  if ( project.transitionRoutine.has_value() || !project.driver.has_value() || !project.driver->stream.has_value() )
  {
    return;
  }
  WindowIndex const stream = *project.driver->stream;
  Target const& target = project.target;
  bool const filled =
      std::ranges::any_of( modules,
                           [&target, stream]( Module const& module )
                           {
                             return std::ranges::any_of(
                                 module.sections(),
                                 [&target, stream]( Section const& section )
                                 {
                                   // A unit set's Section with bytes is filled
                                   // through the Window that shows the set.
                                   if ( std::optional<UnitSetIndex> const set = section.unitSet(); set.has_value() )
                                   {
                                     return section.emitsBytes() && target.unitSetShownBy( stream ) == set;
                                   }
                                   if ( !section.pane().has_value() )
                                   {
                                     return false;
                                   }
                                   Pane const& pane = target.panes[section.pane()->value];
                                   return pane.window == stream && ( !pane.state.has_value() || section.emitsBytes() );
                                 } );
                           } );
  std::string source = loaderGlueOf( project );
  if ( !filled || source.empty() )
  {
    return;
  }
  source = "; What a `.xex` without a Transition fills a Pane's Bank through. See docs/spec/xex.md.\n" + source;
  std::size_t const phaseCount = project.phases.phases.size();
  addAssembledModule( sources, project, modules, "nga.units", std::move( source ), Residency::all( phaseCount ), sink );
}

void addRunStub( diag::SourceManager& sources,
                 Project& project,
                 std::vector<Module>& modules,
                 diag::DiagnosticSink& sink )
{
  // Two reasons to start a program through a stub: a return that must reach
  // `finalize`, and the ROMs the entry Phase needs, which are selected before
  // a byte of it runs. A Container whose program cannot return has a cold
  // start of its own for the second, where it has one at all.
  bool const returns = project.container == Container::XEX || project.container == Container::PRG;
  bool const finishes = returns && project.driver.has_value() && project.driver->finalize.has_value();
  bool const selects = returns && project.driver.has_value() && project.driver->select.has_value();
  if ( !finishes && !selects )
  {
    return;
  }
  std::size_t const phaseCount = project.phases.phases.size();
  auto const add = [&]( std::string_view name, std::string source, Residency const& residency )
  { addAssembledModule( sources, project, modules, name, std::move( source ), residency, sink ); };

  // Where the program returns to, from whichever Phase it is in by then: the
  // driver's `finalize`, and the return to what started the program. Not in
  // a `final` Phase other than the entry, which the program never returns
  // from — see docs/decisions/0245-a-final-phase-keeps-nothing-only-an-exit-runs.md.
  if ( finishes )
  {
    add( "nga.finish",
         "; Where a program that returns ends: the driver's `finalize`, then back to\n"
         "; what started it. See docs/decisions/0229-no-window-has-a-base.md.\n"
         ".export ngaFinish\n"
         ".proc ngaFinish, root\n"
         "        nga.finalize\n"
         "        rts\n"
         ".endp\n",
         notFinal( project.phases ) );
  }

  // The start, in the entry Phase alone since it names the entry: the entry
  // Phase's ROMs selected, and a `jsr` without the call, so that the entry's
  // return lands on `ngaFinish` and not here, which a Transition may have
  // taken out of memory by then.
  std::string source = "; How the program is started: the ROMs its entry Phase needs selected, and\n"
                       "; where it ends left on the stack, so that its return lands on ngaFinish\n"
                       "; from whichever Phase it returns. See\n"
                       "; docs/decisions/0229-no-window-has-a-base.md.\n"
                       ".export ngaRun\n"
                       ".proc ngaRun, root\n";
  if ( selects )
  {
    source += "        lda #" + std::to_string( romsOf( project, project.phases.entry ) ) + "\n        nga.select\n";
  }
  if ( finishes )
  {
    source += "        .own                    ; followed by the rts the program returns with\n"
              "        lda #>( ngaFinish - 1 )\n"
              "        pha\n"
              "        .own\n"
              "        lda #<( ngaFinish - 1 )\n"
              "        pha\n";
  }
  // Into the cold start where the program takes one, which enters the entry
  // Phase as every Phase is entered and leaves the stack as it found it, so
  // the entry's return still lands on ngaFinish — see
  // docs/decisions/0236-a-xex-with-storage-brings-a-transformed-start-by-a-cold-start.md.
  source += takesColdStart( project ) ? "        jmp ngaStart\n.endp\n" : "        jmp nga.entry\n.endp\n";
  Residency entry{ phaseCount };
  entry.add( project.phases.entry );
  add( "nga.run", std::move( source ), entry );
}

} // namespace nga::model
