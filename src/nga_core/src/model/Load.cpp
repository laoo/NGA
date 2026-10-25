#include "nga/model/Load.hpp"

#include "nga/model/Place.hpp"
#include "nga/model/Prune.hpp"
#include "nga/model/Segments.hpp"
#include "nga/model/Transform.hpp"
#include "nga/model/Transition.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nga::model
{

namespace
{

/// The Label a decoder's `.transform FORMAT LABEL` line names, read as the
/// assembler reads a line: words apart by any blank, and nothing after a `;`.
/// Empty where no such line is there.
std::string decoderLabelIn( std::string_view text, std::string_view format )
{
  constexpr std::string_view blanks = " \t\r";
  while ( !text.empty() )
  {
    std::size_t const end = text.find( '\n' );
    std::string_view line = text.substr( 0, end );
    text = end == std::string_view::npos ? std::string_view{} : text.substr( end + 1 );
    line = line.substr( 0, line.find( ';' ) );
    std::vector<std::string_view> words;
    while ( words.size() < 4 )
    {
      std::size_t const begin = line.find_first_not_of( blanks );
      if ( begin == std::string_view::npos )
      {
        break;
      }
      line.remove_prefix( begin );
      std::size_t const after = std::min( line.find_first_of( blanks ), line.size() );
      words.push_back( line.substr( 0, after ) );
      line.remove_prefix( after );
    }
    if ( words.size() == 3 && words[0] == ".transform" && words[1] == format )
    {
      return std::string{ words[2] };
    }
  }
  return {};
}

/// The Namespace `nga.load` holds its decoders in.
constexpr std::string_view LOAD_DECODERS_NAMESPACE = "ngaLoad";

/// The first Label of each Section of the move `loadModuleSource` writes,
/// which the load places only where bytes hide under a ROM. Named exactly, so
/// that a decoder's own Label is never taken for one of them.
constexpr std::array<std::string_view, 8> MOVE_LABELS{
  "ngaUnder",   "ngaUnderTable", "ngaUnderAt",    "ngaUnderFrom",
  "ngaUnderTo", "ngaUnderLeft",  "ngaUnderCount", "ngaUnderRoms",
};

/// `free` less `cut`, both sorted and disjoint ranges.
void subtract( std::vector<AddressRange>& free, AddressRange cut )
{
  std::vector<AddressRange> left;
  for ( AddressRange const& range : free )
  {
    if ( cut.end <= range.begin || cut.begin >= range.end )
    {
      left.push_back( range );
      continue;
    }
    if ( range.begin < cut.begin )
    {
      left.push_back( AddressRange{ .begin = range.begin, .end = cut.begin } );
    }
    if ( cut.end < range.end )
    {
      left.push_back( AddressRange{ .begin = cut.end, .end = range.end } );
    }
  }
  free = std::move( left );
}

/// The first address of `wanted` that no range of `free` covers, or nothing
/// where all of it is free.
std::optional<std::uint32_t> firstNotIn( std::vector<AddressRange> const& free, AddressRange wanted )
{
  std::uint32_t at = wanted.begin;
  while ( at < wanted.end )
  {
    auto const covering =
        std::ranges::find_if( free, [at]( AddressRange const& range ) { return range.begin <= at && at < range.end; } );
    if ( covering == free.end() )
    {
      return at;
    }
    at = covering->end;
  }
  return std::nullopt;
}

/// What the entry Phase holds that the OS or the hardware uses while the
/// loader runs: its `root` reservations, the OS's variables and its ROM among
/// them.
std::vector<AddressRange> heldByTheMachine( Placed const& build )
{
  Project const& project = build.project();
  Layout const& layout = build.layout();
  Sizes const& sizes = build.sizes();
  std::vector<AddressRange> held;
  for ( std::uint32_t module = 0; module < build.modules().size(); ++module )
  {
    Module const& one = build.modules()[module];
    for ( std::uint32_t index = 0; index < one.sections().size(); ++index )
    {
      Section const& section = one.sections()[index];
      SectionRef const where{ .module = ModuleIndex{ module }, .section = SectionIndex{ index } };
      if ( !section.isRoot() || section.emitsBytes() || !layout.isPlaced( where ) || !sizes.isKnown( where ) ||
           !heldAcrossOf( build, where ).includes( project.phases.entry ) )
      {
        continue;
      }
      std::uint32_t const at = layout.addressOf( where );
      held.push_back( AddressRange{ .begin = at, .end = at + sizes.sizeOfSection( where ) } );
    }
  }
  return held;
}

/// The pool a placement class draws from, less what the machine holds: what
/// the entry Phase keeps of it, and what the `load` block says it holds while
/// the file loads — see
/// docs/decisions/0246-the-load-of-a-xex-needs-what-the-machine-holds-then.md.
std::vector<AddressRange> freeAtLoad( Placed const& build, syntax::PlacementClass placement )
{
  Pools const& pools = build.target().pools;
  std::vector<AddressRange> free = placement == syntax::PlacementClass::ZEROPAGE ? pools.zeroPage : pools.general;
  for ( AddressRange const& held : heldByTheMachine( build ) )
  {
    subtract( free, held );
  }
  for ( HeldWhileLoading const& held : heldWhileLoading( build ) )
  {
    subtract( free, held.range );
  }
  // What of the load's code a Holder holds into a Phase is written first and
  // stays: nothing the load writes after it stands on it — see
  // docs/decisions/0247-the-hardware-holds-a-section-for-as-long-as-it-is-given-it.md.
  for ( ModuleIndex const module : build.project().loadNeeds )
  {
    for ( std::uint32_t index = 0; index < build.modules()[module.value].sections().size(); ++index )
    {
      SectionRef const where{ .module = module, .section = SectionIndex{ index } };
      // And a Section in a Pane or a unit set stands at the medium's address,
      // which is no CPU memory.
      if ( isMachineAtLoad( build, where ) || placedByTheLoad( build, where ) || !build.layout().isPlaced( where ) ||
           !build.sizes().isKnown( where ) || build.modules()[module.value].sectionAt( where.section ).inMedium() )
      {
        continue;
      }
      std::uint32_t const at = build.layout().addressOf( where );
      subtract( free, AddressRange{ .begin = at, .end = at + build.sizes().sizeOfSection( where ) } );
    }
  }
  // And `nga.choose`, which is written before the first `init` and read until
  // the last segment — see
  // docs/decisions/0302-a-document-may-be-chosen-while-a-xex-loads.md.
  if ( std::optional<ModuleIndex> const choose = build.project().chooseModule; choose.has_value() )
  {
    for ( std::uint32_t index = 0; index < build.modules()[choose->value].sections().size(); ++index )
    {
      SectionRef const where{ .module = *choose, .section = SectionIndex{ index } };
      if ( build.layout().isPlaced( where ) && build.sizes().isKnown( where ) )
      {
        std::uint32_t const at = build.layout().addressOf( where );
        subtract( free, AddressRange{ .begin = at, .end = at + build.sizes().sizeOfSection( where ) } );
      }
    }
  }
  return free;
}

/// The lowest start in `free` at which `size` bytes fit, a multiple of
/// `alignment` and, with a boundary, not crossing one.
std::optional<std::uint32_t> lowestStart( std::vector<AddressRange> const& free,
                                          std::uint32_t size,
                                          std::uint32_t alignment,
                                          std::uint32_t boundary )
{
  for ( AddressRange const& range : free )
  {
    for ( std::uint32_t at = ( range.begin + alignment - 1 ) / alignment * alignment; at + size <= range.end;
          at += alignment )
    {
      if ( boundary == 0 || size == 0 || at / boundary == ( at + size - 1 ) / boundary )
      {
        return at;
      }
    }
  }
  return std::nullopt;
}

/// Which Sections of a Module another's `then` names, and so begin no chain.
std::vector<bool> followedIn( Module const& module )
{
  std::vector<bool> followed( module.sections().size(), false );
  for ( Section const& section : module.sections() )
  {
    if ( std::optional<SectionIndex> const next = section.next(); next.has_value() )
    {
      followed[next->value] = true;
    }
  }
  return followed;
}

/// The memory free while the file loads, for each of the two placement
/// classes a Section of the load may have.
struct FreeAtLoad
{
  std::vector<AddressRange> general;
  std::vector<AddressRange> zeroPage;

  [[nodiscard]] std::vector<AddressRange>& of( syntax::PlacementClass placement )
  {
    return placement == syntax::PlacementClass::ZEROPAGE ? zeroPage : general;
  }

  void subtractRange( AddressRange cut )
  {
    subtract( general, cut );
    subtract( zeroPage, cut );
  }
};

/// Places the `then` chain `head` begins as Place would, end to end and only
/// what is reachable and sized of it: in the pool of free bytes its placement
/// takes from, at its pin or at the lowest start its alignment and boundary
/// allow, every pool then the shorter by it. The pools are tried in order and
/// each is a part of the one after it, so a chain takes the first that has
/// room. What it placed goes into `placed`.
void placeChain( Placed const& build,
                 Layout& layout,
                 SectionRef head,
                 std::span<FreeAtLoad> tiers,
                 std::vector<SectionRef>& placed,
                 diag::DiagnosticSink& sink )
{
  Sizes const& sizes = build.sizes();
  Module const& one = build.modules()[head.module.value];
  std::vector<SectionRef> chain;
  std::uint32_t size = 0;
  for ( std::optional<SectionIndex> at = head.section; at.has_value(); at = one.sections()[at->value].next() )
  {
    SectionRef const where{ .module = head.module, .section = *at };
    if ( build.reachable().includes( where ) && sizes.isKnown( where ) )
    {
      chain.push_back( where );
      size += sizes.sizeOfSection( where );
    }
  }
  if ( chain.empty() )
  {
    return;
  }
  auto const declared = [&build, &head]( syntax::Expression const* value ) -> std::optional<std::int64_t>
  {
    if ( value == nullptr )
    {
      return std::nullopt;
    }
    return declaredValueOf( build.sources(), build.symbols(), &build.charsets(), head.module, *value );
  };
  Section const& section = one.sectionAt( head.section );
  std::optional<std::uint32_t> start;
  if ( std::optional<std::int64_t> const pin = declared( section.pinnedAddress() ); pin.has_value() )
  {
    start = static_cast<std::uint32_t>( *pin );
  }
  else
  {
    auto const alignment = static_cast<std::uint32_t>( declared( section.alignment() ).value_or( 1 ) );
    auto const boundary = static_cast<std::uint32_t>( declared( section.boundary() ).value_or( 0 ) );
    for ( FreeAtLoad& tier : tiers )
    {
      start = lowestStart( tier.of( section.placement() ), size, std::max<std::uint32_t>( alignment, 1 ), boundary );
      if ( start.has_value() )
      {
        break;
      }
    }
  }
  if ( !start.has_value() )
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::LOAD_NO_ROOM )
                  .arg( "section", one.displayNameOf( head.section ) )
                  .arg( "size", static_cast<std::int64_t>( size ) )
                  .sortedBy( "load" ) );
    return;
  }
  std::uint32_t at = *start;
  for ( FreeAtLoad& tier : tiers )
  {
    subtract( tier.of( section.placement() ), AddressRange{ .begin = at, .end = at + size } );
  }
  for ( SectionRef const where : chain )
  {
    layout.place( where, at );
    at += sizes.sizeOfSection( where );
    placed.push_back( where );
  }
}

