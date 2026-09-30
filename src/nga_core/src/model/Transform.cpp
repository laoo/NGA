#include "nga/model/Transform.hpp"

#include "nga/model/Assemble.hpp"
#include "nga/model/Transition.hpp"
#include "nga/model/Zx0.hpp"

#include <algorithm>
#include <array>
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

/// The formats the tool encodes, by name. The number a block carries is not
/// here: it is the position of the format's decoder in the Project's list,
/// which the end of Assemble decides.
constexpr std::array<Format, 3> FORMATS{ { { .name = "copy", .encode = &encodeCopy },
                                           { .name = "rle", .encode = &encodeRle },
                                           { .name = "zx0", .encode = &encodeZx0 } } };

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

/// Where a Symbol of a Module is, by name.
std::optional<std::uint32_t> symbolIndexOf( Module const& module, std::string_view name )
{
  std::vector<Symbol> const& symbols = module.symbols().symbols();
  for ( std::uint32_t index = 0; index < symbols.size(); ++index )
  {
    if ( symbols[index].name == name )
    {
      return index;
    }
  }
  return std::nullopt;
}

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
      std::optional<std::uint32_t> const symbol = symbolIndexOf( modules[index], declaration.label );
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
                    .at( request.span.begin, request.span.length )
                    .arg( "transform", request.transform ) );
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
  std::vector<SectionRef> payloads;
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

  // What one form may come to, where a driver is promised that nothing crosses
  // a unit: the widest unit there is, since that is the ceiling an author is
  // held to and the placement will find it. Zero where images may cross, and
  // then nothing is ever cut.
  Target const& target = build.project().target;
  std::uint32_t ceiling = 0;
  if ( build.project().driver.has_value() && build.project().driver->spansNothing )
  {
    for ( std::uint32_t unit = 0; target.bankSize() > 0 && unit * target.bankSize() < target.storageSize(); ++unit )
    {
      ceiling = std::max( ceiling, target.usableInUnit( unit ) );
    }
  }
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
      std::span<std::uint8_t const> const content = bytes.of( where );
      std::span<std::uint8_t const> const initialised =
          content.subspan( std::min<std::size_t>( extent.begin, content.size() ),
                           std::min<std::size_t>( extent.size(), content.size() - extent.begin ) );
      storage.store( where, id, piecesOf( format, initialised, extent.begin, ceiling ) );
    }
  }
}

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
  // through, and which state is its base, are the Target's; the driver has
  // been resolved by now, so both are known where this text is written.
  std::string source = "; The dispatcher over the decoders the program declared, in the order the\n"
                       "; tool numbered them. See docs/spec/transition.md.\n";
  std::optional<std::string> streamWindow;
  if ( project.driver.has_value() && project.driver->stream.has_value() )
  {
    streamWindow = project.target.windows[project.driver->stream->value].name;
  }
  bool const xex = project.container == Container::XEX;
  std::vector<WindowIndex> const order = baseOrderOf( project );

  // The bases the entered Phase gives every Window, which the routine calls
  // for at the end of every Transition: read from the Frame, one byte per
  // Window in the order the Frame lists them, and shown through `showAt` —
  // the stream's Window last, since showing its base takes the stream away.
  // Without a driver there is nothing to show, and the routine is not built.
  source += ".export ngaShowBases\n.proc ngaShowBases\n";
  if ( project.driver.has_value() )
  {
    for ( WindowIndex const window : order )
    {
      source += "        nga.read\n        tax\n        nga.showAt " + project.target.windows[window.value].name + "\n";
    }
  }
  source += "        rts\n.endp\n";

  // Base memory as the program starts: the entry Phase's base in every
  // Window, known where this text is written, which a Container that loads
  // through memory calls last — a Root then, since nothing in the program
  // names it and the routine may be dropped.
  source += ".export ngaRestore\n.proc ngaRestore";
  source += xex ? ", root\n" : "\n";
  if ( project.driver.has_value() )
  {
    for ( WindowIndex const window : order )
    {
      std::optional<std::uint32_t> const base = baseIn( project, project.phases.entry, window );
      if ( base.has_value() )
      {
        source +=
            "        nga.show " + project.target.windows[window.value].name + ", " + std::to_string( *base ) + "\n";
      }
    }
  }
  source += "        rts\n.endp\n";

  // The glue a Container that loads through memory calls: a cell it writes
  // a state's index into, and INITAD at the Proc that hands it to `showAt`.
  // Roots, since nothing in the program names them, and only for the
  // Container that does: a raw image would carry the resident bytes for
  // nothing, and a Root where the program declared none would turn Prune on.
  // Without a Window the stream reads through there is nothing to fill, and
  // the Container says so when it is written.
  if ( xex && streamWindow.has_value() )
  {
    source += ".export ngaLoadUnit, ngaLoadMap\n"
              ".section root\n"
              "ngaLoadUnit\n"
              "        .res 1\n"
              ".ends\n"
              ".proc ngaLoadMap, root\n"
              "        ldx ngaLoadUnit\n"
              "        nga.showAt " +
              *streamWindow +
              "\n"
              "        rts\n"
              ".endp\n";
  }
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
  diag::FileId const file = sources.addFile( "<nga>/transforms.asm", std::move( source ) );
  ModuleIndex const index{ static_cast<std::uint32_t>( modules.size() ) };
  diag::DiagnosticSink found{ sink.policy() };
  modules.push_back( assembleModule( sources, file, "nga.transforms", Residency::all( phaseCount ), found ) );
  modules.back().keepOutsideWindow();
  sink.merge( std::move( found ) );

  project.modules.push_back( ProjectModule{ .name = "nga.transforms",
                                            .file = file,
                                            .residency = Residency::all( phaseCount ),
                                            .generated = Generated::NONE,
                                            .outsideWindow = true } );
  for ( Phase& phase : project.phases.phases )
  {
    if ( std::ranges::find( phase.needs, index ) == phase.needs.end() )
    {
      phase.needs.push_back( index );
    }
  }
}

