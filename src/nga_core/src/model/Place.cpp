#include "nga/model/Place.hpp"

#include "nga/model/Prune.hpp"

#include "nga/model/Solver.hpp"
#include "nga/model/Trace.hpp"
#include "nga/model/TypeCheck.hpp"

#include <algorithm>
#include <cstddef>
#include <functional>
#include <ranges>
#include <span>
#include <string>
#include <utility>

namespace nga::model
{

namespace
{

/// A Section waiting to be placed, with what Place knows about it before the
/// solver is asked: in Project order, then declaration order within a Module.
struct Candidate
{
  SectionRef where;
  std::uint32_t size = 0;
  PlacementClass placement = PlacementClass::ABSOLUTE;
  diag::SourceSpan span;
  std::optional<std::uint32_t> pinned;

  /// What `align` and `within` declared, as a Claim carries them.
  std::uint32_t alignment = 1;
  std::uint32_t boundary = 0;

  /// Refused where it was declared — larger than its boundary — and so
  /// neither pinned nor handed to the solver, though it still weighs what it
  /// weighs in the sums.
  bool impossible = false;

  /// A start per Phase rather than one — see
  /// docs/decisions/0030-movable-sections.md.
  bool movable = false;

  /// A Temporary, which may share an address with another the Interference
  /// allows — see docs/decisions/0034-trace.md.
  bool temporary = false;

  /// The Proc this one falls through into, placed immediately after it.
  std::optional<SectionRef> next;

  /// The Module's, which its Sections inherit. Outlives the Candidate: it is
  /// the Module's own and the Modules outlive Place.
  Residency const* residency = nullptr;

  /// The Phases the Section's bytes are held apart in: its Residency, or for
  /// a Pane's Section with bytes — written once at load and never again —
  /// the Phases it is live across, see liveAcross. What every Claim and pin
  /// is asked against.
  Residency live{};

  /// Whether this Section may not live in the Window: a Transition loads it
  /// from a Bank, so that while the Bank is switched in an address there is
  /// the Bank — or it is the routine that does the switching.
  bool outsideWindow = false;

  /// The Pane the Section is in, when it is in one: its pool is then the
  /// Pane's Window, and its address space either the named state the Pane
  /// is pinned to — `group` — or the Bank the solver gives the Pane, asked
  /// for as the PaneClaim at `paneClaim` — see docs/decisions/0054-panes.md.
  std::optional<PaneIndex> pane{};
  std::optional<std::uint32_t> group{};
  std::optional<std::size_t> paneClaim{};

  /// The ranges the Section may not stand in: the stream's, where a
  /// Transition needs it, and every Window a `.with` shows a state of while
  /// it runs or is named — see docs/decisions/0055-with.md. Nothing for a
  /// Pane's Section, whose Window is its pool.
  std::vector<AddressRange> keepOut{};
  std::vector<WindowIndex> keepOutWindows{};
};

/// The pins accepted so far, kept sorted so that the one an overlap names is
/// the lowest in memory rather than the first that happened to be looked at.
class AcceptedPins
{
public:
  /// The pin already holding any part of this range in a Phase the claimant
  /// is present in and in the same state of memory, or nothing.
  [[nodiscard]] std::optional<SectionRef>
  overlapping( AddressRange range, Residency const& residency, std::optional<std::uint32_t> group ) const
  {
    for ( Pin const& pin : mPins )
    {
      if ( pin.range.begin < range.end && range.begin < pin.range.end && pin.group == group &&
           pin.residency->intersects( residency ) )
      {
        return pin.owner;
      }
    }
    return std::nullopt;
  }

  void accept( AddressRange range, SectionRef owner, Residency const& residency, std::optional<std::uint32_t> group )
  {
    mPins.push_back( Pin{ .range = range, .owner = owner, .residency = &residency, .group = group } );
    for ( std::size_t index = mPins.size() - 1; index > 0; --index )
    {
      if ( mPins[index - 1].range.begin <= mPins[index].range.begin )
      {
        break;
      }
      std::swap( mPins[index - 1], mPins[index] );
    }
  }

private:
  struct Pin
  {
    AddressRange range;
    SectionRef owner;
    Residency const* residency = nullptr;
    std::optional<std::uint32_t> group;
  };

