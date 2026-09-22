#pragma once

#include "nga/diag/SourceLocation.hpp"
#include "nga/model/Binding.hpp"
#include "nga/model/Project.hpp"
#include "nga/syntax/Operand.hpp"
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

/// A macro use: `NAME [expr, ...]`, its arguments the Chunk's items. Expand
/// instantiates the macro's body for those arguments, and the instructions
/// and data that come of it are the Chunk's **inner** Chunks,
/// kept by the Section beside the arena their expressions live in. One
/// statement, one Chunk, with an inside nothing outside can name: a nested
/// use inside the body is expanded in place, so the inside is flat. See
/// docs/decisions/0043-macros.md.
/// What a `.with` wrote, and which of the two Chunks it became: the one
/// before the statement, showing what it names, or the one after, showing
/// again what was shown where the statement stands — the Section's own
/// Pane, or the Window's base. See docs/decisions/0055-with.md and
/// 0063-with-shows-again-what-was-shown.md.
enum class WithForm : std::uint8_t
{
  NONE,

  /// `.with PANE`: the state the solver gave the Pane.
  SHOW,

  /// `.with FAMILY, x` or `.with WINDOW, x`: the state whose index is in X.
  AT,

  /// `.with WINDOW = STATE`: a named state of the Window.
  STATE,
};

enum class WithSide : std::uint8_t
{
  NONE,
  ENTER,
  LEAVE,
};

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
  WithForm form = WithForm::NONE;
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

/// A Slot's Cell: what a Reference to the Slot reaches, rewritten by the
/// routine on every edge, and holding the entry Phase's Implementation when
/// the program starts. Names the Slot's Symbol by position, since a Symbol
/// reference is defined above Chunks. See docs/decisions/0031-slots.md.
struct SlotCellContent
{
  ModuleIndex module;
  std::uint32_t symbol = 0;
  Binding binding = Binding::POINTER;
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

using ChunkContent = std::variant<InstructionContent,
                                  MacroUseContent,
                                  DataContent,
                                  ReserveContent,
                                  TransitionContent,
                                  TransitionCellContent,
                                  SlotCellContent,
                                  DispatchContent>;

/// Who follows an address a Chunk takes — see Chunk::taking.
enum class Taking : std::uint8_t
{
  ESCAPE,
  OWN,
  ROOT,
};

/// What a `.declare` says the `.ztemp` below it is to the Proc holding it:
/// one of its arguments, in the order the declarations stand in, or its
/// result — see docs/decisions/0081-a-procs-signature-is-declared.md.
enum class Declaring : std::uint8_t
{
  ARGUMENT,
  RESULT,
};

/// The type a declared byte of a Proc holds, where one was written. The
/// types of the C subset, and `u8[N]` for a run of bytes. Optional, because
/// the shape of the reservation answers where nothing was written, and held
/// to that shape where both are present.
struct DeclaredType
{
  enum class Kind : std::uint8_t
  {
    U8,
    I8,
    U16,
    I16,
    BOOL,
    BYTES,
  };

  Kind kind = Kind::U8;

  /// How many elements `u8[N]` has; one for every other kind.
  std::uint32_t count = 1;
  diag::SourceSpan span{};

  friend bool operator==( DeclaredType const& left, DeclaredType const& right )
  {
    return left.kind == right.kind && left.count == right.count;
  }

  /// How many bytes holding the type takes, which is what the reservation
  /// under the declaration is held to.
  [[nodiscard]] std::uint32_t bytes() const
  {
    switch ( kind )
    {
    case Kind::U16:
    case Kind::I16:
      return 2;
    case Kind::BYTES:
      return count;
    case Kind::U8:
    case Kind::I8:
    case Kind::BOOL:
      break;
    }
    return 1;
  }
};

/// How a declared type is written, which is how a finding names it.
[[nodiscard]] std::string spellingOf( DeclaredType const& type );

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
  Taking taking = Taking::ESCAPE;

  /// Under `.own NAME [, NAME]...`: the Sections that follow the address,
  /// by the names written, resolved where they are read; empty for `.own`
  /// alone, which is this Section. The one thing a Chunk holds of its own,
  /// and empty for all but a handful.
  std::vector<syntax::Token> followers{};
};

} // namespace nga::model
