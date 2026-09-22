#include "nga/model/Slots.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <variant>

namespace nga::model
{

std::vector<SymbolRef> slotsOf( std::span<Module const> modules )
{
  std::vector<SymbolRef> slots;
  for ( std::uint32_t module = 0; module < modules.size(); ++module )
  {
    std::vector<Symbol> const& symbols = modules[module].symbols().symbols();
    for ( std::uint32_t index = 0; index < symbols.size(); ++index )
    {
      if ( symbols[index].kind == SymbolKind::SLOT )
      {
        slots.push_back( SymbolRef{ .module = ModuleIndex{ module }, .index = index } );
      }
    }
  }
  return slots;
}

void addSlotCells( Project& project, std::vector<Module>& modules, diag::DiagnosticSink& sink )
{
  std::vector<SymbolRef> const slots = slotsOf( modules );
  if ( slots.empty() )
  {
    return;
  }
  if ( slots.size() > MOST_SLOTS )
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::TOO_MANY_SLOTS )
                  .arg( "count", static_cast<std::int64_t>( slots.size() ) )
                  .sortedBy( "slots" ) );
  }

  // One Module, one Section per Slot, each with the placement the Slot
  // declared, standing where the Slot's own declaration does. It borrows the
  // first Slot's file, since a Module has one and nothing reads it.
  std::size_t const phaseCount = project.phases.phases.size();
  ModuleIndex const cellsIndex{ static_cast<std::uint32_t>( modules.size() ) };
  Module cells{ "nga.slots", project.modules[slots.front().module.value].file, Residency::all( phaseCount ) };

  for ( SymbolRef const slot : slots )
  {
    Symbol& symbol = modules[slot.module.value].symbols().at( slot.index );
    auto& declaration = std::get<SlotDeclaration>( symbol.value );
    Section section{ declaration.placement, nullptr,          nullptr, nullptr, false, false,
                     SectionKind::PLAIN,    symbol.definition };
    section.appendChunk( SlotCellContent{ .module = slot.module, .symbol = slot.index, .binding = declaration.binding },
                         symbol.definition,
                         {} );
    declaration.cell = SectionRef{ .module = cellsIndex, .section = cells.addSection( std::move( section ) ) };
  }

  // Outside the Window, as every Cell is: the routine writes it with the
  // driver's stream open, which on a mapped driver is a Bank in.
  cells.keepOutsideWindow();
  modules.push_back( std::move( cells ) );
  project.modules.push_back( ProjectModule{ .name = "nga.slots",
                                            .file = project.modules[slots.front().module.value].file,
                                            .residency = Residency::all( phaseCount ),
                                            .generated = Generated::SLOT_CELLS,
                                            .outsideWindow = true } );
  for ( Phase& phase : project.phases.phases )
  {
    if ( std::ranges::find( phase.needs, cellsIndex ) == phase.needs.end() )
    {
      phase.needs.push_back( cellsIndex );
    }
  }
  project.slotCells = cellsIndex;
}

std::optional<SymbolRef> implementationIn( GlobalSymbols const& symbols, SymbolRef slot, PhaseIndex phase )
{
  for ( ResolvedImplementation const& implementation : symbols.implementationsOf( slot ) )
  {
    Residency const& residency = symbols.moduleAt( implementation.target.module ).residency();
    if ( residency.phaseCount() > phase.value && residency.includes( phase ) )
    {
      return implementation.target;
    }
  }
  return std::nullopt;
}

std::optional<std::uint32_t> addressOfImplementation( Placed const& build, SymbolRef target, PhaseIndex phase )
{
  Layout const& layout = build.layout();
  Symbol const& symbol = build.symbols().at( target );
  if ( auto const* const label = std::get_if<LabelPosition>( &symbol.value ); label != nullptr )
  {
    SectionRef const where{ .module = target.module, .section = label->section };
    if ( !layout.isPlaced( where ) || !build.sizes().isKnown( where ) )
    {
      return std::nullopt;
    }
    return layout.addressIn( where, phase ) + build.sizes().offsetOf( where, label->chunk );
  }
  return std::nullopt;
}

} // namespace nga::model