  std::vector<Pin> mPins;
};

std::string nameOfSection( Merged const& build, SectionRef where )
{
  return build.symbols().moduleAt( where.module ).displayNameOf( where.section, build.sources() );
}

std::vector<Candidate> candidatesOf( Sized const& build, Storage const& storage, diag::DiagnosticSink& sink )
{
  Sizes const& sizes = build.sizes();
  std::vector<Candidate> candidates;
  for ( std::uint32_t module = 0; module < build.modules().size(); ++module )
  {
    Module const& one = build.modules()[module];
    for ( std::uint32_t index = 0; index < one.sections().size(); ++index )
    {
      Section const& section = one.sections()[index];
      SectionRef const where{ .module = ModuleIndex{ module }, .section = SectionIndex{ index } };
      if ( !build.reachable().includes( where ) )
      {
        continue;
      }

      Candidate candidate{ .where = where,
                           .size = sizes.isKnown( where ) ? sizes.sizeOfSection( where ) : 0,
                           .placement = section.placement(),
                           .span = section.span(),
                           .pinned = std::nullopt,
                           .next = std::nullopt,
                           .residency = &one.residency(),
                           .outsideWindow = storage.hasPayload( where ) || one.outsideWindow(),
                           .pane = section.pane(),
                           .group = std::nullopt,
                           .keepOut = {},
                           .keepOutWindows = {} };
      candidate.movable = section.isMovable();
      candidate.live = candidate.pane.has_value() && section.emitsBytes()
                           ? liveAcross( build.phases(), one.residency() )
                           : one.residency();
      if ( !candidate.pane.has_value() )
      {
        if ( candidate.outsideWindow )
        {
          candidate.keepOut.insert(
              candidate.keepOut.end(), build.target().streamRanges.begin(), build.target().streamRanges.end() );
        }
        for ( WindowIndex const window : build.freezes().keptOutOf( where ) )
        {
          candidate.keepOutWindows.push_back( window );
          Window const& shown = build.target().windows[window.value];
          candidate.keepOut.insert( candidate.keepOut.end(), shown.ranges.begin(), shown.ranges.end() );
        }
        // A Section the solver places inside a Window is in the Window's
        // base state as the variant declares it; a Phase that gives the
        // Window another base does not show it, so a Section present in such
        // a Phase stands outside — see docs/decisions/0056-a-phase-chooses-a-base.md.
        // A pin says where the Section is, and presence follows.
        if ( section.pinnedAddress() == nullptr )
        {
          for ( std::uint32_t window = 0; window < build.target().windows.size(); ++window )
          {
            Window const& shown = build.target().windows[window];
            bool differs = false;
            for ( std::uint32_t phase = 0; phase < one.residency().phaseCount() && !differs; ++phase )
            {
              differs = one.residency().includes( PhaseIndex{ phase } ) &&
                        baseIn( build.project(), PhaseIndex{ phase }, WindowIndex{ window } ) != shown.base;
            }
            if ( differs )
            {
              candidate.keepOut.insert( candidate.keepOut.end(), shown.ranges.begin(), shown.ranges.end() );
            }
          }
        }
      }
      candidate.temporary = section.isTemporary();
      if ( std::optional<SectionIndex> const next = section.next(); next.has_value() )
      {
        candidate.next = SectionRef{ .module = where.module, .section = *next };
      }

      if ( section.pinnedAddress() != nullptr )
      {
        // A pin is an Integer with a declared value, for the reason that
        // governs an instruction's width: one computed from the layout would
        // be the same cycle one turn wider.
        std::optional<std::int64_t> const address = declaredValueOf(
            build.sources(), build.symbols(), &build.charsets(), where.module, *section.pinnedAddress() );
        if ( !address.has_value() || *address < 0 || *address >= 0x10000 )
        {
          diag::SourceSpan const at = section.pinnedAddress()->span;
          sink.add( diag::diagnostic( diag::DiagnosticId::PIN_NOT_AN_ADDRESS )
                        .at( at.begin, at.length )
                        .arg( "address", address.value_or( 0 ) ) );
        }
        else
        {
          candidate.pinned = static_cast<std::uint32_t>( *address );
        }
      }

      // An alignment and a boundary are declared values for the same reason,
      // and each is a count of bytes, so zero is not one.
      if ( section.alignment() != nullptr )
      {
        std::optional<std::int64_t> const value =
            declaredValueOf( build.sources(), build.symbols(), &build.charsets(), where.module, *section.alignment() );
        if ( !value.has_value() || *value < 1 || *value > 0x10000 )
        {
          diag::SourceSpan const at = section.alignment()->span;
          sink.add( diag::diagnostic( diag::DiagnosticId::BAD_ALIGNMENT )
                        .at( at.begin, at.length )
                        .arg( "alignment", value.value_or( 0 ) ) );
        }
        else
        {
          candidate.alignment = static_cast<std::uint32_t>( *value );
        }
      }

      if ( section.boundary() != nullptr )
      {
        std::optional<std::int64_t> const value =
            declaredValueOf( build.sources(), build.symbols(), &build.charsets(), where.module, *section.boundary() );
        if ( !value.has_value() || *value < 1 || *value > 0x10000 )
        {
          diag::SourceSpan const at = section.boundary()->span;
          sink.add( diag::diagnostic( diag::DiagnosticId::BAD_BOUNDARY )
                        .at( at.begin, at.length )
                        .arg( "boundary", value.value_or( 0 ) ) );
        }
        else
        {
          candidate.boundary = static_cast<std::uint32_t>( *value );
        }
      }

      // Nothing that large lies within the boundary wherever it starts: a
      // local impossibility, reported once here and asked of nobody.
      if ( candidate.boundary > 0 && candidate.size > candidate.boundary )
      {
        sink.add( diag::diagnostic( diag::DiagnosticId::SECTION_EXCEEDS_BOUNDARY )
                      .at( section.span().begin, section.span().length )
                      .arg( "section", nameOfSection( build, where ) )
                      .arg( "size", candidate.size )
                      .arg( "boundary", candidate.boundary ) );
        candidate.impossible = true;
      }

      candidates.push_back( candidate );
    }
  }
  return candidates;
}

/// What a Section takes of a pool: its size, or for a pin the part of it that
/// lies within the pool — a pin outside the pool is a Constraint the author
/// chose and costs the pool nothing.
std::uint32_t bytesWithin( Candidate const& candidate, std::span<AddressRange const> pool )
{
  if ( !candidate.pinned.has_value() )
  {
    return candidate.size;
  }
  std::uint32_t within = 0;
  for ( AddressRange const& part : pool )
  {
    std::uint32_t const begin = std::max( *candidate.pinned, part.begin );
    std::uint32_t const end = std::min( *candidate.pinned + candidate.size, part.end );
    within += end > begin ? end - begin : 0;
  }
  return within;
}

std::uint32_t sizeOf( std::span<AddressRange const> pool )
{
  std::uint32_t total = 0;
  for ( AddressRange const& part : pool )
  {
    total += part.size();
  }
  return total;
}

/// The first layer of explaining a layout that cannot exist: for every Phase
/// and pool, what the Sections present take against what the pool holds.
/// Arithmetic over the model, before the solver is asked, because the sum
/// names the cause where a solver could only name whichever Section came
/// last — see docs/spec/diagnostics.md. The Sections that weigh most are
/// attached, largest first and in Project order among equals.
/// True when some Phase was refused.
bool reportCapacity( Sized const& build, std::span<Candidate const> candidates, diag::DiagnosticSink& sink )
{
  PhaseGraph const& phases = build.phases();
  Pools const& pools = build.target().pools;

  constexpr std::size_t largest = 3;
  bool refused = false;

  struct Contribution
  {
    std::uint32_t bytes = 0;
    Candidate const* who = nullptr;
  };

  for ( std::uint32_t index = 0; index < phases.phases.size(); ++index )
  {
    PhaseIndex const phase{ index };
    std::string const name = phases.phases[index].name.value_or( "(implicit)" );

    auto const report = [&]( PlacementClass placement, std::span<AddressRange const> pool, diag::Diagnostic finding )
    {
      std::uint32_t required = 0;
      std::vector<Contribution> contributions;
      auto const presentHere = [&]( Candidate const& candidate )
      {
        return candidate.placement == placement && !candidate.pane.has_value() &&
               candidate.residency->phaseCount() > index && candidate.residency->includes( phase );
      };
      for ( Candidate const& candidate : candidates )
      {
        // A Pane's Section takes nothing from the pools: it stands in a Bank.
        if ( !presentHere( candidate ) )
        {
          continue;
        }
        // A Temporary that may share with another present here takes
        // nothing the sum can count: what the pair takes together is the
        // solver's answer, and a sum that counted both would refuse a Phase
        // that fits — the sum is a bound from below or it is no refusal.
        if ( candidate.temporary &&
             std::ranges::any_of( candidates,
                                  [&]( Candidate const& other )
                                  {
                                    return &other != &candidate && other.temporary && presentHere( other ) &&
                                           build.interference().mayShare( candidate.where, other.where );
                                  } ) )
        {
          continue;
        }
        std::uint32_t const bytes = bytesWithin( candidate, pool );
        if ( bytes == 0 )
        {
          continue;
        }
        required += bytes;
        contributions.push_back( Contribution{ .bytes = bytes, .who = &candidate } );
      }
      if ( required <= sizeOf( pool ) )
      {
        return;
      }

      refused = true;
      std::ranges::stable_sort( contributions, std::ranges::greater{}, &Contribution::bytes );
      finding =
          std::move( finding ).arg( "phase", name ).arg( "required", required ).arg( "available", sizeOf( pool ) );
      for ( Contribution const& contribution : contributions | std::views::take( largest ) )
      {
        finding = std::move( finding ).note( diag::diagnostic( diag::DiagnosticId::LARGEST_CONTRIBUTOR )
                                                 .at( contribution.who->span.begin, contribution.who->span.length )
                                                 .arg( "section", nameOfSection( build, contribution.who->where ) )
                                                 .arg( "size", contribution.bytes ) );
      }
      sink.add( std::move( finding ).sortedBy( name ) );
    };

    // One View until Views exist, and the glossary names it.
    report( PlacementClass::ABSOLUTE,
            pools.general,
            diag::diagnostic( diag::DiagnosticId::PHASE_DOES_NOT_FIT ).arg( "view", "fixed" ) );
    report( PlacementClass::ZEROPAGE,
            pools.zeroPage,
            diag::diagnostic( diag::DiagnosticId::ZERO_PAGE_DOES_NOT_FIT_IN_PHASE ) );
  }
  return refused;
}

/// The starts at which `size` bytes lie within `pool`, or nothing when the
/// pool is too small for them.
std::optional<AddressRange> startsWithin( AddressRange pool, std::uint32_t size )
{
  if ( pool.size() < size )
  {
    return std::nullopt;
  }
  return AddressRange{ .begin = pool.begin, .end = pool.end - size + 1 };
}

/// The group a Claim in a state of a Window belongs to: one address space
/// per Bank of a set, whichever Window shows it, and one per named state of
/// each Window. `fixed` and a Window's base state are the one space with no
/// group.
std::uint32_t groupOfBank( UnitSetIndex set, std::uint32_t bank )
{
  return ( set.value << 16U ) | bank;
}

std::uint32_t groupOfState( WindowIndex window, std::uint32_t state )
{
  return 0x80000000U | ( window.value << 8U ) | state;
}

/// The Panes as the solver is asked about them: every Pane of a unit set
/// that is not pinned to a named state becomes a PaneClaim, and its
/// Sections' Candidates name it; a Pane pinned to a named state has its
/// address space now, and its Sections carry it as a group. Returns which
/// PaneClaim each Pane became, or nothing for a Pane the solver has no say
/// in.
std::vector<std::optional<std::size_t>>
paneClaimsOf( Target const& target, std::vector<Candidate>& candidates, Layout& layout, std::vector<PaneClaim>& into )
{
  std::vector<std::optional<std::size_t>> claimOf( target.panes.size() );
  for ( std::uint32_t index = 0; index < target.panes.size(); ++index )
  {
    Pane const& pane = target.panes[index];
    if ( pane.state.has_value() )
    {
      layout.placePane( PaneIndex{ index }, *pane.state );
      continue;
    }
    std::optional<UnitSetIndex> const set = target.unitSetShownBy( pane.window );
    if ( !set.has_value() )
    {
      continue;
    }
    claimOf[index] = into.size();
    into.push_back( PaneClaim{ .set = set->value,
                               .banks = target.unitSets[set->value].count,
                               .count = pane.count,
                               .bank = std::nullopt,
                               .window = target.windows[pane.window.value].ranges } );
  }
  for ( Candidate& candidate : candidates )
  {
    if ( !candidate.pane.has_value() )
    {
      continue;
    }
    candidate.paneClaim = claimOf[candidate.pane->value];
    if ( !candidate.paneClaim.has_value() )
    {
      candidate.group = addressSpaceOf( target, layout, *candidate.pane );
    }
  }
  return claimOf;
}

/// The sums for a Pane, before the solver is asked: in every Phase, what
/// its Sections present take against what one Bank holds — a Window's
/// worth — and for a family, whether the set has that many Banks in a row.
/// Arithmetic over the model, as reportCapacity is, because the sum names
/// the cause where the solver could only say that no layout exists.
/// True when some Pane was refused.
bool reportPaneCapacity( Sized const& build, std::span<Candidate const> candidates, diag::DiagnosticSink& sink )
{
  Target const& target = build.target();
  PhaseGraph const& phases = build.phases();
  bool refused = false;
  for ( std::uint32_t index = 0; index < target.panes.size(); ++index )
  {
    Pane const& pane = target.panes[index];
    std::optional<UnitSetIndex> const set = target.unitSetShownBy( pane.window );
    if ( pane.state.has_value() || !set.has_value() )
    {
      continue;
    }
    UnitSet const& units = target.unitSets[set->value];
    if ( pane.count > units.count )
    {
      sink.add( diag::diagnostic( diag::DiagnosticId::PANE_FAMILY_EXCEEDS_SET )
                    .at( pane.site.begin, pane.site.length )
                    .arg( "pane", pane.name )
                    .arg( "count", pane.count )
                    .arg( "set", units.name )
                    .arg( "banks", units.count )
                    .sortedBy( pane.name ) );
      refused = true;
      continue;
    }
    std::uint32_t const holds = target.windows[pane.window.value].size();
    for ( std::uint32_t phase = 0; phase < phases.phases.size(); ++phase )
    {
      std::uint32_t needed = 0;
      for ( Candidate const& candidate : candidates )
      {
        if ( candidate.pane == PaneIndex{ index } && candidate.live.phaseCount() > phase &&
             candidate.live.includes( PhaseIndex{ phase } ) )
        {
          needed += candidate.size;
        }
      }
      if ( needed <= holds )
      {
        continue;
      }
      std::string const name = phases.phases[phase].name.value_or( "(implicit)" );
      sink.add( diag::diagnostic( diag::DiagnosticId::PANE_NO_BANK )
                    .at( pane.site.begin, pane.site.length )
                    .arg( "pane", pane.name )
                    .arg( "size", needed )
                    .arg( "phase", name )
                    .arg( "set", units.name )
                    .arg( "available", holds )
                    .sortedBy( pane.name + " " + name ) );
      refused = true;
      break;
    }
  }
  return refused;
}

/// Where an unpinned Section may start: each range of its pool, with the
/// ranges it keeps out of — the stream's, a `.with`'s Window's — cut out of
/// it. The ranges stay sorted, so the lowest is searched first.
std::vector<AddressRange>
allowedStartsOf( Candidate const& candidate, Pools const& pools, std::span<AddressRange const> paneWindow )
{
  std::span<AddressRange const> const pool =
      candidate.placement == PlacementClass::ZEROPAGE ? pools.zeroPage : pools.general;
  std::vector<AddressRange> starts;
  if ( candidate.pane.has_value() )
  {
    // A Pane's Section stands within its Window, whatever the pools say:
    // the Window's ranges are its pool, and the stream's ranges are where
    // it lives rather than what it keeps out of.
    for ( AddressRange const& part : paneWindow )
    {
      if ( std::optional<AddressRange> const within = startsWithin( part, candidate.size ); within.has_value() )
      {
        starts.push_back( *within );
      }
    }
    return starts;
  }
  auto const add = [&]( AddressRange part )
  {
    if ( std::optional<AddressRange> const within = startsWithin( part, candidate.size ); within.has_value() )
    {
      starts.push_back( *within );
    }
  };

  // The pool with the kept-out ranges taken out of it, one range at a time,
  // since a Window's ranges need not be contiguous.
  std::vector<AddressRange> parts( pool.begin(), pool.end() );
  {
    for ( AddressRange const& window : candidate.keepOut )
    {
      std::vector<AddressRange> cut;
      for ( AddressRange const& part : parts )
      {
        if ( window.end <= part.begin || window.begin >= part.end )
        {
          cut.push_back( part );
          continue;
        }
        if ( window.begin > part.begin )
        {
          cut.push_back( AddressRange{ .begin = part.begin, .end = std::min( window.begin, part.end ) } );
        }
        if ( window.end < part.end )
        {
          cut.push_back( AddressRange{ .begin = std::max( window.end, part.begin ), .end = part.end } );
        }
      }
      parts = std::move( cut );
    }
  }
  for ( AddressRange const& part : parts )
  {
    add( part );
  }
  return starts;
}

} // namespace

Layout::Layout( std::span<Module const> modules, std::size_t paneCount )
{
  mByModule.resize( modules.size() );
  for ( std::size_t module = 0; module < modules.size(); ++module )
  {
    mByModule[module].resize( modules[module].sections().size() );
  }
  mPaneStates.resize( paneCount );
}

void Layout::placePane( PaneIndex pane, std::uint32_t state )
{
  if ( pane.value >= mPaneStates.size() )
  {
    mPaneStates.resize( pane.value + 1 );
  }
  mPaneStates[pane.value] = state;
}

std::optional<std::uint32_t> Layout::stateOfPane( PaneIndex pane ) const
{
  return pane.value < mPaneStates.size() ? mPaneStates[pane.value] : std::nullopt;
}

std::optional<std::uint32_t> addressSpaceOf( Target const& target, Layout const& layout, PaneIndex pane )
{
  std::optional<std::uint32_t> const state = layout.stateOfPane( pane );
  if ( !state.has_value() )
  {
    return std::nullopt;
  }
  Pane const& one = target.panes[pane.value];
  if ( one.state.has_value() )
  {
    return groupOfState( one.window, *state );
  }
  std::optional<UnitSetIndex> const set = target.unitSetShownBy( one.window );
  if ( !set.has_value() )
  {
    return std::nullopt;
  }
  std::uint32_t const first = target.windows[one.window.value].firstStateOf( *set, target.unitSets ).value_or( 0 );
  return groupOfBank( *set, *state - first );
}

void Layout::place( SectionRef where, std::uint32_t address )
{
  mByModule[where.module.value][where.section.value] = Placement{ .everywhere = address, .byPhase = {} };
}

void Layout::place( SectionRef where, PhaseIndex phase, std::uint32_t address )
{
  Placement& placement = mByModule[where.module.value][where.section.value];
  placement.everywhere.reset();
  if ( placement.byPhase.size() <= phase.value )
  {
    placement.byPhase.resize( phase.value + 1 );
  }
  placement.byPhase[phase.value] = address;
}

Layout::Placement const& Layout::at( SectionRef where ) const
{
  return mByModule[where.module.value][where.section.value];
}

bool Layout::isPlaced( SectionRef where ) const
{
  Placement const& placement = at( where );
  return placement.everywhere.has_value() ||
         std::ranges::any_of( placement.byPhase,
                              []( std::optional<std::uint32_t> const& one ) { return one.has_value(); } );
}

std::uint32_t Layout::addressIn( SectionRef where, PhaseIndex phase ) const
{
  Placement const& placement = at( where );
  if ( placement.everywhere.has_value() )
  {
    return *placement.everywhere;
  }
  return phase.value < placement.byPhase.size() ? placement.byPhase[phase.value].value_or( 0 ) : 0;
}

std::optional<std::uint32_t> Layout::addressAcross( SectionRef where, Residency const* across ) const
{
  Placement const& placement = at( where );
  if ( placement.everywhere.has_value() )
  {
    return placement.everywhere;
  }
  std::optional<std::uint32_t> one;
  for ( std::size_t phase = 0; phase < placement.byPhase.size(); ++phase )
  {
    std::optional<std::uint32_t> const& here = placement.byPhase[phase];
    if ( !here.has_value() ||
         ( across != nullptr && ( phase >= across->phaseCount() ||
                                  !across->includes( PhaseIndex{ static_cast<std::uint32_t>( phase ) } ) ) ) )
    {
      continue;
    }
    if ( one.has_value() && *one != *here )
    {
      return std::nullopt;
    }
    one = here;
  }
  return one;
}

std::uint32_t Layout::addressOf( SectionRef where ) const
{
  Placement const& placement = at( where );
  if ( placement.everywhere.has_value() )
  {
    return *placement.everywhere;
  }
  for ( std::optional<std::uint32_t> const& here : placement.byPhase )
  {
    if ( here.has_value() )
    {
      return *here;
    }
  }
  return 0;
}

Layout placeSections( Sized const& build, Storage const& storage, bool explain, diag::DiagnosticSink& sink )
{
  PhaseGraph const& phases = build.phases();
  Target const& target = build.target();
  Layout layout{ build.modules(), target.panes.size() };
  auto const frozenOverOf = [&build]( Candidate const& candidate )
  {
    std::span<Residency const> const across = build.freezes().of( candidate.where );
    return std::vector<Residency>( across.begin(), across.end() );
  };
  std::vector<Candidate> candidates = candidatesOf( build, storage, sink );
  Pools const& pools = target.pools;
  Problem problem;
  std::vector<std::optional<std::size_t>> const paneClaimOf = paneClaimsOf( target, candidates, layout, problem.panes );

  // What the Frames took of the storage set before any Bank was given: the
  // first half of PlaceStorage put them at the start of storage, and a
  // Pane's Section may not stand on them in a Phase the Frame is live in.
  // A Frame may run from one unit into the next, and takes its part of each.
  if ( target.storageUnits.has_value() && target.unitSize > 0 )
  {
    for ( std::uint32_t index = 0; index < storage.frameCount(); ++index )
    {
      FrameIndex const frame{ index };
      if ( !storage.isFramePlaced( frame ) )
      {
        continue;
      }
      std::uint32_t const begin = target.positionOf( storage.frameAddressOf( frame ) );
      std::uint32_t const end = begin + storage.frameSizeOf( frame );
      for ( std::uint32_t at = begin; at < end; at = ( ( at / target.unitSize ) + 1 ) * target.unitSize )
      {
        std::uint32_t const unitEnd = std::min( end, ( ( at / target.unitSize ) + 1 ) * target.unitSize );
        problem.occupied.push_back( Occupied{ .set = target.storageUnits->value,
                                              .bank = at / target.unitSize,
                                              .offset = at % target.unitSize,
                                              .size = unitEnd - at,
                                              .live = storage.frameLiveOf( frame ) } );
      }
    }
  }
  auto const paneWindowOf = [&target]( Candidate const& candidate ) -> std::span<AddressRange const>
  {
    if ( !candidate.pane.has_value() )
    {
      return {};
    }
    return target.windows[target.panes[candidate.pane->value].window.value].ranges;
  };

  bool const refusedByPools = reportCapacity( build, candidates, sink );
  bool const refusedByPanes = reportPaneCapacity( build, candidates, sink );
  bool const refusedBySums = refusedByPools || refusedByPanes;

  // What the solver is asked, and which Section each Claim stands for. Pins
  // go first: their start is decided, so deciding it first costs the search
  // nothing and everything else is fitted around them.
  std::vector<Claim>& claims = problem.claims;
  std::vector<Candidate const*> claimants;
  AcceptedPins accepted;

  // Which pins can be told apart before the solver: two in one address space,
  // or two in one Pane, whose Bank is one whatever it turns out to be. Pins
  // in two Panes of a set may or may not share a Bank, which the solver says.
  auto const pinSpaceOf = []( Candidate const& candidate ) -> std::optional<std::uint32_t>
  {
    if ( candidate.paneClaim.has_value() )
    {
      return 0x40000000U | static_cast<std::uint32_t>( *candidate.paneClaim );
    }
    return candidate.group;
  };

  // A pin that is impossible on its own is refused here, where the message
  // can still say what it was: past the end of memory, out of the zero page,
  // in the Window, in a register Region, or on another pin in a Phase both
  // are present in. Two pins at one address in disjoint Phases are not an
  // overlap: the two are never in memory together, and a pin across Phases is
  // exactly what the rule allows. A pin in a reserved Region is a warning and
  // stands: the variant and the author disagree about the address, and the
  // tool cannot know the stack's depth or what the author knows about it.
  for ( Candidate const& candidate : candidates )
  {
    if ( !candidate.pinned.has_value() || candidate.impossible )
    {
      continue;
    }

    AddressRange const range{ .begin = *candidate.pinned, .end = *candidate.pinned + candidate.size };
    if ( range.end > ADDRESS_SPACE_END )
    {
      sink.add( diag::diagnostic( diag::DiagnosticId::NO_ROOM )
                    .at( candidate.span.begin, candidate.span.length )
                    .arg( "section", nameOfSection( build, candidate.where ) )
                    .arg( "size", candidate.size ) );
      continue;
    }

    if ( candidate.placement == PlacementClass::ZEROPAGE && range.end > ZERO_PAGE_END )
    {
      sink.add( diag::diagnostic( diag::DiagnosticId::ZERO_PAGE_DOES_NOT_FIT )
                    .at( candidate.span.begin, candidate.span.length )
                    .arg( "section", nameOfSection( build, candidate.where ) ) );
      continue;
    }

    if ( candidate.outsideWindow && !candidate.pane.has_value() &&
         std::ranges::any_of( target.streamRanges,
                              [range]( AddressRange const& window )
                              { return range.begin < window.end && window.begin < range.end; } ) )
    {
      sink.add( diag::diagnostic( diag::DiagnosticId::PAYLOAD_PINNED_IN_WINDOW )
                    .at( candidate.span.begin, candidate.span.length )
                    .arg( "section", nameOfSection( build, candidate.where ) )
                    .arg( "address", range.begin ) );
      continue;
    }

    // Pinned inside a Window that a `.with` shows another state of while
    // this Section runs or is named: it would not be seen there.
    bool underWith = false;
    for ( WindowIndex const window : candidate.keepOutWindows )
    {
      if ( target.windows[window.value].meets( range ) )
      {
        sink.add( diag::diagnostic( diag::DiagnosticId::PIN_UNDER_WITH )
                      .at( candidate.span.begin, candidate.span.length )
                      .arg( "section", nameOfSection( build, candidate.where ) )
                      .arg( "address", range.begin )
                      .arg( "window", target.windows[window.value].name ) );
        underWith = true;
        break;
      }
    }
    if ( underWith )
    {
      continue;
    }

    // A Pane's Section pinned outside its Window is pinned in a state that
    // does not reach there.
    if ( candidate.pane.has_value() &&
         !std::ranges::any_of( paneWindowOf( candidate ),
                               [range]( AddressRange const& part )
                               { return range.begin >= part.begin && range.end <= part.end; } ) )
    {
      sink.add( diag::diagnostic( diag::DiagnosticId::PIN_OUTSIDE_PANE_WINDOW )
                    .at( candidate.span.begin, candidate.span.length )
                    .arg( "section", nameOfSection( build, candidate.where ) )
                    .arg( "address", range.begin )
                    .arg( "pane", target.panes[candidate.pane->value].name ) );
      continue;
    }

    if ( std::optional<RegionIndex> const over =
             candidate.pane.has_value() ? std::nullopt : target.mostRestrictiveOver( range );
         over.has_value() )
    {
      Region const& region = target.regions[over->value];
      auto const inRegion = [&]( diag::DiagnosticId id )
      {
        diag::Diagnostic finding = diag::diagnostic( id )
                                       .at( candidate.span.begin, candidate.span.length )
                                       .arg( "section", nameOfSection( build, candidate.where ) )
                                       .arg( "address", range.begin )
                                       .arg( "region", displayNameOf( region ) );
        if ( region.site.has_value() )
        {
          finding = std::move( finding ).note( diag::diagnostic( diag::DiagnosticId::REGION_DECLARED_HERE )
                                                   .at( region.site->begin, region.site->length )
                                                   .arg( "region", displayNameOf( region ) ) );
        }
        return finding;
      };
      if ( region.property == RegionProperty::REGISTER )
      {
        sink.add( inRegion( diag::DiagnosticId::PIN_IN_REGISTER ) );
        continue;
      }
      if ( region.property == RegionProperty::RESERVED )
      {
        sink.add( inRegion( diag::DiagnosticId::PIN_IN_RESERVED ) );
      }
    }

    // A pin has to satisfy the rest of its placement, and whether it does is
    // arithmetic.
    if ( range.begin % candidate.alignment != 0 )
    {
      sink.add( diag::diagnostic( diag::DiagnosticId::PIN_NOT_ALIGNED )
                    .at( candidate.span.begin, candidate.span.length )
                    .arg( "section", nameOfSection( build, candidate.where ) )
                    .arg( "address", range.begin )
                    .arg( "alignment", candidate.alignment ) );
      continue;
    }

    if ( candidate.boundary > 0 && candidate.size > 0 &&
         range.begin / candidate.boundary != ( range.end - 1 ) / candidate.boundary )
    {
      sink.add( diag::diagnostic( diag::DiagnosticId::PIN_CROSSES_BOUNDARY )
                    .at( candidate.span.begin, candidate.span.length )
                    .arg( "section", nameOfSection( build, candidate.where ) )
                    .arg( "address", range.begin )
                    .arg( "last", range.end - 1 )
                    .arg( "boundary", candidate.boundary ) );
      continue;
    }

    if ( std::optional<SectionRef> const other = accepted.overlapping( range, candidate.live, pinSpaceOf( candidate ) );
         other.has_value() )
    {
      sink.add( diag::diagnostic( diag::DiagnosticId::SECTION_OVERLAP )
                    .at( candidate.span.begin, candidate.span.length )
                    .arg( "section", nameOfSection( build, candidate.where ) )
                    .arg( "other", nameOfSection( build, *other ) )
                    .arg( "address", range.begin ) );
      continue;
    }

    accepted.accept( range, candidate.where, candidate.live, pinSpaceOf( candidate ) );
    // A pin is one start in every Phase, so a pinned Section is not movable
    // whatever it declared.
    claims.push_back( Claim{ .size = candidate.size,
                             .allowedStarts = { AddressRange{ .begin = range.begin, .end = range.begin + 1 } },
                             .alignment = candidate.alignment,
                             .boundary = candidate.boundary,
                             .movable = false,
                             .frozenOver = {},
                             .residency = candidate.live,
                             .sharesWith = {},
                             .follows = std::nullopt,
                             .group = candidate.group,
                             .pane = candidate.paneClaim } );
    claimants.push_back( &candidate );
  }

  for ( Candidate const& candidate : candidates )
  {
    if ( candidate.pinned.has_value() || candidate.impossible )
    {
      continue;
    }
    claims.push_back( Claim{ .size = candidate.size,
                             .allowedStarts = allowedStartsOf( candidate, pools, paneWindowOf( candidate ) ),
                             .alignment = candidate.alignment,
                             .boundary = candidate.boundary,
                             .movable = candidate.movable,
                             .frozenOver = candidate.movable ? frozenOverOf( candidate ) : std::vector<Residency>{},
                             .residency = candidate.live,
                             .sharesWith = {},
                             .follows = std::nullopt,
                             .group = candidate.group,
                             .pane = candidate.paneClaim } );
    claimants.push_back( &candidate );
  }

  // Two Temporaries never live at once may overlap in every Phase both are
  // present in; the Interference says which, and each Claim names the other.
  for ( std::size_t a = 0; a < claims.size(); ++a )
  {
    if ( !claimants[a]->temporary )
    {
      continue;
    }
    for ( std::size_t b = a + 1; b < claims.size(); ++b )
    {
      if ( claimants[b]->temporary && build.interference().mayShare( claimants[a]->where, claimants[b]->where ) )
      {
        claims[a].sharesWith.push_back( b );
        claims[b].sharesWith.push_back( a );
      }
    }
  }

  // A Proc another falls through into starts where that one ends.
  for ( std::size_t before = 0; before < claims.size(); ++before )
  {
    if ( !claimants[before]->next.has_value() )
    {
      continue;
    }
    for ( std::size_t index = 0; index < claims.size(); ++index )
    {
      if ( claimants[index]->where == *claimants[before]->next )
      {
        claims[index].follows = before;
      }
    }
  }

  Solution const solution = solve( problem, phases );

  // Every Pane its state, as the solver gave it: the Window's index of the
  // Bank.
  for ( std::uint32_t index = 0; index < target.panes.size(); ++index )
  {
    std::optional<std::size_t> const claim = paneClaimOf[index];
    if ( !claim.has_value() || !solution.paneBanks[*claim].has_value() )
    {
      continue;
    }
    Pane const& pane = target.panes[index];
    std::uint32_t const bank = *solution.paneBanks[*claim];
    std::optional<UnitSetIndex> const set = target.unitSetShownBy( pane.window );
    std::uint32_t const first =
        set.has_value() ? target.windows[pane.window.value].firstStateOf( *set, target.unitSets ).value_or( 0 ) : 0;
    layout.placePane( PaneIndex{ index }, first + bank );
  }

  // No layout at all is one finding and not one per Section: which Section
  // to name is exactly what the solver cannot say in this form, and naming
  // all of them would say nothing.
  if ( !solution.feasible )
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::NO_LAYOUT ) );
  }

  // The pairs no layout can keep apart — asked for, and only where the sums
  // did not already say why: a pair explains nothing a Phase over capacity
  // did not. A pair has no position of its own, so each Section gets a note.
  if ( !solution.feasible && explain && !refusedBySums )
  {
    Explanation const explanation = model::explain( problem, phases );
    for ( auto const& [a, b] : explanation.conflicts )
    {
      Candidate const& first = *claimants[a];
      Candidate const& second = *claimants[b];
      std::string const firstName = nameOfSection( build, first.where );
      std::string const secondName = nameOfSection( build, second.where );
      std::string key = firstName;
      key.append( " " ).append( secondName );
      sink.add( diag::diagnostic( diag::DiagnosticId::CANNOT_BE_KEPT_APART )
                    .arg( "section", firstName )
                    .arg( "other", secondName )
                    .note( diag::diagnostic( diag::DiagnosticId::SECTION_IS_HERE )
                               .at( first.span.begin, first.span.length )
                               .arg( "section", firstName ) )
                    .note( diag::diagnostic( diag::DiagnosticId::SECTION_IS_HERE )
                               .at( second.span.begin, second.span.length )
                               .arg( "section", secondName ) )
                    .sortedBy( std::move( key ) ) );
    }
    if ( !explanation.minimal )
    {
      sink.add( diag::diagnostic( diag::DiagnosticId::EXPLANATION_NOT_MINIMAL ) );
    }
  }

  for ( std::size_t index = 0; index < claims.size(); ++index )
  {
    Candidate const& candidate = *claimants[index];

    // A movable Section is placed Phase by Phase, whether or not it moved.
    if ( !solution.byPhase[index].empty() )
    {
      bool any = false;
      for ( std::size_t phase = 0; phase < solution.byPhase[index].size(); ++phase )
      {
        if ( std::optional<std::uint32_t> const& here = solution.byPhase[index][phase]; here.has_value() )
        {
          layout.place( candidate.where, PhaseIndex{ static_cast<std::uint32_t>( phase ) }, *here );
          any = true;
        }
      }
      if ( any )
      {
        continue;
      }
    }

    std::optional<std::uint32_t> const at = solution.addresses[index];
    if ( at.has_value() )
    {
      layout.place( candidate.where, *at );
      continue;
    }

    // A Section nothing could hold on its own is its own finding, whether or
    // not the rest had a layout.
    if ( std::ranges::all_of( claims[index].allowedStarts,
                              []( AddressRange range ) { return range.begin >= range.end; } ) )
    {
      sink.add( diag::diagnostic( candidate.placement == PlacementClass::ZEROPAGE
                                      ? diag::DiagnosticId::ZERO_PAGE_DOES_NOT_FIT
                                      : diag::DiagnosticId::NO_ROOM )
                    .at( candidate.span.begin, candidate.span.length )
                    .arg( "section", nameOfSection( build, candidate.where ) )
                    .arg( "size", candidate.size ) );
    }
  }

  return layout;
}

