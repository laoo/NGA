#pragma once

#include "nga/diag/SourceManager.hpp"
#include "nga/model/Chunk.hpp"
#include "nga/model/Merge.hpp"
#include "nga/syntax/Expression.hpp"
#include "nga/syntax/Operand.hpp"

#include <cstdint>
#include <string_view>

namespace nga::model
{

/// What a Chunk does with what a Reference of it names. See the glossary.
///
/// A Chunk is one statement, so every Reference it holds is of one kind, and
/// the kind is a function of the statement and not of the expression: `jsr`
/// calls, `jmp` and a branch jump, a load reads, a store writes, a shift on
/// memory does both. ESCAPE is an address taken rather than used — an
/// immediate operand, a `.byte` or a `.word`, the size of a reservation —
/// after which the memory may be reached by code no Reference names. An
/// indirect operand reads the pointer it names, and what the pointer points
/// at is what escaped.
enum class ReferenceKind : std::uint8_t
{
  CALL,
  JUMP,
  READ,
  WRITE,
  READ_WRITE,
  ESCAPE,
};

std::string_view nameOf( ReferenceKind kind );

/// The kind of every Reference an instruction holds, from what the mnemonic
/// does to memory and the shape its operand was written in. A mnemonic the
/// ISA does not have, or one that takes no operand, is reported by the
/// encoder; here it escapes, which is the kind that assumes the least.
[[nodiscard]] ReferenceKind referenceKindOf( std::string_view mnemonic, syntax::OperandShape shape );

/// The kind of every Reference a Chunk holds. The Chunks of the Target's own
/// kinds jump: a `.transition` to the routine and the Phase it enters, a
/// Cell to what the edges write into it.
[[nodiscard]] ReferenceKind referenceKindOf( diag::SourceManager const& sources, Chunk const& chunk );

/// What one expression of a Chunk reaches, told to whoever asked.
///
/// A Reference is a use of a Symbol inside a Chunk, and two Steps ask about
/// the same ones: the type check, for where a Reference may point and where it
/// holds a Movable Section still, and Prune, for what is kept. One walk serves
/// both, so that they cannot disagree about which names reach memory — a
/// disagreement that would show up exactly on `x.runtimeSectionSize`, which
/// reaches nothing, against `x.runtimeSectionAddress`, which does.
class ReferenceVisitor
{
public:
  ReferenceVisitor() = default;
  ReferenceVisitor( ReferenceVisitor const& ) = delete;
  ReferenceVisitor( ReferenceVisitor&& ) = delete;
  ReferenceVisitor& operator=( ReferenceVisitor const& ) = delete;
  ReferenceVisitor& operator=( ReferenceVisitor&& ) = delete;
  virtual ~ReferenceVisitor() = default;

  /// A name that resolved to a Label, a Section or a Slot: what the Chunk
  /// encodes an address of, or the Cell it reaches one through.
  virtual void reference( SymbolRef target ) = 0;

  /// A local label, which is a position in the referrer's own Module.
  virtual void localReference( LabelPosition /*target*/ ) {}
};

/// Walks one expression as the Module `from` reads it, and reports every
/// Symbol it names that reaches memory.
///
/// A Constant is followed to what it was defined as, read in the Module that
/// defined it, because the Chunk using `operand = label + 1` encodes `label`;
/// a cycle among Constants has been reported by typing and is not followed
/// here. A Section's size and its Residency are facts and not memory, so
/// `.runtimeSectionSize` and `.resident` reach nothing; `.runtimeSectionAddress` reaches the
/// Section. A name that resolves to nothing has been reported by typing and is
/// skipped. Charsets are not memory.
void walkReferences( GlobalSymbols const& symbols,
                     diag::SourceManager const& sources,
                     ModuleIndex from,
                     syntax::Expression const& node,
                     ReferenceVisitor& visitor );

} // namespace nga::model