/// The program's own code for the load, from the Modules the `load` block
/// needs: placed in what is free while the file loads, pins where they stand,
/// each `then` chain end to end. It runs before anything else of the file is
/// written, so what is written after may stand over it — see
/// docs/decisions/0246-the-load-of-a-xex-needs-what-the-machine-holds-then.md.
/// What nothing the file writes while it loads stands on: free then, and no
/// Section with bytes, no Window, no form a Section decodes from and nothing
/// of `nga.load` there — nor a Section without bytes that what runs while the
/// file loads reaches: the load's own code, the glue the loader calls and the
/// driver, which may write it. A Section without bytes nothing of those
/// reaches is written by nothing until the program runs. Where `nga.choose`
/// goes, since it is written before the first `init` and called until the last
/// segment — see
/// docs/decisions/0302-a-document-may-be-chosen-while-a-xex-loads.md.
FreeAtLoad keptAtLoad( Placed const& build, Storage const& storage )
{
  FreeAtLoad kept{ .general = freeAtLoad( build, syntax::PlacementClass::ABSOLUTE ),
                   .zeroPage = freeAtLoad( build, syntax::PlacementClass::ZEROPAGE ) };
  Layout const& layout = build.layout();
  Sizes const& sizes = build.sizes();
  Project const& project = build.project();

  std::vector<SectionRef> running;
  for ( std::uint32_t module = 0; module < build.modules().size(); ++module )
  {
    bool const loads = std::ranges::find( project.loadNeeds, ModuleIndex{ module } ) != project.loadNeeds.end() ||
                       ( project.driver.has_value() && project.driver->module == ModuleIndex{ module } );
    for ( std::uint32_t index = 0; loads && index < build.modules()[module].sections().size(); ++index )
    {
      running.push_back( SectionRef{ .module = ModuleIndex{ module }, .section = SectionIndex{ index } } );
    }
  }
  Reachable const touched = reachedFrom( build, running );
  Reachable const called = calledByTheLoader( build );

  for ( std::uint32_t module = 0; module < build.modules().size(); ++module )
  {
    Module const& one = build.modules()[module];
    for ( std::uint32_t index = 0; index < one.sections().size(); ++index )
    {
      SectionRef const where{ .module = ModuleIndex{ module }, .section = SectionIndex{ index } };
      Section const& section = one.sections()[index];
      if ( ModuleIndex{ module } == project.chooseModule || !layout.isPlaced( where ) || !sizes.isKnown( where ) ||
           section.inMedium() )
      {
        continue;
      }
      if ( section.emitsBytes() || ModuleIndex{ module } == project.loadModule || touched.includes( where ) ||
           called.includes( where ) )
      {
        std::uint32_t const at = layout.addressOf( where );
        kept.subtractRange( AddressRange{ .begin = at, .end = at + sizes.sizeOfSection( where ) } );
      }
    }
  }
  for ( Window const& window : build.target().windows )
  {
    for ( AddressRange const& range : window.ranges )
    {
      kept.subtractRange( range );
    }
  }
  for ( Storage::LoadForm const& form : storage.loadForms() )
  {
    kept.subtractRange(
        AddressRange{ .begin = form.to, .end = form.at + static_cast<std::uint32_t>( form.form.size() ) } );
  }
  // The vectors the file writes, and the stack whatever the loader calls runs
  // on.
  kept.subtractRange( AddressRange{ .begin = RUNAD, .end = INITAD + 2 } );
  kept.subtractRange( AddressRange{ .begin = 0x100, .end = 0x200 } );
  return kept;
}

