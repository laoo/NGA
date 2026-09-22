#pragma once

#include "nga/model/PlacementClass.hpp"
#include "nga/syntax/Operand.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace nga::model
{

/// How an instruction reaches its operand.
///
/// This is what an instruction is **encoded** as, and it is not the shape the
/// grammar saw: `lda expr` is zero page or absolute depending on what `expr`
/// names. IMPLIED covers the accumulator forms too, because `asl` shifts the
/// accumulator and `asl a` is not a spelling this assembler has.
enum class AddressingMode : std::uint8_t
{
  IMPLIED,
  IMMEDIATE,
  ZERO_PAGE,
  ZERO_PAGE_X,
  ZERO_PAGE_Y,
  ABSOLUTE,
  ABSOLUTE_X,
  ABSOLUTE_Y,
  INDIRECT,
  INDEXED_INDIRECT,
  INDIRECT_INDEXED,
  RELATIVE,

  /// `(zp)` — the 65SC02's indirect with no index, which the 6502 has only
  /// with one. Two bytes.
  INDIRECT_ZERO_PAGE,

  /// `(abs,x)` — the 65SC02's indexed indirect jump, where the 6502 indexes
  /// only within the zero page. Three bytes.
  ABSOLUTE_INDEXED_INDIRECT,

  /// A Jcc out of a branch's reach: the opposite branch over three bytes, then
  /// `jmp` absolute. A mode of this assembler rather than of the processor,
  /// written with the opcode of its first byte — see
  /// docs/decisions/0077-a-jcc-is-two-bytes-or-five.md.
  BRANCH_OVER_JUMP,
};

std::string_view nameOf( AddressingMode mode );

/// How many bytes an instruction in this mode occupies, opcode included.
std::uint32_t sizeOf( AddressingMode mode );

struct Instruction
{
  std::string_view mnemonic;
  AddressingMode mode;
  std::uint8_t opcode;
};

/// Every legal 6502 instruction. Compiled in and reached only from here, so
/// the parser never learns that an ISA exists.
std::span<Instruction const> instructionTable();

[[nodiscard]] bool isMnemonic( std::string_view mnemonic );

/// Whether a mnemonic is one of the eight Jccs, which the processor does not
/// have: the Bcc of the same letters where the distance fits one, the opposite
/// Bcc over a `jmp` where it does not. `isMnemonic` is true of them, and
/// `opcodeOf` answers one in `RELATIVE` with its Bcc's opcode and in
/// `BRANCH_OVER_JUMP` with the opposite Bcc's.
[[nodiscard]] bool isJcc( std::string_view mnemonic );

/// The Jcc a Bcc is where the distance fits, which is what a Bcc out of range
/// is offered as. Nothing for any other mnemonic.
[[nodiscard]] std::optional<std::string_view> jccFor( std::string_view branch );

/// Whether a mnemonic is one of the **certain jumps**, `jmpeq` and its
/// fellows: a jump whose condition the writer knows holds, so both forms are
/// unconditional in effect. The Bcc of the same letters where the distance
/// fits one, and a plain `jmp` where it does not — never the opposite branch
/// over a jump, there being nothing to skip. `isMnemonic` is true of them,
/// and `opcodeOf` answers one in `RELATIVE` with its Bcc's opcode and in
/// `ABSOLUTE` with `jmp`'s — see
/// docs/decisions/0181-a-jump-whose-flag-is-known.md.
[[nodiscard]] bool isCertainJump( std::string_view mnemonic );

/// What a branch out of reach becomes: `BRANCH_OVER_JUMP` for a Jcc, and
/// `ABSOLUTE` for a certain jump, which needs no branch at all.
[[nodiscard]] AddressingMode lengthenedModeOf( std::string_view mnemonic );

[[nodiscard]] std::optional<std::uint8_t> opcodeOf( std::string_view mnemonic, AddressingMode mode );

/// Whether an instruction in this mode is the 65SC02's and not the 6502's.
/// The table holds both processors, so that a program writing `stz` on a
/// 6502 is told which processor it wants rather than that no such mnemonic
/// exists — see docs/decisions/0182-the-target-names-its-processor.md.
[[nodiscard]] bool needs65sc02( std::string_view mnemonic, AddressingMode mode );

/// The mode an instruction is encoded in, from the shape that was written and
/// the PlacementClass of what the operand names.
///
/// The PlacementClass is **declared**, so this never reads back an address. A
/// zero-page operand widens to absolute where the instruction has no zero-page
/// form, because widening is always correct and narrowing never is.
[[nodiscard]] std::optional<AddressingMode>
modeFor( std::string_view mnemonic, syntax::OperandShape shape, PlacementClass placement );

/// What an instruction does to the memory its operand names, when it names
/// memory at all: a load reads it, a store writes it, a shift on memory does
/// both, `jsr` calls it and comes back, a jump or a branch goes and does not.
/// NONE for an instruction with no operand. What this is for is the kind of
/// Reference a Chunk holds — see `References.hpp`.
enum class MemoryAccess : std::uint8_t
{
  NONE,
  READ,
  WRITE,
  READ_WRITE,
  CALL,
  JUMP,
};

[[nodiscard]] MemoryAccess accessOf( std::string_view mnemonic );

/// What an instruction does to the flow of control after it: most go on to
/// the next, a branch may go to its operand or on, a jump goes and does not
/// come back, `jsr` goes and comes back to the next, and a return leaves the
/// Section with nothing after it. `brk` goes on, since the handler returns
/// past its operand byte. What this is for is the Successors of a Chunk —
/// see `Trace.hpp` and the glossary.
enum class ControlFlow : std::uint8_t
{
  NEXT,
  BRANCH,
  JUMP,
  CALL,
  RETURN,
};

[[nodiscard]] ControlFlow flowOf( std::string_view mnemonic );

} // namespace nga::model