void checkAssertions( Placed const& build, diag::DiagnosticSink& sink )
{
  GlobalSymbols const& symbols = build.symbols();
  Resolved resolved{ build.sizes(), build.layout() };

  for ( std::uint32_t module = 0; module < symbols.modules().size(); ++module )
  {
    // An Assertion sees a Movable Section as its Module does: at one address
    // across the Module's Residency, or as undecidable where it moves within
    // it — an Assertion encodes nothing, so it freezes nothing.
    resolved.viewFrom( symbols.modules()[module].residency() );
    for ( Assertion const& assertion : symbols.modules()[module].assertions() )
    {
      if ( containsError( *assertion.condition ) )
      {
        continue;
      }

      // An expression the type check refused has been reported, and evaluating
      // it anyway would answer a question the rules just said is not one: a
      // comparison across two Sections has a value here and is still refused.
      diag::SeverityPolicy policy;
      diag::DiagnosticSink quiet{ policy };
      if ( !typeOf( build, ModuleIndex{ module }, *assertion.condition, quiet ).isKnown() )
      {
        continue;
      }

      std::optional<std::int64_t> const value = evaluate(
          build.sources(), symbols, &build.charsets(), ModuleIndex{ module }, *assertion.condition, resolved );
      if ( !value.has_value() )
      {
        sink.add( diag::diagnostic( diag::DiagnosticId::ASSERTION_NOT_DECIDABLE )
                      .at( assertion.span.begin, assertion.span.length ) );
        continue;
      }

      if ( *value == 0 )
      {
        sink.add( diag::diagnostic( diag::DiagnosticId::ASSERTION_FAILED )
                      .at( assertion.span.begin, assertion.span.length ) );
      }
    }
  }
}

} // namespace nga::model