/// `nga.choose`, each `then` chain end to end, in what nothing the file writes
/// while it loads stands on.
void placeChooseModule( Placed const& build,
                        Layout& layout,
                        Storage const& storage,
                        std::vector<SectionRef>& placed,
                        diag::DiagnosticSink& sink )
{
  std::optional<ModuleIndex> const choose = build.project().chooseModule;
  if ( !choose.has_value() )
  {
    return;
  }
  std::array<FreeAtLoad, 1> kept{ keptAtLoad( build, storage ) };
  Module const& module = build.modules()[choose->value];
  std::vector<bool> const followed = followedIn( module );
  for ( std::uint32_t head = 0; head < module.sections().size(); ++head )
  {
    if ( !followed[head] )
    {
      placeChain( build, layout, SectionRef{ .module = *choose, .section = SectionIndex{ head } }, kept, placed, sink );
    }
  }
}

std::vector<SectionRef> placeLoadCode( Placed const& build, Layout& layout, diag::DiagnosticSink& sink )
{
  std::vector<SectionRef> placed;
  std::array<FreeAtLoad, 1> free{ FreeAtLoad{ .general = freeAtLoad( build, syntax::PlacementClass::ABSOLUTE ),
                                              .zeroPage = freeAtLoad( build, syntax::PlacementClass::ZEROPAGE ) } };
  for ( ModuleIndex const module : build.project().loadNeeds )
  {
    Module const& one = build.modules()[module.value];
    std::vector<bool> const followed = followedIn( one );
    for ( std::uint32_t head = 0; head < one.sections().size(); ++head )
    {
      SectionRef const first{ .module = module, .section = SectionIndex{ head } };
      if ( followed[head] || !placedByTheLoad( build, first ) )
      {
        continue;
      }
      placeChain( build, layout, first, free, placed, sink );
    }
  }
  return placed;
}

} // namespace

std::vector<std::uint8_t> storedFormAtLoad( std::vector<std::uint8_t> const& encoded )
{
  std::vector<std::uint8_t> form{ static_cast<std::uint8_t>( encoded.size() & 0xFFU ),
                                  static_cast<std::uint8_t>( ( encoded.size() >> 8U ) & 0xFFU ) };
  form.insert( form.end(), encoded.begin(), encoded.end() );
  return form;
}

std::vector<AddressRange> writableAtLoad( Placed const& build )
{
  // A Window is RAM a `.xex` writes the units through, and in no pool.
  std::vector<AddressRange> writable = freeAtLoad( build, syntax::PlacementClass::ABSOLUTE );
  for ( AddressRange const& range : freeAtLoad( build, syntax::PlacementClass::ZEROPAGE ) )
  {
    writable.push_back( range );
  }
  for ( Window const& window : build.target().windows )
  {
    writable.insert( writable.end(), window.ranges.begin(), window.ranges.end() );
  }
  std::ranges::sort( writable, {}, &AddressRange::begin );
  std::vector<AddressRange> joined;
  for ( AddressRange const& range : writable )
  {
    if ( !joined.empty() && range.begin <= joined.back().end )
    {
      joined.back().end = std::max( joined.back().end, range.end );
    }
    else
    {
      joined.push_back( range );
    }
  }
  // The stack, which whatever the loader calls runs on.
  subtract( joined, AddressRange{ .begin = 0x100, .end = 0x200 } );
  return joined;
}

