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
#include <string>
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

/// The Sections a Transition from `from` to `to` loads: those that emit bytes
/// in the Modules `to` needs and `from` does not — the load set of 0016 — in
/// Project order. The one definition of it: PlaceStorage reads it for what
/// waits in a Bank, and Size for how long a table is.
/// `from` absent is the **cold start**: no Phase was left, so nothing was kept
/// and everything the entered Phase needs that emits bytes is in the set.
std::vector<SectionRef>
loadSetOf( PhaseGraph const& graph, std::optional<PhaseIndex> from, PhaseIndex to, std::span<Module const> modules );

/// A Phase's entry Label, resolved against the Modules it needs — private or
/// exported, since the Project stands above privacy. Nothing where there is
/// not exactly one Label of that name, which is reported where an address is
/// wanted and not here: Prune asks this before any Container does.
std::optional<SymbolRef> entrySymbolOf( PhaseGraph const& graph, PhaseIndex phase, GlobalSymbols const& symbols );

/// Where a Phase starts: its entry Label, required to be exactly one Label.
/// Nothing when reported, or when an earlier Step has already failed to place
/// it.
std::optional<std::uint32_t> entryAddressOf( Placed const& build, PhaseIndex phase, diag::DiagnosticSink& sink );

/// Where a Container starts the program: the entry Phase's entry, as
/// `entryAddressOf` has it. When some edge enters that Phase, Patch has
/// resolved the same entry for a table and reported whatever was wrong with
/// it, so it is not reported a second time here.
std::optional<std::uint32_t> startAddressOf( Placed const& build, diag::DiagnosticSink& sink );

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

/// The Label of its tail, which the routine falls through into and which the
/// cold start jumps to: everything from the Frame's address onward, and where
/// the jump through the entry the Frame hands it stands — see
/// docs/decisions/0216-a-car-names-its-format-and-the-cold-start-is-an-edge.md.
constexpr std::string_view TRANSITION_ENTER_NAME = "ngaEnter";

/// The Proc a Container with no loader points its run vector at, and the three
/// bytes it reads the cold start's Frame from, which that Container patches.
constexpr std::string_view COLD_START_NAME = "ngaStart";
constexpr std::string_view COLD_FRAME_NAME = "ngaColdFrame";

/// What a Container that loads through memory writes and calls: the cell it
/// names a unit in, and the glue that hands the cell to the driver's `map`.
/// Both in the dispatcher's Module, exported — see docs/spec/xex.md.
constexpr std::string_view LOAD_UNIT_NAME = "ngaLoadUnit";
constexpr std::string_view LOAD_MAP_NAME = "ngaLoadMap";

/// What a `.xex` without storage writes and calls to decode a Section while it
/// loads: two words naming where the form stands and where it goes, and per
/// format the glue that hands them to the decoder — see
/// docs/decisions/0235-a-xex-decodes-in-place-while-it-loads.md.
constexpr std::string_view UNPACK_FROM_NAME = "ngaUnpackFrom";
constexpr std::string_view UNPACK_TO_NAME = "ngaUnpackTo";

/// What moves bytes under a ROM while a `.xex` loads, and the cell its batch's
/// table is named in — see
/// docs/decisions/0241-bytes-kept-for-a-later-phase-may-lie-hidden-under-a-rom.md.
constexpr std::string_view UNDER_NAME = "ngaUnder";
constexpr std::string_view UNDER_TABLE_NAME = "ngaUnderTable";

/// The ROMs the decode under a ROM puts back once it is done, which the
/// `.xex` writes once before the first.
constexpr std::string_view UNDER_PUT_BACK_NAME = "ngaUnderPutBack";

/// What a `.xex` built from a `choose` block chooses with and changes its
/// bytes with while it loads: the byte the program writes the Choice into, the
/// routine the file calls, and the two words that say which of the list's
/// entries it takes this time — see
/// docs/decisions/0302-a-document-may-be-chosen-while-a-xex-loads.md.
constexpr std::string_view CHOICE_NAME = "ngaChoice";
constexpr std::string_view PATCH_NAME = "ngaPatch";
constexpr std::string_view PATCH_FROM_NAME = "ngaPatchFrom";
constexpr std::string_view PATCH_TO_NAME = "ngaPatchTo";

/// The glue of a format: `ngaUnpack` and the format's name with its first
/// letter raised, `ngaUnpackZx0`.
std::string loadGlueName( std::string_view format );

