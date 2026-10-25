#include "nga/model/Project.hpp"

#include "nga/model/Runtime.hpp"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <filesystem>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace nga::model
{

namespace
{

/// Each Container and its word, in the order a finding lists them.
constexpr std::array<std::pair<Container, std::string_view>, 11> CONTAINER_WORDS{ {
    { Container::RAW_IMAGE, "raw" },
    { Container::XEX, "xex" },
    { Container::ATR, "atr" },
    { Container::CAR, "car" },
    { Container::BS93, "bs93" },
    { Container::LNX, "lnx" },
    { Container::LYX, "lyx" },
    { Container::PRG, "prg" },
    { Container::D64, "d64" },
    { Container::D80, "d80" },
    { Container::D82, "d82" },
} };

constexpr std::array<Container, CONTAINER_WORDS.size()> CONTAINERS = []
{
  std::array<Container, CONTAINER_WORDS.size()> all{};
  for ( std::size_t index = 0; index < all.size(); ++index )
  {
    all[index] = CONTAINER_WORDS[index].first;
  }
  return all;
}();

} // namespace

std::optional<Container> containerNamed( std::string_view word )
{
  auto const found = std::ranges::find( CONTAINER_WORDS, word, &std::pair<Container, std::string_view>::second );
  return found == CONTAINER_WORDS.end() ? std::nullopt : std::optional{ found->first };
}

std::string_view nameOf( Container container )
{
  auto const found = std::ranges::find( CONTAINER_WORDS, container, &std::pair<Container, std::string_view>::first );
  return found == CONTAINER_WORDS.end() ? std::string_view{ "raw" } : found->second;
}

std::span<Container const> containers()
{
  return CONTAINERS;
}

std::optional<Cpu> cpuNamed( std::string_view word )
{
  if ( word == "6502" )
  {
    return Cpu::MOS6502;
  }
  if ( word == "65sc02" )
  {
    return Cpu::WDC65SC02;
  }
  return std::nullopt;
}

std::string_view nameOf( Cpu cpu )
{
  return cpu == Cpu::WDC65SC02 ? "65sc02" : "6502";
}

std::optional<Intent> intentNamed( std::string_view word )
{
  if ( word == "speed" )
  {
    return Intent::SPEED;
  }
  if ( word == "size" )
  {
    return Intent::SIZE;
  }
  if ( word == "fit" )
  {
    return Intent::FIT;
  }
  return std::nullopt;
}

std::string_view nameOf( Intent intent )
{
  switch ( intent )
  {
  case Intent::SPEED:
    return "speed";
  case Intent::SIZE:
    return "size";
  case Intent::FIT:
    break;
  }
  return "fit";
}

namespace
{

constexpr std::array<std::pair<RegionProperty, std::string_view>, 4> REGION_PROPERTY_WORDS{ {
    { RegionProperty::RAM, "ram" },
    { RegionProperty::ROM, "rom" },
    { RegionProperty::RESERVED, "reserved" },
    { RegionProperty::REGISTER, "register" },
} };

} // namespace

std::string_view nameOf( RegionProperty property )
{
  auto const found =
      std::ranges::find( REGION_PROPERTY_WORDS, property, &std::pair<RegionProperty, std::string_view>::first );
  return found == REGION_PROPERTY_WORDS.end() ? std::string_view{ "region" } : found->second;
}

std::optional<RegionProperty> regionPropertyNamed( std::string_view word )
{
  auto const found =
      std::ranges::find( REGION_PROPERTY_WORDS, word, &std::pair<RegionProperty, std::string_view>::second );
  return found == REGION_PROPERTY_WORDS.end() ? std::nullopt : std::optional{ found->first };
}

std::string displayNameOf( Region const& region )
{
  if ( !region.name.empty() )
  {
    return region.name;
  }
  return fmt::format( "${:04X}..${:04X}", region.range.begin, region.range.end - 1 );
}

namespace
{

std::uint32_t sumOf( std::span<AddressRange const> ranges )
{
  std::uint32_t total = 0;
  for ( AddressRange const& range : ranges )
  {
    total += range.size();
  }
  return total;
}

} // namespace

std::uint32_t Pools::zeroPageSize() const
{
  return sumOf( zeroPage );
}

std::uint32_t Pools::generalSize() const
{
  return sumOf( general );
}

std::uint32_t Pools::readOnlySize() const
{
  return sumOf( readOnly );
}

Pools poolsOf( std::span<Region const> regions, std::span<AddressRange const> windows )
{
  // One entry per address, holding the most restrictive property declared
  // over it, or nothing where no Region reaches. Sixty-four thousand bytes
  // once per Project is cheaper than any interval arithmetic that would have
  // to be right about overlaps.
  std::array<std::optional<RegionProperty>, ADDRESS_SPACE_END> covered;
  for ( Region const& region : regions )
  {
    for ( std::uint32_t address = region.range.begin; address < region.range.end; ++address )
    {
      std::optional<RegionProperty>& here = covered[address];
      if ( !here.has_value() || *here < region.property )
      {
        here = region.property;
      }
    }
  }
  for ( AddressRange const& window : windows )
  {
    for ( std::uint32_t address = window.begin; address < window.end; ++address )
    {
      covered[address].reset();
    }
  }

  Pools pools;
  auto const runsOf =
      [&covered]( RegionProperty property, std::uint32_t from, std::uint32_t to, std::vector<AddressRange>& into )
  {
    std::optional<std::uint32_t> start;
    for ( std::uint32_t address = from; address <= to; ++address )
    {
      bool const in = address < to && covered[address] == property;
      if ( in && !start.has_value() )
      {
        start = address;
      }
      else if ( !in && start.has_value() )
      {
        into.push_back( AddressRange{ .begin = *start, .end = address } );
        start.reset();
      }
    }
  };
  runsOf( RegionProperty::RAM, 0, 0x100, pools.zeroPage );
  runsOf( RegionProperty::RAM, 0x100, ADDRESS_SPACE_END, pools.general );
  // No zero-page half: a Section that is never written gains nothing from the
  // zero page, so the two are refused together and a `rom` Region below `$100`
  // is in no pool.
  runsOf( RegionProperty::ROM, 0x100, ADDRESS_SPACE_END, pools.readOnly );
  return pools;
}

std::vector<Region> standInRegions()
{
  return { Region{ .nameSpan = std::nullopt,
                   .name = {},
                   .range = AddressRange{ .begin = 0x0080, .end = 0x0090 },
                   .property = RegionProperty::RAM,
                   .site = std::nullopt },
           Region{ .nameSpan = std::nullopt,
                   .name = {},
                   .range = AddressRange{ .begin = 0x2000, .end = 0xA000 },
                   .property = RegionProperty::RAM,
                   .site = std::nullopt } };
}

std::uint32_t Window::size() const
{
  std::uint32_t total = 0;
  for ( AddressRange const& range : ranges )
  {
    total += range.size();
  }
  return total;
}

bool Window::covers( std::uint32_t address ) const
{
  return std::ranges::any_of(
      ranges, [address]( AddressRange const& range ) { return address >= range.begin && address < range.end; } );
}

bool Window::meets( AddressRange other ) const
{
  return std::ranges::any_of( ranges, [other]( AddressRange const& range ) { return range.overlaps( other ); } );
}

std::optional<std::uint32_t> Window::firstStateOf( UnitSetIndex set, std::span<UnitSet const> sets ) const
{
  std::uint32_t index = 0;
  for ( WindowState const& state : states )
  {
    if ( state.units.has_value() && *state.units == set )
    {
      return index;
    }
    index += state.indices( sets );
  }
  return std::nullopt;
}

std::uint64_t Window::stateCount( std::span<UnitSet const> sets ) const
{
  std::uint64_t count = 0;
  for ( WindowState const& state : states )
  {
    count += state.indices( sets );
  }
  return count;
}

std::optional<std::uint32_t> Window::namedStateOf( std::string_view wanted, std::span<UnitSet const> sets ) const
{
  std::uint32_t index = 0;
  for ( WindowState const& state : states )
  {
    if ( !state.units.has_value() && state.name == wanted )
    {
      return index;
    }
    index += state.indices( sets );
  }
  return std::nullopt;
}

std::optional<UnitSetIndex> Target::unitSetNamed( std::string_view name ) const
{
  for ( std::uint32_t index = 0; index < unitSets.size(); ++index )
  {
    if ( unitSets[index].name == name )
    {
      return UnitSetIndex{ index };
    }
  }
  return std::nullopt;
}

std::optional<WindowIndex> Target::windowNamed( std::string_view name ) const
{
  for ( std::uint32_t index = 0; index < windows.size(); ++index )
  {
    if ( windows[index].name == name )
    {
      return WindowIndex{ index };
    }
  }
  return std::nullopt;
}

std::optional<PaneIndex> Target::paneNamed( std::string_view name ) const
{
  for ( std::uint32_t index = 0; index < panes.size(); ++index )
  {
    if ( panes[index].name == name )
    {
      return PaneIndex{ index };
    }
  }
  return std::nullopt;
}

std::optional<UnitSetIndex> Target::unitSetShownBy( WindowIndex window ) const
{
  for ( WindowState const& state : windows[window.value].states )
  {
    if ( state.units.has_value() )
    {
      return state.units;
    }
  }
  return std::nullopt;
}

std::optional<std::uint32_t> Target::bankSizeOf( UnitSetIndex set ) const
{
  for ( Window const& window : windows )
  {
    if ( window.firstStateOf( set, unitSets ).has_value() )
    {
      return window.size();
    }
  }
  return std::nullopt;
}

std::optional<AddressRange> Target::linearBanksOf( UnitSetIndex set ) const
{
  UnitSet const& units = unitSets[set.value];
  if ( !units.linear.has_value() )
  {
    return std::nullopt;
  }
  std::optional<std::uint32_t> const bank = bankSizeOf( set );
  if ( !bank.has_value() )
  {
    return units.linear;
  }
  return AddressRange{ .begin = units.linear->begin, .end = units.linear->begin + ( units.count * *bank ) };
}

std::optional<std::uint32_t> Target::linearAddressOf( PaneIndex pane, std::uint32_t state, std::uint32_t address ) const
{
  Pane const& one = panes[pane.value];
  if ( one.state.has_value() )
  {
    return std::nullopt;
  }
  std::optional<UnitSetIndex> const set = unitSetShownBy( one.window );
  if ( !set.has_value() )
  {
    return std::nullopt;
  }
  std::optional<AddressRange> const& linear = unitSets[set->value].linear;
  if ( !linear.has_value() )
  {
    return std::nullopt;
  }
  Window const& window = windows[one.window.value];
  std::optional<std::uint32_t> const first = window.firstStateOf( *set, unitSets );
  if ( !first.has_value() || state < *first )
  {
    return std::nullopt;
  }
  std::uint32_t offset = 0;
  for ( AddressRange const& range : window.ranges )
  {
    if ( address >= range.begin && address < range.end )
    {
      return linear->begin + ( ( state - *first ) * window.size() ) + offset + ( address - range.begin );
    }
    offset += range.size();
  }
  return std::nullopt;
}

StorageAddress Target::addressAt( std::uint32_t position ) const
{
  if ( unitSize == 0 )
  {
    return StorageAddress{ .bank = BankIndex{ 0 }, .offset = position };
  }
  return StorageAddress{ .bank = BankIndex{ position / unitSize }, .offset = position % unitSize };
}

std::uint32_t Target::widestUsableUnit() const
{
  std::uint32_t widest = 0;
  for ( std::uint32_t unit = 0; bankSize() > 0 && unit * bankSize() < storageSize(); ++unit )
  {
    widest = std::max( widest, usableInUnit( unit ) );
  }
  return widest;
}

std::uint32_t Target::positionOf( StorageAddress at ) const
{
  return ( at.bank.value * unitSize ) + at.offset;
}

std::optional<UnitSetIndex> Target::filledSet() const
{
  return hasStorage() ? storageUnits : streamUnits;
}

std::uint32_t Target::filledCount() const
{
  if ( hasStorage() )
  {
    return unitCount;
  }
  return streamUnits.has_value() ? unitSets[streamUnits->value].count : 0;
}

std::uint32_t Target::filledUnitSize() const
{
  return hasStorage() ? unitSize : streamUnitSize;
}

StorageAddress Target::filledAddressAt( std::uint32_t position ) const
{
  if ( hasStorage() )
  {
    return addressAt( position );
  }
  std::uint32_t const size = filledUnitSize();
  return size == 0 ? StorageAddress{ .bank = BankIndex{ 0 }, .offset = position }
                   : StorageAddress{ .bank = BankIndex{ position / size }, .offset = position % size };
}

std::uint32_t Target::filledPositionOf( StorageAddress at ) const
{
  return hasStorage() ? positionOf( at ) : ( at.bank.value * filledUnitSize() ) + at.offset;
}

std::optional<RegionIndex> Target::mostRestrictiveOver( AddressRange range ) const
{
  // The winner over a range is the winner over some address of it, and that
  // is the most restrictive Region touching the range at all; among equals
  // the first declared, so that the finding is the same on every run.
  std::optional<RegionIndex> found;
  for ( std::uint32_t index = 0; index < regions.size(); ++index )
  {
    Region const& region = regions[index];
    if ( region.range.begin >= range.end || range.begin >= region.range.end )
    {
      continue;
    }
    if ( !found.has_value() || regions[found->value].property < region.property )
    {
      found = RegionIndex{ index };
    }
  }
  return found;
}

bool Residency::intersects( Residency const& other ) const
{
  std::size_t const common = std::min( mPhases.size(), other.mPhases.size() );
  for ( std::size_t phase = 0; phase < common; ++phase )
  {
    if ( mPhases[phase] && other.mPhases[phase] )
    {
      return true;
    }
  }
  return false;
}

bool Residency::isEmpty() const
{
  return std::ranges::none_of( mPhases, []( bool present ) { return present; } );
}

bool Residency::isAll() const
{
  return std::ranges::all_of( mPhases, []( bool present ) { return present; } );
}

void deriveResidency( Project& project )
{
  std::size_t const count = project.phases.phases.size();
  for ( ProjectModule& module : project.modules )
  {
    module.residency = Residency{ count };
  }
  for ( std::uint32_t phase = 0; phase < count; ++phase )
  {
    for ( ModuleIndex const module : project.phases.phases[phase].needs )
    {
      project.modules[module.value].residency.add( PhaseIndex{ phase } );
    }
  }
}

ModuleIndex addToolModule( Project& project, ProjectModule module )
{
  ModuleIndex const index{ static_cast<std::uint32_t>( project.modules.size() ) };
  project.modules.push_back( std::move( module ) );
  for ( Phase& phase : project.phases.phases )
  {
    phase.needs.push_back( index );
  }
  deriveResidency( project );
  return index;
}

Residency notFinal( PhaseGraph const& phases )
{
  Residency residency{ phases.phases.size() };
  for ( std::uint32_t phase = 0; phase < phases.phases.size(); ++phase )
  {
    if ( !phases.phases[phase].finalAt.has_value() || PhaseIndex{ phase } == phases.entry )
    {
      residency.add( PhaseIndex{ phase } );
    }
  }
  return residency;
}

Project
synthesiseProject( diag::SourceManager& sources, std::span<diag::FileId const> files, diag::DiagnosticSink& sink )
{
  Project project;
  std::unordered_set<std::string> taken;

  for ( diag::FileId const file : files )
  {
    std::filesystem::path const path{ sources.pathOf( file ) };
    std::string name = path.stem().string();

    if ( !taken.insert( name ).second )
    {
      // No location: this is a fact about the run rather than about a place in
      // a file, so it sorts by identifier and by the name it is about.
      sink.add( diag::diagnostic( diag::DiagnosticId::MODULE_NAME_COLLISION ).arg( "module", name ).sortedBy( name ) );
      continue;
    }

    project.modules.push_back( ProjectModule{
        .name = std::move( name ), .file = file, .residency = {}, .path = std::string{ sources.pathOf( file ) } } );
  }

  // One Phase, unnamed, with everything in it: the same Project a document
  // with no `phase` block describes, so nothing downstream can tell them
  // apart.
  Phase everything;
  for ( std::uint32_t module = 0; module < project.modules.size(); ++module )
  {
    everything.needs.push_back( ModuleIndex{ module } );
  }
  project.phases.phases.push_back( std::move( everything ) );
  project.phases.entry = PhaseIndex{ 0 };
  deriveResidency( project );
  if ( holdsC( sources, project ) )
  {
    addCRuntime( project, sources );
  }

  return project;
}

bool imagesStayInOneUnit( Project const& project )
{
  return project.target.bankSize() > 0 &&
         ( ( project.driver.has_value() && project.driver->spansNothing ) || !project.target.unitUsable.empty() );
}

} // namespace nga::model