void transformAtLoad( Placed const& build, Storage& storage, Bytes const& bytes, diag::DiagnosticSink& sink )
{
  Project const& project = build.project();
  // With storage a transformed Section has a Payload, and `nga.load` is there
  // only for a compressed file.
  if ( !project.loadModule.has_value() || build.target().hasStorage() )
  {
    return;
  }
  Layout const& layout = build.layout();
  Sizes const& sizes = build.sizes();
  for ( std::uint32_t module = 0; module < build.modules().size(); ++module )
  {
    Module const& one = build.modules()[module];
    for ( std::uint32_t index = 0; index < one.sections().size(); ++index )
    {
      Section const& section = one.sections()[index];
      SectionRef const where{ .module = ModuleIndex{ module }, .section = SectionIndex{ index } };
      if ( section.format() == TRANSFORM_COPY || !section.emitsBytes() || !build.reachable().includes( where ) ||
           !layout.isPlaced( where ) || !sizes.isKnown( where ) )
      {
        continue;
      }
      // A Pane's bytes go into its Bank as they are, and a unit set's into
      // the set, and nothing decodes there.
      if ( section.inMedium() )
      {
        std::string place;
        if ( std::optional<PaneIndex> const pane = section.pane(); pane.has_value() )
        {
          place = build.target().panes[pane->value].name;
        }
        else if ( std::optional<UnitSetIndex> const set = section.unitSet(); set.has_value() )
        {
          place = build.target().unitSets[set->value].name;
        }
        sink.add( diag::diagnostic( diag::DiagnosticId::TRANSFORM_IN_PANE )
                      .at( section.span().begin, section.span().length )
                      .arg( "section", one.displayNameOf( SectionIndex{ index } ) )
                      .arg( "module", std::string{ one.name() } )
                      .arg( "pane", place ) );
        continue;
      }
      Format const& format = formats()[section.format()];
      // Bytes under a ROM are decoded there by the move, not in place.
      if ( storage.isHiddenAtLoad( where ) )
      {
        continue;
      }
      InitialisedExtent const extent = sizes.initialisedExtentOf( where );
      std::vector<std::uint8_t> const form = storedFormAtLoad( format.encode( contentOf( bytes, where, extent ) ) );
      std::optional<std::uint32_t> const margin = format.margin != nullptr ? format.margin( form ) : std::nullopt;
      std::string const name = one.displayNameOf( SectionIndex{ index } );
      if ( !margin.has_value() || form.size() >= extent.size() )
      {
        sink.add( diag::diagnostic( diag::DiagnosticId::LOAD_FORM_SAVES_NOTHING )
                      .at( section.span().begin, section.span().length )
                      .arg( "section", name )
                      .arg( "format", std::string{ format.name } )
                      .arg( "form", static_cast<std::int64_t>( form.size() ) )
                      .arg( "size", static_cast<std::int64_t>( extent.size() ) ) );
        continue;
      }
      // Read out once it is known to be there: GCC 14 takes a later `*margin`
      // of an optional made by `?:` for a read of nothing.
      std::uint32_t const past = *margin;

      // The form ends `margin` bytes past the extent, and what stands there
      // while the loader runs has to be its to write: in a pool, and nothing
      // the machine holds. Bytes written later, and a Section decoded later
      // from a form loaded after this one is done, are no obstacle.
      std::uint32_t const to = layout.addressOf( where ) + extent.begin;
      std::uint32_t const end = to + extent.size();
      std::vector<AddressRange> const free = freeAtLoad( build, section.placement() );
      if ( std::optional<std::uint32_t> const blocked =
               firstNotIn( free, AddressRange{ .begin = end, .end = end + past } );
           blocked.has_value() )
      {
        sink.add( diag::diagnostic( diag::DiagnosticId::LOAD_FORM_NO_MARGIN )
                      .at( section.span().begin, section.span().length )
                      .arg( "section", name )
                      .arg( "margin", static_cast<std::int64_t>( past ) )
                      .arg( "address", static_cast<std::int64_t>( *blocked ) ) );
        continue;
      }
      std::uint32_t const top = end + past;
      storage.decodeAtLoad( Storage::LoadForm{ .where = where,
                                               .form = form,
                                               .margin = past,
                                               .format = std::string{ format.name },
                                               .at = top - static_cast<std::uint32_t>( form.size() ),
                                               .to = to } );
    }
  }
}

