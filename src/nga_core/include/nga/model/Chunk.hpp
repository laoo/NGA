#pragma once

#include "nga/diag/SourceLocation.hpp"
#include "nga/model/Indices.hpp"
#include "nga/syntax/Operand.hpp"
#include "nga/syntax/Statement.hpp"
#include "nga/syntax/Token.hpp"

#include <cstdint>
#include <optional>
#include <variant>
#include <vector>

namespace nga::model
{

/// A Chunk's position within its Section. An index equal to the Chunk count is
/// legal and names the position after the last Chunk, which is what lets a
/// Label stand at the end of a Section.
struct ChunkIndex
{
  std::uint32_t value = 0;

  friend bool operator==( ChunkIndex, ChunkIndex ) = default;
};

/// A Section's position within its Module.
struct SectionIndex
{
  std::uint32_t value = 0;

  friend bool operator==( SectionIndex, SectionIndex ) = default;
};

/// Which Section, across the whole program. Defined beside the Chunk because a
/// Chunk of the Target's kind — a Transition table — names Sections.
struct SectionRef
{
  ModuleIndex module;
  SectionIndex section;

  friend bool operator==( SectionRef, SectionRef ) = default;
};

/// One instruction. The mnemonic stays a Token because the ISA is reached by
/// text and because a diagnostic underlines it; the shape is what the grammar
/// saw and not the addressing mode, which the encoder decides. A mnemonic
/// the ISA does not have is a macro use with one argument or none, which
/// Expand turns into a MacroUseContent once every Module's macros are in
/// hand — the grammar cannot tell the two apart and does not try.
struct InstructionContent
{
  syntax::Token mnemonic;
  syntax::OperandShape shape = syntax::OperandShape::NONE;
};

/// Which of the two Chunks a `.with` became: the one before the statement,
/// showing what it names, or the one after, showing again the Pane a Proc
/// runs under where it is one of that Window, and nothing anywhere else. See
/// docs/decisions/0068-with-shows-again-what-was-shown.md.
enum class WithSide : std::uint8_t
{
  NONE,
  ENTER,
  LEAVE,
};

/// A macro use: `NAME [expr, ...]`, its arguments the Chunk's items. Expand
/// instantiates the macro's body for those arguments, and the instructions
/// and data that come of it are the Chunk's **inner** Chunks,
/// kept by the Section beside the arena their expressions live in. One
/// statement, one Chunk, with an inside nothing outside can name: a nested
/// use inside the body is expanded in place, so the inside is flat. See
/// docs/decisions/0043-macros.md.
struct MacroUseContent
{
  /// The Namespaces before the name, where the use was qualified: `nga`,
  /// the tool's, holds the driver's roles, and any other is one the program
  /// opened — see docs/decisions/0044-roles-are-macros.md and 0045.
  std::vector<syntax::Token> path;
  syntax::Token name;

  /// Set for the two Chunks a `.with` puts around a statement: `name` is
  /// then the directive's token, the first item names the Pane, family or
  /// Window, and Expand chooses the driver's `show` or `showAt` for its
  /// Window and the argument the side needs.
  syntax::WithForm form = syntax::WithForm::NONE;
  WithSide side = WithSide::NONE;

  /// The named state after `=`, for STATE; a state is no Symbol, so it is
  /// not an item.
  std::optional<syntax::Token> state{};

  /// What Expand resolved the `.with` to, for the closure check that runs
  /// after it: the Window shown, and the Pane where one is — a family's for
  /// AT, the member's for SHOW; absent where the state is named or unknown.
  std::optional<WindowIndex> window{};
  std::optional<PaneIndex> pane{};
  std::optional<std::uint32_t> shownState{};
};

struct DataContent
{
  syntax::DataWidth width = syntax::DataWidth::BYTE;
};

/// Space that occupies addresses and emits nothing. Which kind a Chunk is gets
/// recorded here and read by Size, which derives from it how far a Section's
/// initialised bytes reach — see
/// docs/decisions/0026-a-payload-is-the-initialised-extent.md.
struct ReserveContent
{
};

/// `.transition NAME`: what the Target emits to enter a Phase — a call into
/// the routine followed by the Phase entered and, per Phase the Section is
/// present in, where the edge's Frame waits. The name is resolved to a Phase
/// at the end of Assemble, once the whole Project is in hand — see
/// docs/decisions/0037-the-statement-names-its-frames.md.
struct TransitionContent
{
  syntax::Token name;
  std::optional<PhaseIndex> target;