/// The glue of a format that decodes a Section hidden under a ROM: the same
/// decoder, called with every ROM out and the interrupts masked —
/// `ngaUnderUnpack` and the format's name with its first letter in capitals.
std::string underGlueName( std::string_view format );

/// The driver's `init`, wrapped in a Proc so that a Container has an address
/// to call: what the hardware takes before the driver can be used at all.
/// Present only where the driver declares the role.
constexpr std::string_view INIT_NAME = "ngaInit";

/// What a Container whose program can return starts it through, where the
/// driver declares `finalize`: `ngaRun` in the entry Phase, which leaves the
/// return to `ngaFinish` on the stack and jumps to the entry, and `ngaFinish`,
/// resident, which expands the role and returns to whatever started the
/// program — see docs/decisions/0229-no-window-has-a-base.md.
constexpr std::string_view RUN_NAME = "ngaRun";
constexpr std::string_view FINISH_NAME = "ngaFinish";

/// The runtime address of a Label, or nothing where it is not one or has no
/// address yet.
std::optional<std::uint32_t>
addressOfLabel( GlobalSymbols const& symbols, SymbolRef where, Sizes const& sizes, Layout const& layout );

/// What a `.transition` emits before its entries — `jsr routine`, the Phase
/// entered, the entry count — and what one entry costs: the Phase the code
/// is in, and the unit and offset where the edge's Frame waits, the unit as
/// wide as the Target's.
/// The layout is docs/spec/transition.md; these are here because Size needs
/// them before a byte of it is written, and the routine's text is given them
/// from here, so that what it steps over and what Patch writes are one fact.
constexpr std::uint32_t TRANSITION_HEADER_SIZE = 5;

constexpr std::uint32_t transitionEntrySize( std::uint32_t unitBytes )
{
  return 3 + unitBytes;
}

/// The bytes a `.transition` in a Section of this Residency emits: an entry
/// per Phase the Section is present in, since at run time the code does not
/// know which one it is in.
std::uint32_t sizeOfTransition( Residency const& residency, std::uint32_t unitBytes );

/// What a Frame holds before its blocks — the entered Phase's entry and the
/// block count — what one block costs (the unit, the offset, the
/// destination, the decoder's number), and after the blocks one byte
/// counting the Cell writes and this many bytes each.
constexpr std::uint32_t FRAME_HEADER_SIZE = 3;

constexpr std::uint32_t frameBlockSize( std::uint32_t unitBytes )
{
  return 5 + unitBytes;
}

/// Two bytes in front of that where the Project said `frames held`: the
/// address the descriptors are read into, or **zero** for an edge whose
/// entered Phase had no room to lend. A program that did not ask for it has
/// no such bytes at all — the routine is generated knowing the answer, so
/// there is nothing for it to read past, and the cheap case stays exactly the
/// size it was.
constexpr std::uint32_t FRAME_HELD_SIZE = 2;

/// The header a Frame of this Project has.
constexpr std::uint32_t frameHeaderSize( bool held )
{
  return held ? FRAME_HEADER_SIZE + FRAME_HELD_SIZE : FRAME_HEADER_SIZE;
}

/// The two bytes a stored form begins with: its own size, which the decoder
/// reads before the data — see docs/spec/transition.md.
constexpr std::uint32_t FORM_HEADER_SIZE = 2;
constexpr std::uint32_t CELL_WRITE_SIZE = 4;

/// A Frame counts its blocks and its Cell writes in a byte each.
constexpr std::uint32_t MOST_BLOCKS = 255;

/// The bytes of an edge's Frame: sized for every Section the edge may have
/// to load and every Slot it may have to write, since a shorter list leaves
/// the rest unread.
std::uint32_t
sizeOfFrame( std::uint32_t payloads, std::uint32_t cellWrites, std::uint32_t headerBytes, std::uint32_t unitBytes );

/// The header a Frame of this Project has: the entry and the count, two bytes
/// more under `frames held`, and one more for the set of ROMs the entered Phase
/// needs where the driver declares `select` — see
/// docs/decisions/0229-no-window-has-a-base.md.
std::uint32_t frameHeaderSizeOf( Project const& project );

/// The set of ROMs a Phase needs, as the driver's `select` is handed it: bit
/// *k* for the *k*-th of the driver's `rom` lines whose Module the Phase needs.
std::uint32_t romsOf( Project const& project, PhaseIndex phase );