std::string loadModuleSource( std::span<DecoderSource const> decoders )
{
  // The move always, since what lies hidden under a ROM is Place's answer,
  // which no Project file knows. Placed only where the load uses it — see
  // docs/decisions/0241-bytes-kept-for-a-later-phase-may-lie-hidden-under-a-rom.md.

  // `nga.read` in this Module is this macro, whatever the driver says: the
  // form stands in memory, where the loader put it, and not in any stream.
  // Y is not kept, as no driver's `read` keeps it.
  std::string source = "; What a `.xex` decodes with while it loads: without storage its transformed\n"
                       "; Sections, and compressed its segments. See\n"
                       "; docs/decisions/0235-a-xex-decodes-in-place-while-it-loads.md and\n"
                       "; docs/decisions/0259-a-xex-may-be-compressed-segment-by-segment.md.\n"
                       "ngaLoadPtr .ztemp 2\n"
                       "\n"
                       ".macro ngaLoadRead\n"
                       "        ldy #0\n"
                       "        lda (ngaLoadPtr),y\n"
                       "        inc ngaLoadPtr\n"
                       "        bne @same\n"
                       "        inc ngaLoadPtr+1\n"
                       "@same\n"
                       ".endm\n"
                       "\n"
                       "; Where the loader puts the form it has just written, and the destination.\n"
                       ".export ngaUnpackFrom, ngaUnpackTo\n"
                       ".section root\n"
                       "ngaUnpackFrom .res 2\n"
                       "ngaUnpackTo   .res 2\n"
                       ".ends\n"
                       "\n"
                       "; What lies hidden under a ROM is written into memory nothing holds yet with a\n"
                       "; table in front of it — how many pieces, the ROMs to put back, then where each\n"
                       "; is, where it goes and how long it is — and moved with every ROM out and the\n"
                       "; interrupts masked. See\n"
                       "; docs/decisions/0241-bytes-kept-for-a-later-phase-may-lie-hidden-under-a-rom.md.\n"
                       ".export ngaUnder, ngaUnderTable, ngaUnderPutBack\n"
                       ".section root\n"
                       "ngaUnderTable .res 2\n"
                       ".ends\n"
                       ".section root\n"
                       "ngaUnderPutBack .res 1\n"
                       ".ends\n"
                       "ngaUnderAt    .ztemp 2\n"
                       "ngaUnderFrom  .ztemp 2\n"
                       "ngaUnderTo    .ztemp 2\n"
                       "ngaUnderLeft  .ztemp 2\n"
                       "ngaUnderCount .ztemp 1\n"
                       "ngaUnderRoms  .ztemp 1\n"
                       ".proc ngaUnder, root\n"
                       "        nga.mask\n"
                       "        lda #0\n"
                       "        nga.select\n"
                       "        lda ngaUnderTable\n"
                       "        sta ngaUnderAt\n"
                       "        lda ngaUnderTable+1\n"
                       "        sta ngaUnderAt+1\n"
                       "        ldy #0\n"
                       "        lda (ngaUnderAt),y\n"
                       "        sta ngaUnderCount\n"
                       "        iny\n"
                       "        lda (ngaUnderAt),y\n"
                       "        sta ngaUnderRoms\n"
                       "        ldx #2\n"
                       "piece   jsr take\n"
                       "        sta ngaUnderFrom\n"
                       "        jsr take\n"
                       "        sta ngaUnderFrom+1\n"
                       "        jsr take\n"
                       "        sta ngaUnderTo\n"
                       "        jsr take\n"
                       "        sta ngaUnderTo+1\n"
                       "        jsr take\n"
                       "        sta ngaUnderLeft\n"
                       "        jsr take\n"
                       "        sta ngaUnderLeft+1\n"
                       "        ldy #0\n"
                       "move    lda ngaUnderLeft\n"
                       "        ora ngaUnderLeft+1\n"
                       "        beq moved\n"
                       "        lda (ngaUnderFrom),y\n"
                       "        sta (ngaUnderTo),y\n"
                       "        inc ngaUnderFrom\n"
                       "        bne from\n"
                       "        inc ngaUnderFrom+1\n"
                       "from    inc ngaUnderTo\n"
                       "        bne to\n"
                       "        inc ngaUnderTo+1\n"
                       "to      lda ngaUnderLeft\n"
                       "        bne left\n"
                       "        dec ngaUnderLeft+1\n"
                       "left    dec ngaUnderLeft\n"
                       "        jmp move\n"
                       "moved   dec ngaUnderCount\n"
                       "        bne piece\n"
                       "        lda ngaUnderRoms\n"
                       "        nga.select\n"
                       "        nga.unmask\n"
                       "        rts\n"
                       "\n"
                       "; The next byte of the table, which X walks from its third.\n"
                       "take    txa\n"
                       "        tay\n"
                       "        lda (ngaUnderAt),y\n"
                       "        inx\n"
                       "        rts\n"
                       ".endp\n";
  for ( DecoderSource const& file : decoders )
  {
    std::string_view const format = file.format;
    // The decoder's own label, from its `.transform` line, which the glue
    // jumps to.
    std::string const label = decoderLabelIn( file.text, format );
    if ( label.empty() )
    {
      continue;
    }
    std::string const glue = loadGlueName( format );
    // In a Namespace of its own, since a program with storage holds the same
    // text resident and a Proc's name is the whole program's — see
    // docs/decisions/0259-a-xex-may-be-compressed-segment-by-segment.md.
    std::string const decoder = std::string{ LOAD_DECODERS_NAMESPACE } + "." + label;
    source += "\n; ";
    source += file.path;
    source += "\n.namespace ";
    source += LOAD_DECODERS_NAMESPACE;
    source += '\n';
    source += file.text;
    source += "\n.endnamespace\n";
    source += "\n.export ";
    source += glue;
    source += "\n.proc ";
    source += glue;
    source += ", root\n"
              "        lda ngaUnpackFrom\n"
              "        sta ngaLoadPtr\n"
              "        lda ngaUnpackFrom+1\n"
              "        sta ngaLoadPtr+1\n"
              "        ldx ngaUnpackTo\n"
              "        ldy ngaUnpackTo+1\n"
              "        jmp ";
    source += decoder;
    source += "\n.endp\n";

    // The same decoder writing under a ROM: every ROM out and the interrupts
    // masked around it, and the ROMs the entry Phase has put back after — see
    // docs/decisions/0241-bytes-kept-for-a-later-phase-may-lie-hidden-under-a-rom.md.
    std::string const under = underGlueName( format );
    source += "\n.export ";
    source += under;
    source += "\n.proc ";
    source += under;
    source += ", root\n"
              "        nga.mask\n"
              "        lda #0\n"
              "        nga.select\n"
              "        lda ngaUnpackFrom\n"
              "        sta ngaLoadPtr\n"
              "        lda ngaUnpackFrom+1\n"
              "        sta ngaLoadPtr+1\n"
              "        ldx ngaUnpackTo\n"
              "        ldy ngaUnpackTo+1\n"
              "        jsr ";
    source += decoder;
    source += "\n"
              "        lda ngaUnderPutBack\n"
              "        nga.select\n"
              "        nga.unmask\n"
              "        rts\n"
              ".endp\n";
  }

  return source;
}

