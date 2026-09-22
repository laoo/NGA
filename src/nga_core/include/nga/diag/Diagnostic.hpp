#pragma once

#include "nga/diag/DiagnosticId.hpp"
#include "nga/diag/SourceLocation.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <variant>
#include <vector>

namespace nga::diag
{

using ArgumentValue = std::variant<std::string, std::int64_t, bool>;

/// A named value substituted into a message template and published verbatim in
/// the JSON output, where it — not the prose — carries the meaning.
struct Argument
{
  std::string name;
  ArgumentValue value;
};

/// A finding. Never carries rendered text: the message is produced from the
/// catalog when, and only when, something is about to be displayed.
struct Diagnostic
{
  DiagnosticId id{};
  SourceSpan span{};
  std::vector<Argument> arguments;
  std::vector<Diagnostic> notes;

  /// Orders findings that have no source position. Ignored for located ones.
  std::string sortKey;

  Diagnostic at( SourceLocation location, std::uint32_t length = 0 ) &&;
  Diagnostic arg( std::string name, ArgumentValue value ) &&;
  Diagnostic note( Diagnostic child ) &&;
  Diagnostic sortedBy( std::string key ) &&;
};

/// Entry point for building a diagnostic:
///   diagnostic( DiagnosticId::CROSS_VIEW_REFERENCE ).at( loc, len ).arg( "symbol", name )
Diagnostic diagnostic( DiagnosticId id );

/// Substitutes {name}, {name:hex} and {name:n} in a message template. Braces are
/// escaped by doubling. A name with no matching argument renders visibly rather
/// than silently, so a catalog mistake cannot hide.
std::string renderMessage( std::string_view messageTemplate, std::span<Argument const> arguments );

/// The message of a diagnostic, from the catalog.
std::string renderMessage( Diagnostic const& value );

} // namespace nga::diag
