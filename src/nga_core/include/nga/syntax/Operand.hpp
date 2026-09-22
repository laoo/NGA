#pragma once

#include <cstdint>
#include <string_view>

namespace nga::syntax
{

/// The syntactic shape of an instruction's operand — what was written, not what
/// it will be encoded as.
///
/// DIRECT is zero page or absolute depending on the PlacementClass of what the
/// expression names, which may be unknown until Merge. Deciding that is the
/// encoder's business, and keeping it out of here is what lets the parser stay
/// free of the ISA: a directive begins with `.`, so everything else is an
/// instruction, and no table of mnemonics is needed to parse one.
enum class OperandShape : std::uint8_t
{
  NONE,             ///< `rts`
  IMMEDIATE,        ///< `lda #expr`
  DIRECT,           ///< `lda expr`
  DIRECT_X,         ///< `lda expr,x`
  DIRECT_Y,         ///< `lda expr,y`
  INDIRECT,         ///< `jmp (expr)`
  INDIRECT_Y,       ///< `lda (expr),y`
  INDEXED_INDIRECT, ///< `lda (expr,x)`
};

std::string_view nameOf( OperandShape shape );

enum class DataWidth : std::uint8_t
{
  BYTE,
  WORD,
};

std::string_view nameOf( DataWidth width );

} // namespace nga::syntax