std::string chooseModuleSource( std::size_t choices )
{
  // A Choice past the last is no Choice, and changes nothing either.
  std::string const count = std::to_string( choices );
  std::string const entry = std::to_string( choices + 1 );
  return "; What a `.xex` built from a `choose` block chooses with and changes itself\n"
         "; with while it loads. The file writes the list once, before anything runs,\n"
         "; and calls `ngaPatch` with the two words naming the entries written since\n"
         "; the last call. See\n"
         "; docs/decisions/0302-a-document-may-be-chosen-while-a-xex-loads.md.\n"
         ".export ngaChoice, ngaPatch, ngaPatchFrom, ngaPatchTo\n"
         ".section root\n"
         "ngaChoice .byte 0\n"
         ".ends\n"
         ".section root\n"
         "ngaPatchFrom .res 2\n"
         "ngaPatchTo   .res 2\n"
         ".ends\n"
         "ngaPatchAt   .ztemp 2\n"
         "ngaPatchHere .ztemp 2\n"
         ".proc ngaPatch, root\n"
         "        lda ngaChoice\n"
         "        beq done\n"
         "        cmp #" +
         count +
         "\n"
         "        bcs done\n"
         "        lda ngaPatchFrom\n"
         "        sta ngaPatchAt\n"
         "        lda ngaPatchFrom+1\n"
         "        sta ngaPatchAt+1\n"
         "next    lda ngaPatchAt\n"
         "        cmp ngaPatchTo\n"
         "        bne entry\n"
         "        lda ngaPatchAt+1\n"
         "        cmp ngaPatchTo+1\n"
         "        beq done\n"
         "entry   ldy #0\n"
         "        lda (ngaPatchAt),y\n"
         "        sta ngaPatchHere\n"
         "        iny\n"
         "        lda (ngaPatchAt),y\n"
         "        sta ngaPatchHere+1\n"
         "\n"
         "; The byte of Choice k stands k + 1 bytes into its entry.\n"
         "        ldy ngaChoice\n"
         "        iny\n"
         "        lda (ngaPatchAt),y\n"
         "        ldy #0\n"
         "        sta (ngaPatchHere),y\n"
         "        clc\n"
         "        lda ngaPatchAt\n"
         "        adc #" +
         entry +
         "\n"
         "        sta ngaPatchAt\n"
         "        bcc next\n"
         "        inc ngaPatchAt+1\n"
         "        jmp next\n"
         "done    rts\n"
         ".endp\n";
}

std::vector<AddressRange> keptWhileLoading( Placed const& build, Storage const& storage )
{
  return keptAtLoad( build, storage ).general;
}

Reachable calledByTheLoader( Placed const& build )
{
  GlobalSymbols const& symbols = build.symbols();
  std::vector<SectionRef> glue;
  for ( std::string_view const name : { INIT_NAME, LOAD_MAP_NAME } )
  {
    if ( std::optional<SymbolRef> const where = symbols.find( name ); where.has_value() )
    {
      if ( std::optional<SectionRef> const section = sectionNamedBy( symbols, *where ); section.has_value() )
      {
        glue.push_back( *section );
      }
    }
  }
  return reachedFrom( build, glue );
}

void placeLoad( Placed const& build, Layout& layout, Storage const& storage, Bytes& bytes, diag::DiagnosticSink& sink )
{
  std::optional<ModuleIndex> const load = build.project().loadModule;
  // What this load runs: the move where bytes hide under a ROM, the decode
  // under a ROM where one of them is carried encoded, and the decoders where
  // anything is decoded at all, which a compressed file always may be.
  bool const hides = !storage.hiddenAtLoad().empty();
  bool const decodesUnder =
      std::ranges::any_of( storage.hiddenAtLoad(), []( Storage::Hidden const& one ) { return !one.form.empty(); } );
  bool const compresses = build.project().compress.has_value();
  bool const decodes = !storage.loadForms().empty() || decodesUnder || compresses;
  std::vector<SectionRef> placed = placeLoadCode( build, layout, sink );
  placeChooseModule( build, layout, storage, placed, sink );
  if ( !load.has_value() || ( !decodes && !hides ) )
  {
    patchSections( build, storage, placed, bytes, sink );
    return;
  }
  Sizes const& sizes = build.sizes();
  FreeAtLoad free{ .general = freeAtLoad( build, syntax::PlacementClass::ABSOLUTE ),
                   .zeroPage = freeAtLoad( build, syntax::PlacementClass::ZEROPAGE ) };
  for ( Storage::LoadForm const& form : storage.loadForms() )
  {
    free.subtractRange(
        AddressRange{ .begin = form.to, .end = form.at + static_cast<std::uint32_t>( form.form.size() ) } );
  }
  auto const sectionRange = [&build, &sizes]( SectionRef where ) -> std::optional<AddressRange>
  {
    if ( !build.layout().isPlaced( where ) || !sizes.isKnown( where ) )
    {
      return std::nullopt;
    }
    std::uint32_t const at = build.layout().addressOf( where );
    return AddressRange{ .begin = at, .end = at + sizes.sizeOfSection( where ) };
  };
  // Where bytes hide under a ROM, the driver's Module is written before the
  // move that calls it, so it stands in memory while `nga.load` runs.
  PhaseIndex const entry = build.project().phases.entry;
  if ( hides && build.project().driver.has_value() )
  {
    ModuleIndex const driver = build.project().driver->module;
    Module const& holder = build.modules()[driver.value];
    for ( std::uint32_t index = 0; index < holder.sections().size() && holder.residency().includes( entry ); ++index )
    {
      if ( std::optional<AddressRange> const taken =
               sectionRange( { .module = driver, .section = SectionIndex{ index } } );
           taken.has_value() )
      {
        free.subtractRange( *taken );
      }
    }
  }

  // A compressed file decodes into the Windows while it fills the units, after
  // what the loader calls is written, and the rest of the start after both —
  // so the decoder keeps off the first two, and where it can off the third,
  // whose bytes under it are written last and as they are. See
  // docs/decisions/0259-a-xex-may-be-compressed-segment-by-segment.md. What
  // `except` names is written with the first, and kept off as it is.
  std::vector<FreeAtLoad> tiers;
  if ( compresses )
  {
    for ( Window const& window : build.target().windows )
    {
      for ( AddressRange const& range : window.ranges )
      {
        free.subtractRange( range );
      }
    }
    Reachable const called = calledByTheLoader( build );
    FreeAtLoad preferred = free;
    for ( std::uint32_t module = 0; module < build.modules().size(); ++module )
    {
      Module const& one = build.modules()[module];
      for ( std::uint32_t index = 0; index < one.sections().size(); ++index )
      {
        SectionRef const where{ .module = ModuleIndex{ module }, .section = SectionIndex{ index } };
        Section const& section = one.sections()[index];
        if ( ModuleIndex{ module } == *load || !section.emitsBytes() || section.inMedium() ||
             !build.reachable().includes( where ) || !heldAcrossOf( build, where ).includes( entry ) )
        {
          continue;
        }
        // What `except` names is written early and as it is, so nothing can
        // be written over it last — see
        // docs/decisions/0260-a-compressed-xex-writes-what-it-is-told-as-it-is.md.
        bool const early =
            called.includes( where ) ||
            std::ranges::find( build.project().uncompressed, where.module ) != build.project().uncompressed.end();
        if ( std::optional<AddressRange> const taken = sectionRange( where ); taken.has_value() )
        {
          preferred.subtractRange( *taken );
          if ( early )
          {
            free.subtractRange( *taken );
          }
        }
      }
    }
    tiers.push_back( std::move( preferred ) );
  }
  tiers.push_back( std::move( free ) );

  // A Proc that falls into the one its `then` names is placed with it, the
  // chain end to end, as Place would: a decoder is written that way.
  Module const& module = build.modules()[load->value];
  std::vector<bool> const followed = followedIn( module );
  for ( std::uint32_t head = 0; head < module.sections().size(); ++head )
  {
    if ( followed[head] )
    {
      continue;
    }
    // What this load uses of the Module: the move and its cells where bytes
    // hide under a ROM, the decoders and theirs where a Section is decoded.
    std::string const name = module.displayNameOf( SectionIndex{ head } );
    bool const underGlue = std::ranges::any_of( build.project().transforms,
                                                [&name]( TransformRequest const& request )
                                                { return name == underGlueName( request.transform ); } );
    bool used = decodes;
    if ( underGlue || name == UNDER_PUT_BACK_NAME )
    {
      used = decodesUnder;
    }
    else if ( std::ranges::find( MOVE_LABELS, name ) != MOVE_LABELS.end() )
    {
      used = hides;
    }
    if ( used )
    {
      placeChain( build, layout, SectionRef{ .module = *load, .section = SectionIndex{ head } }, tiers, placed, sink );
    }
  }
  patchSections( build, storage, placed, bytes, sink );
}

