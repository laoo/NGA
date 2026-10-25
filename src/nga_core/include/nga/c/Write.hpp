#pragma once

#include "nga/c/Compile.hpp"
#include "nga/c/Ir.hpp"
#include "nga/diag/SourceManager.hpp"

#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace nga::c::detail
{

/// What a function's text says of where its parameters and its result
/// could be carried: its body with the stores to a byte a register carries
/// taken out, and what `A` held at each `rts` — see
/// docs/decisions/0145-an-argument-in-a-register.md.
struct PlaceFacts
{
  std::string body;
  std::vector<std::vector<std::string>> returns;
};

/// The text of one lowered file — see docs/decisions/0062-a-subset-of-c.md.
[[nodiscard]] std::string writeUnit(
    diag::SourceManager const& sources, std::string_view sourcePath, Cpu cpu, Intent intent, ir::Unit const& lowered );

/// What the text of one function says of where its parameters and its result
/// could be carried, written as in the unit it belongs to.
[[nodiscard]] PlaceFacts placeFacts( diag::SourceManager const& sources,
                                     std::string_view sourcePath,
                                     Cpu cpu,
                                     Intent intent,
                                     ir::Function const& lowered,
                                     ir::Unit const& unit );

/// Whether an operand is the value `value`.
[[nodiscard]] bool isValue( ir::Operand const& operand, ir::Value value );

} // namespace nga::c::detail
