#pragma once

#include "nga/diag/DiagnosticSink.hpp"
#include "nga/diag/SourceManager.hpp"
#include "nga/model/Build.hpp"
#include "nga/model/Merge.hpp"
#include "nga/model/Module.hpp"
#include "nga/model/Place.hpp"
#include "nga/model/Project.hpp"
#include "nga/model/Size.hpp"
#include "nga/model/Storage.hpp"
#include "nga/model/Transform.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace nga::model
{

/// Adds to a Project whose PhaseGraph has an edge the Modules that take its
/// Transitions: the Target's routine and the Cell. Lists them in every
/// Phase's `needs` and derives Residency again, so that nothing about them is
/// declared. A Project with no edge is left alone. See
/// docs/decisions/0019-transition-mechanism.md and
/// docs/decisions/0037-the-statement-names-its-frames.md.
void addTransitionModules( Project& project, diag::SourceManager& sources, diag::DiagnosticSink& sink );

/// Builds a Module the generator added and the assembler cannot: the Cell,
/// one Section of one Chunk of the Target's kind.
Module buildGeneratedModule( diag::SourceManager const& sources, ProjectModule const& entry );

/// The end of Assemble for a Project with Transitions: resolves every
/// `.transition` to its Phase and checks it against the edges, and holds
/// every edge's load set to what a Frame can count. Needs every Module
/// assembled, because a load set is a set of Sections.
void resolveTransitions( diag::SourceManager const& sources,
                         Project const& project,
                         std::span<Module> modules,
                         diag::DiagnosticSink& sink );

/// The Sections a Transition from `from` to `to` may have to load: those that
/// emit bytes in the Modules `to` needs and `from` does not — the load set of
/// 0016 — and every Movable Section present in both, which the edge copies
/// again wherever the two Phases hold it at different addresses; in Project
/// order. The one definition of it: PlaceStorage reads it for what waits in a
/// Bank, Size for how long a table may be, and a table lists of it what the
/// Layout then says moved — see docs/decisions/0030-movable-sections.md.
std::vector<SectionRef>
loadSetOf( PhaseGraph const& graph, PhaseIndex from, PhaseIndex to, std::span<Module const> modules );

/// A Phase's entry Label, resolved against the Modules it needs — private or
/// exported, since the Project stands above privacy. Nothing where there is
/// not exactly one Label of that name, which is reported where an address is
/// wanted and not here: Prune asks this before any Container does.
std::optional<SymbolRef> entrySymbolOf( PhaseGraph const& graph, PhaseIndex phase, GlobalSymbols const& symbols );

/// Where a Phase starts: its entry Label, required to be exactly one Label.
/// Nothing when reported, or when an earlier Step has already failed to place
/// it.
std::optional<std::uint32_t> entryAddressOf( Placed const& build, PhaseIndex phase, diag::DiagnosticSink& sink );

/// Every Phase's entry address, resolved once and reported once however many
/// tables name it.
class EntryAddresses
{
public:
  EntryAddresses( Placed const& build, diag::DiagnosticSink& sink );

  std::optional<std::uint32_t> of( PhaseIndex phase );

private:
  Placed const* mBuild;
  diag::DiagnosticSink* mSink;

  /// Outer: resolved yet; inner: the address, or nothing when reported.
  std::vector<std::optional<std::optional<std::uint32_t>>> mResolved;
};

/// Whether some edge enters this Phase — in which case Patch has resolved and
/// reported its entry through a table, and the Container should not again.
bool isEnteredByAnEdge( PhaseGraph const& graph, PhaseIndex phase );

/// The exported Label the routine starts at, which a `.transition` jumps to.
constexpr std::string_view TRANSITION_ROUTINE_NAME = "ngaTransition";

/// What a Container that loads through memory writes and calls: the cell it
/// names a unit in, and the glue that hands the cell to the driver's `map`.
/// Both in the dispatcher's Module, exported — see docs/spec/xex.md.
constexpr std::string_view LOAD_UNIT_NAME = "ngaLoadUnit";
constexpr std::string_view LOAD_MAP_NAME = "ngaLoadMap";
constexpr std::string_view RESTORE_NAME = "ngaRestore";

/// The runtime address of a Label, or nothing where it is not one or has no
/// address yet.
std::optional<std::uint32_t>
addressOfLabel( GlobalSymbols const& symbols, SymbolRef where, Sizes const& sizes, Layout const& layout );

/// What a `.transition` emits before its entries — `jsr routine`, the Phase
/// entered, the entry count — and what one entry costs: the Phase the code
/// is in, and the unit and offset where the edge's Frame waits.
/// The layout is docs/spec/transition.md; these are here because Size needs
/// them before a byte of it is written.
constexpr std::uint32_t TRANSITION_HEADER_SIZE = 5;
constexpr std::uint32_t TRANSITION_ENTRY_SIZE = 4;

/// The bytes a `.transition` in a Section of this Residency emits: an entry
/// per Phase the Section is present in, since at run time the code does not
/// know which one it is in.
std::uint32_t sizeOfTransition( Residency const& residency );

/// What a Frame holds before its blocks — the entered Phase's entry and the
/// block count — what one block costs (the unit, the offset, the
/// destination, the decoder's number), and after the blocks one byte
/// counting the Cell writes and this many bytes each.
constexpr std::uint32_t FRAME_HEADER_SIZE = 3;
constexpr std::uint32_t FRAME_BLOCK_SIZE = 6;
constexpr std::uint32_t CELL_WRITE_SIZE = 4;

/// A Frame counts its blocks and its Cell writes in a byte each.
constexpr std::uint32_t MOST_BLOCKS = 255;

/// The bytes of an edge's Frame: sized for every Section the edge may have
/// to load and every Slot it may have to write, since a shorter list leaves
/// the rest unread, and one byte per Window of the Target for the base the
/// entered Phase gives it — see docs/decisions/0056-a-phase-chooses-a-base.md.
std::uint32_t sizeOfFrame( std::uint32_t payloads, std::uint32_t cellWrites, std::uint32_t windows );

/// The order the routine shows the entered Phase's bases in, and the Frame
/// lists them in: every Window of the Target in its order, the one the
/// stream reads through last, since showing its base takes the stream's
/// Bank away and nothing is read after it.
std::vector<WindowIndex> baseOrderOf( Project const& project );

/// What a diagnostic calls a Frame: the edge it belongs to.
std::string nameOfFrame( PhaseGraph const& graph, PhaseIndex from, PhaseIndex to );

/// The Frame of one edge, as the routine reads it from its Bank: the entered
/// Phase's entry, a block per Payload the edge copies, and the Cell writes.
/// Reads Layout and Storage and never Sizes.
std::vector<std::uint8_t>
frameBytesOf( FrameIndex frame, Placed const& build, Storage const& storage, EntryAddresses& entries );

/// The byte naming the current Phase.
constexpr std::uint32_t TRANSITION_CELL_SIZE = 1;

/// What Patch writes for the Target's kinds, in the layout
/// docs/spec/transition.md states. Each writes into `into` at offset zero and
/// no further than its size; an address an earlier Step failed to produce is
/// written as zero, since that Step has reported. A `.transition` names
/// where its Frames wait, which is Storage's answer, so it takes Storage.
void writeTransition( TransitionContent const& content,
                      ModuleIndex home,
                      Placed const& build,
                      Storage const& storage,
                      std::span<std::uint8_t> into );

void writeCell( Placed const& build, std::span<std::uint8_t> into );

/// A Slot's Cell as the program starts: the entry Phase's Implementation, or
/// zero where the entry Phase has none — and `jmp` in front of it for a
/// `vector`, which the routine never rewrites.
void writeSlotCell( SlotCellContent const& content, Placed const& build, std::span<std::uint8_t> into );

} // namespace nga::model