void findHiddenAtLoad( Placed const& build, Storage& storage, Bytes const& bytes, diag::DiagnosticSink& sink )
{
  if ( !canHide( build ) )
  {
    return;
  }
  Project const& project = build.project();
  Layout const& layout = build.layout();
  Sizes const& sizes = build.sizes();
  PhaseIndex const entry = project.phases.entry;

  // What a ROM the driver switches covers while the entry Phase has it in.
  std::vector<AddressRange> roms;
  for ( std::uint32_t module = 0; module < build.modules().size(); ++module )
  {
    Module const& one = build.modules()[module];
    if ( !isSwitchedRom( build, ModuleIndex{ module } ) || !one.residency().includes( entry ) )
    {
      continue;
    }
    for ( std::uint32_t index = 0; index < one.sections().size(); ++index )
    {
      SectionRef const where{ .module = ModuleIndex{ module }, .section = SectionIndex{ index } };
      if ( layout.isPlaced( where ) && sizes.isKnown( where ) )
      {
        std::uint32_t const at = layout.addressOf( where );
        roms.push_back( AddressRange{ .begin = at, .end = at + sizes.sizeOfSection( where ) } );
      }
    }
  }
  if ( roms.empty() )
  {
    return;
  }

  for ( std::uint32_t module = 0; module < build.modules().size(); ++module )
  {
    Module const& one = build.modules()[module];
    if ( project.loadModule == ModuleIndex{ module } || isSwitchedRom( build, ModuleIndex{ module } ) )
    {
      continue;
    }
    for ( std::uint32_t index = 0; index < one.sections().size(); ++index )
    {
      Section const& section = one.sections()[index];
      SectionRef const where{ .module = ModuleIndex{ module }, .section = SectionIndex{ index } };
      if ( !section.emitsBytes() || section.inMedium() || !build.reachable().includes( where ) ||
           !layout.isPlaced( where ) || !sizes.isKnown( where ) || !heldAcrossOf( build, where ).includes( entry ) )
      {
        continue;
      }
      InitialisedExtent const extent = sizes.initialisedExtentOf( where );
      std::uint32_t const to = layout.addressOf( where ) + extent.begin;
      AddressRange const range{ .begin = to, .end = to + extent.size() };
      if ( extent.size() == 0 ||
           std::ranges::none_of( roms, [&range]( AddressRange const& rom ) { return range.overlaps( rom ); } ) )
      {
        continue;
      }
      std::span<std::uint8_t const> const content = contentOf( bytes, where, extent );
      Storage::Hidden hidden{ .where = where,
                              .to = to,
                              .bytes = std::vector<std::uint8_t>( content.begin(), content.end() ) };

      // Named in a `transform`, it is carried encoded and decoded under the
      // ROM, not in place, so no margin is asked of it; one whose form saves
      // nothing is moved as it is, as 0235 has it.
      if ( section.format() != TRANSFORM_COPY )
      {
        Format const& format = formats()[section.format()];
        std::vector<std::uint8_t> form = storedFormAtLoad( format.encode( content ) );
        if ( form.size() >= extent.size() )
        {
          sink.add( diag::diagnostic( diag::DiagnosticId::LOAD_FORM_SAVES_NOTHING )
                        .at( section.span().begin, section.span().length )
                        .arg( "section", one.displayNameOf( SectionIndex{ index } ) )
                        .arg( "format", std::string{ format.name } )
                        .arg( "form", static_cast<std::int64_t>( form.size() ) )
                        .arg( "size", static_cast<std::int64_t>( extent.size() ) ) );
        }
        else
        {
          hidden.form = std::move( form );
          hidden.format = std::string{ format.name };
        }
      }
      storage.hideAtLoad( std::move( hidden ) );
    }
  }
}