void addDriverInit( diag::SourceManager& sources,
                    Project& project,
                    std::vector<Module>& modules,
                    diag::DiagnosticSink& sink )
{
  if ( !project.driver.has_value() || !project.driver->init.has_value() )
  {
    return;
  }

  // A Module of its own and not the dispatcher's, because the dispatcher is
  // built only for a Project with Transitions and this is wanted without
  // one: a program that draws a frame and loops has no edge to take and still
  // has to reach the hardware before its first `.with`. A Root, since nothing
  // in the program names it — the Container calls it by address — and outside
  // the Window, as every generated Proc that the driver's macros land in is.
  std::size_t const phaseCount = project.phases.phases.size();
  std::string source = "; The driver's `init`, wrapped so that a Container has an address to call.\n"
                       "; See docs/spec/transition.md and docs/spec/xex.md.\n"
                       ".export ngaInit\n"
                       ".proc ngaInit, root\n"
                       "        nga.init\n"
                       "        rts\n"
                       ".endp\n";
  diag::FileId const file = sources.addFile( "<nga>/init.asm", std::move( source ) );
  ModuleIndex const index{ static_cast<std::uint32_t>( modules.size() ) };
  diag::DiagnosticSink found{ sink.policy() };
  modules.push_back( assembleModule( sources, file, "nga.init", Residency::all( phaseCount ), found ) );
  modules.back().keepOutsideWindow();
  sink.merge( std::move( found ) );

  project.modules.push_back( ProjectModule{ .name = "nga.init",
                                            .file = file,
                                            .residency = Residency::all( phaseCount ),
                                            .generated = Generated::NONE,
                                            .outsideWindow = true } );
  for ( Phase& phase : project.phases.phases )
  {
    if ( std::ranges::find( phase.needs, index ) == phase.needs.end() )
    {
      phase.needs.push_back( index );
    }
  }
}

} // namespace nga::model