  /// Written `.transition NAME, fast`: every image this edge opens begins at a
  /// unit boundary, so that a driver which reaches an offset by reading up to
  /// it never has to. Silence is the Intent's to fill. See
  /// docs/decisions/0221-an-edge-that-must-be-quick.md.
  bool fast = false;
};

/// Which Frame, across the whole program: the edges some `.transition`
/// takes, numbered in the order the statements are met, since a Frame is a
/// thing in storage and Storage is keyed by it.
struct FrameIndex
{
  std::uint32_t value = 0;

  friend bool operator==( FrameIndex, FrameIndex ) = default;
};

/// The byte that names the current Phase, holding the entry Phase's number
/// when the program starts and written by the routine on every Transition.
struct TransitionCellContent
{
};

/// Where a program without storage enters a Phase: a jump from every
/// `.transition` into it lands here, and it does what a Frame would have said
/// — the ROMs the Phase needs selected, the Slots' Cells written, the Phase's
/// number put in the Cell — and jumps to the Phase's entry. Code, written by
/// Patch as a Frame is, since what it writes is addresses only the Layout
/// knows. See docs/decisions/0234-without-storage-bytes-stay-where-they-are-live.md.
struct PhaseEntryContent
{
  PhaseIndex phase;
};

/// A Slot's Cell: what a Reference to the Slot reaches, rewritten by the
/// routine on every edge, and holding the entry Phase's Implementation when
/// the program starts. Names the Slot's Symbol by position, since a Symbol
/// reference is defined above Chunks. See docs/decisions/0031-slots.md.
struct SlotCellContent
{
  ModuleIndex module;
  std::uint32_t symbol = 0;
  syntax::Binding binding = syntax::Binding::POINTER;
};

/// `.dispatch TARGET [, TARGET]...`: control goes to one of the positions
/// named, chosen by the value in `A`, which the statement before it has held
/// below their number. The targets are positions of this Chunk's own Section,
/// so where one goes is known and the Section keeps the liveness of ordinary
/// code; a position of another Section is `.own`'s, not this statement's. A
/// target may stand more than once, which is how a `switch`'s holes and its
/// `default` are written. How it is encoded is the Target's — one table of
/// addresses jumped through where the processor has `jmp (abs,x)` and the
/// count fits its index, two half tables and the `rts` trick otherwise — see
/// docs/decisions/0191-a-dispatch-goes-to-one-of-its-own-positions.md.
struct DispatchContent
{
};

/// `.release NAME`: the Holder named holds nothing from here. Emits nothing;
/// the Trace reads it — see
/// docs/decisions/0247-the-hardware-holds-a-section-for-as-long-as-it-is-given-it.md.
struct ReleaseContent
{
  syntax::Token holder;
};

using ChunkContent = std::variant<InstructionContent,
                                  MacroUseContent,
                                  DataContent,
                                  ReserveContent,
                                  TransitionContent,
                                  TransitionCellContent,
                                  SlotCellContent,
                                  DispatchContent,
                                  PhaseEntryContent,
                                  ReleaseContent>;

/// The bytes of exactly one emitting statement, as a description of them.
///
/// It carries neither a size nor an address: sizes are the result of Size and
/// addresses the result of Place, both held apart from the Chunks — see
/// docs/decisions/0012-step-results-are-separate-objects.md. Its expressions
/// live in the Section's arena, so a Chunk owns nothing and a vector of them
/// grows by copying bytes.
struct Chunk
{
  ChunkContent content;
  diag::SourceSpan span;

  /// Half-open range into the Section's item arena.
  std::uint32_t firstItem = 0;
  std::uint32_t itemCount = 0;

  /// Who follows an address this Chunk takes: nobody the tool knows, which
  /// is an escape; this Section alone, under `.own`, which is a jump to
  /// code and a use of a Temporary; or the hardware, under `.root`, which
  /// makes what is named a Root — see docs/decisions/0060-own.md and
  /// docs/decisions/0061-root-at-the-taking.md.
  syntax::Taking taking = syntax::Taking::ESCAPE;

  /// Under `.own NAME [, NAME]...`: the Sections that follow the address,
  /// by the names written, resolved where they are read; empty for `.own`
  /// alone, which is this Section. The one thing a Chunk holds of its own,
  /// and empty for all but a handful.
  std::vector<syntax::Token> followers{};

  /// Under `.alt`: the mnemonics this instruction's opcode may be replaced
  /// with at run time, each in the instruction's own mode — see
  /// docs/decisions/0237-an-instruction-declares-the-opcodes-it-may-be-given.md.
  std::vector<syntax::Token> alternatives{};
};

} // namespace nga::model