void stageHiddenAtLoad( Placed const& build, Storage& storage, diag::DiagnosticSink& sink )
{
  std::span<Storage::Hidden const> const hidden = storage.hiddenAtLoad();
  std::optional<ModuleIndex> const load = build.project().loadModule;
  if ( hidden.empty() || !load.has_value() )
  {
    return;
  }
  Layout const& layout = build.layout();
  Sizes const& sizes = build.sizes();

  // Free while the program loads, less what `nga.load` itself stands on,
  // since the move runs from there. Nothing of the program is written yet, so
  // the copies may stand wherever its bytes will later go.
  // And less the driver's Module, which the move calls to switch the ROMs and
  // which the `.xex` therefore writes before it.
  std::vector<AddressRange> free = freeAtLoad( build, syntax::PlacementClass::ABSOLUTE );
  PhaseIndex const entryPhase = build.project().phases.entry;
  for ( std::optional<ModuleIndex> const holder :
        { load, build.project().driver.has_value() ? std::optional{ build.project().driver->module } : std::nullopt } )
  {
    if ( !holder.has_value() )
    {
      continue;
    }
    Module const& module = build.modules()[holder->value];
    for ( std::uint32_t index = 0; index < module.sections().size(); ++index )
    {
      SectionRef const where{ .module = *holder, .section = SectionIndex{ index } };
      bool const present = holder == load || module.residency().includes( entryPhase );
      if ( present && layout.isPlaced( where ) && sizes.isKnown( where ) )
      {
        std::uint32_t const at = layout.addressOf( where );
        subtract( free, AddressRange{ .begin = at, .end = at + sizes.sizeOfSection( where ) } );
      }
    }
  }
  auto const largest = std::ranges::max_element( free, {}, []( AddressRange const& range ) { return range.size(); } );

  // A table of a count and the ROMs to put back, then six bytes a piece: X
  // walks it in `ngaUnder`, so no batch has more pieces than its byte reaches.
  constexpr std::uint32_t header = 2;
  constexpr std::uint32_t entry = 6;
  constexpr std::uint32_t mostPieces = ( 0xFF - header ) / entry;
  if ( largest == free.end() || largest->size() <= header + entry )
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::LOAD_NO_STAGING ).sortedBy( "load" ) );
    return;
  }
  std::uint32_t const room = largest->size();
  auto const roms = static_cast<std::uint8_t>( romsOf( build.project(), build.project().phases.entry ) );

  // A Section carried encoded is written whole and decoded under the ROM, one
  // at a time, the copy written over by the next; one whose form is larger
  // than the stretch is moved as it is.
  std::vector<Storage::Hidden const*> raw;
  for ( Storage::Hidden const& one : hidden )
  {
    if ( one.form.empty() )
    {
      raw.push_back( &one );
      continue;
    }
    if ( one.form.size() <= room )
    {
      storage.decodeHidden(
          Storage::HiddenForm{ .at = largest->begin, .to = one.to, .form = one.form, .format = one.format } );
      continue;
    }
    Module const& owner = build.modules()[one.where.module.value];
    Section const& section = owner.sectionAt( one.where.section );
    sink.add( diag::diagnostic( diag::DiagnosticId::LOAD_HIDDEN_AS_IT_IS )
                  .at( section.span().begin, section.span().length )
                  .arg( "section", owner.displayNameOf( one.where.section ) )
                  .arg( "format", one.format )
                  .arg( "form", static_cast<std::int64_t>( one.form.size() ) )
                  .arg( "room", static_cast<std::int64_t>( room ) ) );
    raw.push_back( &one );
  }

  struct Piece
  {
    std::uint32_t to = 0;
    std::span<std::uint8_t const> bytes;
    SectionRef where{};
  };

  std::size_t section = 0;
  std::size_t offset = 0;
  while ( section < raw.size() )
  {
    // As many pieces as fit, the last of them cut where the room ends.
    std::vector<Piece> pieces;
    std::uint32_t used = header;
    while ( section < raw.size() && pieces.size() < mostPieces && used + entry < room )
    {
      Storage::Hidden const& one = *raw[section];
      std::size_t const left = one.bytes.size() - offset;
      std::size_t const fits = std::min<std::size_t>( left, room - used - entry );
      pieces.push_back( Piece{ .to = one.to + static_cast<std::uint32_t>( offset ),
                               .bytes = std::span{ one.bytes }.subspan( offset, fits ),
                               .where = one.where } );
      used += entry + static_cast<std::uint32_t>( fits );
      offset += fits;
      if ( offset == one.bytes.size() )
      {
        ++section;
        offset = 0;
      }
    }
    std::uint32_t const at = largest->begin;
    std::vector<std::uint8_t> image{ static_cast<std::uint8_t>( pieces.size() ), roms };
    std::uint32_t from = at + header + ( entry * static_cast<std::uint32_t>( pieces.size() ) );
    std::vector<Storage::HiddenBatch::Piece> carried;
    carried.reserve( pieces.size() );
    for ( Piece const& piece : pieces )
    {
      auto const size = static_cast<std::uint32_t>( piece.bytes.size() );
      for ( std::uint32_t const word : { from, piece.to, size } )
      {
        image.push_back( static_cast<std::uint8_t>( word & 0xFFU ) );
        image.push_back( static_cast<std::uint8_t>( ( word >> 8U ) & 0xFFU ) );
      }
      carried.push_back( Storage::HiddenBatch::Piece{
          .where = piece.where, .offset = piece.to - layout.addressOf( piece.where ), .at = from, .size = size } );
      from += size;
    }
    for ( Piece const& piece : pieces )
    {
      image.insert( image.end(), piece.bytes.begin(), piece.bytes.end() );
    }
    std::vector<SectionRef> holds;
    holds.reserve( pieces.size() );
    for ( Piece const& piece : pieces )
    {
      holds.push_back( piece.where );
    }
    storage.stageHidden( Storage::HiddenBatch{
        .at = at, .image = std::move( image ), .holds = std::move( holds ), .pieces = std::move( carried ) } );
  }
}

} // namespace nga::model