/// How many bytes a state of this Window takes wherever the tool writes or
/// passes one: two where the driver's `showAt` for it takes the high byte in
/// `Y`, one otherwise and where no driver was resolved — see
/// docs/decisions/0228-a-driver-names-the-cells-its-stream-stands-in.md.
std::uint32_t stateBytesOf( Project const& project, WindowIndex window );

/// Whether the Container writes bytes into a named state of this Window
/// while the program loads: only a `.xex`, and only into the Window its
/// driver's stream reads through, since what shows a state there is the
/// loader's glue around that Window's `showAt` — see
/// docs/decisions/0256-a-named-state-is-filled-where-a-xex-can-show-it.md.
bool fillsNamedStates( Project const& project, WindowIndex window );

/// What a diagnostic calls a Frame: the edge it belongs to.
/// What a finding and the map call a Frame. The cold start comes from no
/// Phase and is named for what it is.
std::string nameOfFrame( PhaseGraph const& graph, std::optional<PhaseIndex> from, PhaseIndex to );

/// One block of an edge: a stored form of a Payload, and which of them.
struct EdgeBlock
{
  SectionRef payload;
  std::uint32_t piece = 0;
};

/// The blocks an edge loads, **in the order the routine loads them**: the
/// edge's load set, one block per stored form, and — where the
/// Project said `frames held` — the largest of them moved to the end, since
/// that is the block whose memory lends the routine its list of descriptors
/// and it has to arrive after the rest. PlaceStorage packs the images in this
/// order and Patch writes the Frame in it, so the two cannot drift apart.
///
/// Answered from Layout and from the sizes of the stored forms, which are
/// known before an image is placed: nothing here may ask where one waits.
std::vector<EdgeBlock> edgeBlocksOf( FrameIndex frame, Placed const& build, Storage const& storage );

/// The Frame of one edge, as the routine reads it from its Bank: the entered
/// Phase's entry, a block per Payload the edge copies, and the Cell writes.
/// Reads Layout and Storage and never Sizes.
std::vector<std::uint8_t>
frameBytesOf( FrameIndex frame, Placed const& build, Storage const& storage, EntryAddresses& entries );

/// The byte naming the current Phase, and the name it is exported under.
constexpr std::uint32_t TRANSITION_CELL_SIZE = 1;
constexpr std::string_view TRANSITION_CELL_NAME = "ngaCurrentPhase";

/// Where a program without storage enters a Phase: one stub per Phase some
/// `.transition` enters, exported under this prefix and the Phase's number,
/// in a Module the tool adds at the end of Assemble. A `.transition` there is
/// a `jmp` to it and nothing more — see
/// docs/decisions/0234-without-storage-bytes-stay-where-they-are-live.md.
constexpr std::string_view ENTRY_STUB_PREFIX = "ngaInto";
constexpr std::uint32_t TRANSITION_JUMP_SIZE = 3;

/// The name of the stub of a Phase.
std::string entryStubName( PhaseIndex phase );

/// The driver's `select`, wrapped in a Proc so that a stub Patch writes byte
/// by byte has an address to call it at. Present only in a program without
/// storage whose driver declares the role.
constexpr std::string_view SELECT_ROMS_NAME = "ngaSelectRoms";

/// The bytes of a stub: `lda` and `jsr` to select the ROMs where the driver
/// switches any, `lda` and `sta` twice for each Slot, the Phase's number into
/// the Cell, and the `jmp` to the entry. Sized for every Slot, as a Frame is;
/// a Slot the Phase has no Implementation of is not written, and what is left
/// stands after the `jmp`, where nothing reaches it.
std::uint32_t sizeOfEntryStub( Project const& project, std::uint32_t slots );

/// The end of Assemble for a program without storage: a stub for every Phase
/// some resolved `.transition` enters, present wherever such a statement runs
/// and in every Phase from which one of those is reached, and the Proc around
/// the driver's `select` where it declares one. Nothing for a program with
/// storage, whose edges the routine takes.
void addEntryStubs( diag::SourceManager& sources,
                    Project& project,
                    std::vector<Module>& modules,
                    diag::DiagnosticSink& sink );

/// A stub's bytes, as sizeOfEntryStub lays them out; `entry` is the entered
/// Phase's, or nothing where that was reported.
void writeEntryStub( PhaseEntryContent const& content,
                     Placed const& build,
                     std::optional<std::uint32_t> entry,
                     std::span<std::uint8_t> into );

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
