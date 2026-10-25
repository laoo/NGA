#include "nga/model/VerifyLayout.hpp"

#include "nga/model/Prune.hpp"
#include "nga/model/Trace.hpp"

#include "nga/model/Evaluate.hpp"
#include "nga/model/Place.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace nga::model
{

namespace
{

/// One past the highest address a 6502 can name. The general pool's end is not
/// this: a Target may make a pool small, and a Section outside a small pool is
/// outside its pool and not off the end of the machine.
constexpr std::uint32_t MEMORY_END = 0x10000;

/// One Section as this file needs to see it: where the Layout put it, how far
/// it reaches, and what its source asked for.
///
/// Read from the model and never from Place's own working state, which is the
/// point of the exercise — a Claim carried over from the allocator would carry
/// its mistakes with it.
struct PlacedSection
{
  SectionRef where;
  diag::SourceSpan span;
  std::uint32_t size = 0;
  syntax::PlacementClass placement = syntax::PlacementClass::ABSOLUTE;
  Residency const* residency = nullptr;
  std::optional<std::uint32_t> pinned;
  std::uint32_t alignment = 1;
  std::uint32_t boundary = 0;
  bool payload = false;
  std::optional<SectionRef> next;

  /// The Pane the Section is in, and the address space its state is — see
  /// addressSpaceOf; nothing for a Section in `fixed`. A family's Section is
  /// in as many spaces as the family has members, one a Bank from the first,
  /// which is how the solver holds it apart.
  std::optional<PaneIndex> pane{};
  std::optional<std::uint32_t> space{};
  std::uint32_t spaces = 1;

  /// The unit set it stands in as the medium sees it, at a linear address —
  /// see docs/decisions/0254-a-section-may-stand-in-a-unit-set.md.
  std::optional<UnitSetIndex> units{};

  /// Where the Section stands in each Phase it is held across, at its one
  /// address, or one entry for a Section in no Phase, which stands somewhere
  /// all the same.
  struct Stand
  {
    std::optional<PhaseIndex> phase;
    AddressRange range;
  };

  std::vector<Stand> stands;
};

/// The last address a range covers. Every message says where a Section *ends*,
/// and a half-open end is one past that.
std::uint32_t lastOf( AddressRange range )
{
  return range.end - 1;
}

/// Whether two Sections stand in an address space both are in: `fixed` with
/// `fixed`, or Banks the two Panes' runs share.
bool shareASpace( PlacedSection const& one, PlacedSection const& other )
{
  if ( !one.space.has_value() || !other.space.has_value() )
  {
    return one.space == other.space;
  }
  return *one.space < *other.space + other.spaces && *other.space < *one.space + one.spaces;
}

} // namespace

void verifyLayout( Placed const& build, Storage const& storage, ReadOnly const& readOnly, diag::DiagnosticSink& sink )
{
  diag::SourceManager const& sources = build.sources();
  GlobalSymbols const& symbols = build.symbols();
  Sizes const& sizes = build.sizes();
  Layout const& layout = build.layout();
  Charsets const& charsets = build.charsets();
  PhaseGraph const& phases = build.phases();
  Target const& target = build.target();
  std::span<Module const> const modules = symbols.modules();

  auto const nameOf = [&symbols]( SectionRef where )
  { return symbols.moduleAt( where.module ).displayNameOf( where.section ); };
  auto const phaseName = [&phases]( std::optional<PhaseIndex> phase )
  { return phase.has_value() ? phases.phases[phase->value].displayName() : std::string{ "(none)" }; };

  std::vector<HeldWhileLoading> const machineAtLoad = heldWhileLoading( build );

  std::vector<PlacedSection> placed;
  for ( std::uint32_t module = 0; module < modules.size(); ++module )
  {
    Module const& one = modules[module];
    for ( std::uint32_t index = 0; index < one.sections().size(); ++index )
    {
      Section const& section = one.sections()[index];
      SectionRef const where{ .module = ModuleIndex{ module }, .section = SectionIndex{ index } };

      // What decodes or runs while a `.xex` loads is placed after this, by
      // the load Step, in memory nothing else needs at that moment.
      if ( placedByTheLoad( build, where ) )
      {
        continue;
      }

      // What Prune dropped has no address, and what it kept has one.
      if ( !build.reachable().includes( where ) )
      {
        if ( layout.isPlaced( where ) )
        {
          sink.add( diag::diagnostic( diag::DiagnosticId::LAYOUT_UNREACHABLE_PLACED )
                        .at( section.span().begin, section.span().length )
                        .arg( "section", nameOf( where ) ) );
        }
        continue;
      }
      if ( !layout.isPlaced( where ) )
      {
        sink.add( diag::diagnostic( diag::DiagnosticId::LAYOUT_NOT_PLACED )
                      .at( section.span().begin, section.span().length )
                      .arg( "section", nameOf( where ) ) );
        continue;
      }

      PlacedSection entry{ .where = where,
                           .span = section.span(),
                           .size = sizes.isKnown( where ) ? sizes.sizeOfSection( where ) : 0,
                           .placement = section.placement(),
                           .residency = &one.residency(),
                           .pinned = std::nullopt,
                           .alignment = 1,
                           .boundary = 0,
                           .payload = storage.hasPayload( where ),
                           .next = std::nullopt,
                           .pane = section.pane(),
                           .space = section.pane().has_value() ? addressSpaceOf( target, layout, *section.pane() )
                                                               : std::nullopt,
                           .spaces = section.pane().has_value() ? target.panes[section.pane()->value].count : 1U,
                           .units = section.unitSet(),
                           .stands = {} };
      // A unit set's linear addresses are a space of their own.
      if ( entry.units.has_value() )
      {
        entry.space = 0x20000000U | entry.units->value;
      }
      if ( std::optional<SectionIndex> const next = section.next(); next.has_value() )
      {
        entry.next = SectionRef{ .module = where.module, .section = *next };
      }

      // Where it stands, Phase by Phase: in every Phase its bytes are held
      // across, which for bytes nothing restores is more than its Residency.
      Residency const held = heldAcrossOf( build, where );
      for ( std::uint32_t phase = 0; phase < phases.phases.size(); ++phase )
      {
        if ( held.includes( PhaseIndex{ phase } ) )
        {
          std::uint32_t const at = layout.addressOf( where );
          entry.stands.push_back( PlacedSection::Stand{
              .phase = PhaseIndex{ phase }, .range = AddressRange{ .begin = at, .end = at + entry.size } } );
        }
      }
      if ( entry.stands.empty() )
      {
        std::uint32_t const at = layout.addressOf( where );
        entry.stands.push_back( PlacedSection::Stand{ .phase = std::nullopt,
                                                      .range = AddressRange{ .begin = at, .end = at + entry.size } } );
      }

      // The pin, the alignment and the boundary come from the declaration,
      // evaluated again here. Taking them from Place would ask the solver
      // whether it did what the solver decided to do.
      if ( section.pinnedAddress() != nullptr )
      {
        std::optional<std::int64_t> const address =
            declaredValueOf( sources, symbols, &charsets, where.module, *section.pinnedAddress() );
        std::int64_t const end = entry.units.has_value() ? std::int64_t{ 1 } << 32 : 0x10000;
        if ( address.has_value() && *address >= 0 && *address < end )
        {
          entry.pinned = static_cast<std::uint32_t>( *address );
        }
      }
      if ( section.alignment() != nullptr )
      {
        std::optional<std::int64_t> const value =
            declaredValueOf( sources, symbols, &charsets, where.module, *section.alignment() );
        if ( value.has_value() && *value >= 1 && *value <= 0x10000 )
        {
          entry.alignment = static_cast<std::uint32_t>( *value );
        }
      }
      if ( section.boundary() != nullptr )
      {
        std::optional<std::int64_t> const value =
            declaredValueOf( sources, symbols, &charsets, where.module, *section.boundary() );
        if ( value.has_value() && *value >= 1 && *value <= 0x10000 )
        {
          entry.boundary = static_cast<std::uint32_t>( *value );
        }
      }

      placed.push_back( std::move( entry ) );
    }
  }

  for ( PlacedSection const& one : placed )
  {
    // Every rule about one address, at every address the Section stands at
    // — once per distinct address, so a Section held still is checked once.
    std::vector<std::uint32_t> seen;
    for ( PlacedSection::Stand const& stand : one.stands )
    {
      if ( std::ranges::find( seen, stand.range.begin ) != seen.end() )
      {
        continue;
      }
      seen.push_back( stand.range.begin );
      AddressRange const range = stand.range;

      auto const report = [&one, &nameOf, range]( diag::DiagnosticId id )
      {
        return diag::diagnostic( id )
            .at( one.span.begin, one.span.length )
            .arg( "section", nameOf( one.where ) )
            .arg( "address", range.begin );
      };

      if ( std::optional<std::uint32_t> const pinned = one.pinned; pinned.has_value() && range.begin != *pinned )
      {
        sink.add( report( diag::DiagnosticId::LAYOUT_PIN_MOVED ).arg( "pinned", *pinned ) );
      }

      if ( range.begin % one.alignment != 0 )
      {
        sink.add( report( diag::DiagnosticId::LAYOUT_NOT_ALIGNED ).arg( "alignment", one.alignment ) );
      }

      // A Section of no bytes covers no address, so every rule below is about
      // one that does.
      if ( range.begin == range.end )
      {
        continue;
      }

      if ( one.boundary > 0 && range.begin / one.boundary != lastOf( range ) / one.boundary )
      {
        sink.add( report( diag::DiagnosticId::LAYOUT_CROSSES_BOUNDARY )
                      .arg( "last", lastOf( range ) )
                      .arg( "boundary", one.boundary ) );
      }

      // In a unit set the rest is about the CPU's memory, and what holds of
      // a linear address is that it lies within the set's Banks.
      if ( one.units.has_value() )
      {
        std::optional<AddressRange> const banks = target.linearBanksOf( *one.units );
        if ( banks.has_value() && ( range.begin < banks->begin || range.end > banks->end ) )
        {
          sink.add( report( diag::DiagnosticId::LAYOUT_OUTSIDE_POOL ).arg( "last", lastOf( range ) ) );
        }
        continue;
      }

      if ( range.end > MEMORY_END )
      {
        sink.add( report( diag::DiagnosticId::LAYOUT_PAST_MEMORY ) );
        continue;
      }

      if ( one.placement == syntax::PlacementClass::ZEROPAGE && range.end > ZERO_PAGE_END )
      {
        sink.add( report( diag::DiagnosticId::LAYOUT_NOT_ZERO_PAGE ).arg( "last", lastOf( range ) ) );
      }

      // What the `.xex` writes while it loads stands on nothing the machine
      // holds then, unless the author pinned it there — see
      // docs/decisions/0246-the-load-of-a-xex-needs-what-the-machine-holds-then.md.
      if ( !one.pane.has_value() && !one.pinned.has_value() &&
           ( !stand.phase.has_value() || *stand.phase == phases.entry ) && writtenWhileLoading( build, one.where ) )
      {
        for ( HeldWhileLoading const& machine : machineAtLoad )
        {
          if ( range.overlaps( machine.range ) )
          {
            sink.add(
                report( diag::DiagnosticId::LAYOUT_HELD_WHILE_LOADING ).arg( "holder", nameOf( machine.holder ) ) );
            break;
          }
        }
      }

      if ( one.payload && !one.pane.has_value() &&
           std::ranges::any_of( target.streamRanges,
                                [&range]( AddressRange const& window ) { return range.overlaps( window ); } ) )
      {
        sink.add( report( diag::DiagnosticId::LAYOUT_PAYLOAD_IN_WINDOW ).arg( "last", lastOf( range ) ) );
      }

      // What is in no Pane stands outside every Window — see
      // docs/decisions/0229-no-window-has-a-base.md.
      if ( !one.pane.has_value() )
      {
        for ( Window const& window : target.windows )
        {
          if ( window.meets( range ) )
          {
            sink.add( report( diag::DiagnosticId::LAYOUT_IN_WINDOW ).arg( "window", window.name ) );
          }
        }
      }

      // Only what the solver placed. A pin outside the pool is the author's
      // Constraint and the solver is obliged to satisfy it, not to refuse it.
      // A Pane's Section has its Window for a pool.
      if ( !one.pinned.has_value() )
      {
        std::span<AddressRange const> pool =
            one.placement == syntax::PlacementClass::ZEROPAGE ? target.pools.zeroPage : target.pools.general;
        if ( one.pane.has_value() )
        {
          pool = target.windows[target.panes[one.pane->value].window.value].ranges;
        }
        // A Section that stands in ROM is allocated from the ROM pool, so that
        // is the pool it is held to — read here from the same answer Place
        // read, and not from the address it was given, which would make the
        // check ask itself.
        else if ( readOnly.standsInRom( one.where ) )
        {
          pool = target.pools.readOnly;
        }
        // And within one of the Regions its `in` named, if it named any.
        std::span<AddressRange const> const regions =
            build.modules()[one.where.module.value].sectionAt( one.where.section ).regions();
        auto const within = [&range]( AddressRange const& part )
        { return range.begin >= part.begin && range.end <= part.end; };
        bool const inside =
            std::ranges::any_of( pool, within ) && ( regions.empty() || std::ranges::any_of( regions, within ) );
        if ( !inside )
        {
          sink.add( report( diag::DiagnosticId::LAYOUT_OUTSIDE_POOL ).arg( "last", lastOf( range ) ) );
        }
      }
    }
  }

  // A Proc another falls through into starts where that one ends, in every
  // Phase both stand in.
  for ( PlacedSection const& one : placed )
  {
    if ( !one.next.has_value() )
    {
      continue;
    }
    auto const following =
        std::ranges::find_if( placed, [&one]( PlacedSection const& other ) { return other.where == *one.next; } );
    if ( following == placed.end() )
    {
      continue;
    }
    for ( PlacedSection::Stand const& stand : one.stands )
    {
      for ( PlacedSection::Stand const& after : following->stands )
      {
        if ( after.phase != stand.phase || after.range.begin == stand.range.end )
        {
          continue;
        }
        sink.add( diag::diagnostic( diag::DiagnosticId::LAYOUT_NOT_FOLLOWING )
                      .at( one.span.begin, one.span.length )
                      .arg( "section", nameOf( one.where ) )
                      .arg( "address", stand.range.end )
                      .arg( "phase", phaseName( stand.phase ) )
                      .arg( "next", nameOf( following->where ) )
                      .arg( "other", after.range.begin ) );
        break;
      }
    }
  }

  // The co-visibility rule as its definition rather than as a search: for
  // every Phase, every pair of Sections it holds, once, with nothing skipped
  // for standing anywhere in particular in a list — and a pair reported once
  // however many Phases it collides in. This is the loop Place cannot afford
  // and does not need to.
  std::vector<std::pair<std::size_t, std::size_t>> reported;
  for ( std::uint32_t phase = 0; phase < phases.phases.size(); ++phase )
  {
    PhaseIndex const here{ phase };
    auto const standIn = [here]( PlacedSection const& one ) -> AddressRange const*
    {
      for ( PlacedSection::Stand const& stand : one.stands )
      {
        if ( stand.phase == here )
        {
          return &stand.range;
        }
      }
      return nullptr;
    };
    for ( std::size_t index = 0; index < placed.size(); ++index )
    {
      AddressRange const* const one = standIn( placed[index] );
      if ( one == nullptr || one->begin == one->end )
      {
        continue;
      }
      for ( std::size_t other = index + 1; other < placed.size(); ++other )
      {
        AddressRange const* const two = standIn( placed[other] );
        // Two states of one Window are never visible together, so two
        // Sections in different address spaces do not collide.
        if ( two == nullptr || two->begin == two->end || !one->overlaps( *two ) ||
             !shareASpace( placed[index], placed[other] ) ||
             std::ranges::find( reported, std::pair{ index, other } ) != reported.end() ||
             build.interference().mayShare( placed[index].where, placed[other].where ) ||
             hidesUnder( build, placed[index].where, placed[other].where ) ||
             hidesUnder( build, placed[other].where, placed[index].where ) )
        {
          continue;
        }
        reported.emplace_back( index, other );
        sink.add( diag::diagnostic( diag::DiagnosticId::LAYOUT_OVERLAP )
                      .at( placed[index].span.begin, placed[index].span.length )
                      .arg( "section", nameOf( placed[index].where ) )
                      .arg( "other", nameOf( placed[other].where ) )
                      .arg( "address", std::max( one->begin, two->begin ) )
                      .arg( "phase", phaseName( here ) ) );
      }
    }

    // A Section in a unit set against a Pane's of the same set, at the
    // linear address the Pane's stands at in each member of its run.
    for ( std::size_t index = 0; index < placed.size(); ++index )
    {
      AddressRange const* const one = standIn( placed[index] );
      if ( one == nullptr || one->begin == one->end || !placed[index].units.has_value() )
      {
        continue;
      }
      for ( std::size_t other = 0; other < placed.size(); ++other )
      {
        AddressRange const* const two = standIn( placed[other] );
        std::optional<PaneIndex> const pane = placed[other].pane;
        if ( two == nullptr || two->begin == two->end || !pane.has_value() ||
             target.unitSetShownBy( target.panes[pane->value].window ) != placed[index].units )
        {
          continue;
        }
        std::optional<std::uint32_t> const state = layout.stateOfPane( *pane );
        std::optional<std::uint32_t> const linear =
            state.has_value() ? target.linearAddressOf( *pane, *state, two->begin ) : std::nullopt;
        std::optional<std::uint32_t> const bank = target.bankSizeOf( *placed[index].units );
        if ( !linear.has_value() || !bank.has_value() )
        {
          continue;
        }
        // Read out once they are known to be there: GCC 14 takes a later read
        // of an optional made by `?:` for a read of nothing.
        std::uint32_t const start = *linear;
        std::uint32_t const span = *bank;
        for ( std::uint32_t member = 0; member < target.panes[pane->value].count; ++member )
        {
          AddressRange const there{ .begin = start + ( member * span ),
                                    .end = start + ( member * span ) + two->size() };
          if ( !one->overlaps( there ) || std::ranges::find( reported, std::pair{ index, other } ) != reported.end() )
          {
            continue;
          }
          reported.emplace_back( index, other );
          sink.add( diag::diagnostic( diag::DiagnosticId::LAYOUT_OVERLAP )
                        .at( placed[index].span.begin, placed[index].span.length )
                        .arg( "section", nameOf( placed[index].where ) )
                        .arg( "other", nameOf( placed[other].where ) )
                        .arg( "address", std::max( one->begin, there.begin ) )
                        .arg( "phase", phaseName( here ) ) );
        }
      }
    }
  }
}

} // namespace nga::model
