#pragma once

#include "nga/diag/DiagnosticSink.hpp"
#include "nga/model/Build.hpp"
#include "nga/model/Merge.hpp"
#include "nga/model/Module.hpp"
#include "nga/model/Place.hpp"
#include "nga/model/Project.hpp"
#include "nga/model/Size.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace nga::model
{

/// A Frame counts an edge's Cell writes in a byte.
constexpr std::uint32_t MOST_SLOTS = 255;

/// Every Slot of the program, in Project order: Modules in order, Symbols in
/// definition order. The order every list of Cells and Cell writes follows.
std::vector<SymbolRef> slotsOf( std::span<Module const> modules );

/// The end of Assemble for a program with a Slot: builds the Module holding
/// one Cell per Slot — Resident, since a Cell is read from every Phase — and
/// gives each Slot its Cell. Nothing when the program declares no Slot. See
/// docs/decisions/0031-slots.md.
void addSlotCells( Project& project, std::vector<Module>& modules, diag::DiagnosticSink& sink );

/// The Implementation live in a Phase: the one whose Module the Phase needs,
/// or nothing where no Module implementing the Slot is present.
std::optional<SymbolRef> implementationIn( GlobalSymbols const& symbols, SymbolRef slot, PhaseIndex phase );

/// Where an Implementation stands in a Phase — a Label's position, a
/// Section's start — or nothing when Place gave it none.
std::optional<std::uint32_t> addressOfImplementation( Placed const& build, SymbolRef target, PhaseIndex phase );

} // namespace nga::model
