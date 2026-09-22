#pragma once

#include "nga/diag/SourceManager.hpp"
#include "nga/model/Merge.hpp"
#include "nga/model/Types.hpp"
#include "nga/syntax/Expression.hpp"

#include <cstdint>
#include <optional>

namespace nga::model
{

/// What a Step can answer about the program while an expression is evaluated.
///
/// Each Step supplies what it has produced and nothing else, which is what
/// stops an expression from quietly reaching a result that does not exist yet:
/// before Size there are no sizes to give, and before Place no addresses. The
/// base answers nothing at all.
class ValueSource
{
public:
  ValueSource() = default;
  ValueSource( ValueSource const& ) = delete;
  ValueSource( ValueSource&& ) = delete;
  ValueSource& operator=( ValueSource const& ) = delete;
  ValueSource& operator=( ValueSource&& ) = delete;
  virtual ~ValueSource() = default;

  [[nodiscard]] virtual std::optional<std::int64_t> sizeOfSection( SectionRef /*where*/ )
  {
    return std::nullopt;
  }

  /// How far a Chunk stands from the start of its Section — or, given
  /// `inner`, how far the inner Chunk at that index of a macro use's
  /// expansion does, which is the position a local label of the body names.
  [[nodiscard]] virtual std::optional<std::int64_t>
  offsetOf( SectionRef /*where*/, ChunkIndex /*chunk*/, std::optional<std::uint32_t> /*inner*/ )
  {
    return std::nullopt;
  }

  [[nodiscard]] virtual std::optional<std::int64_t> addressOf( SectionRef /*where*/ )
  {
    return std::nullopt;
  }

  /// The index of the state the solver gave a Pane, which needs Place to
  /// have run — see docs/decisions/0054-panes.md.
  [[nodiscard]] virtual std::optional<std::int64_t> stateOfPane( PaneIndex /*pane*/ )
  {
    return std::nullopt;
  }
};

/// A source that knows nothing, which is what a declared value is measured
/// against.
ValueSource& nothingIsKnown();

/// The number an expression evaluates to, or nothing when something it needs
/// has not been produced yet.
///
/// The tables are a pointer because there is one caller for which they do not
/// exist yet: resolving a character set is what produces them, so a declaration
/// cannot be written in terms of one. Everywhere else they are present.
///
/// It reports nothing. Every rule that can be broken here has already been
/// reported by the type check; what is left is whether the answer exists, and
/// the Step that wanted it is the one that can say why it mattered.
std::optional<std::int64_t> evaluate( diag::SourceManager const& sources,
                                      GlobalSymbols const& symbols,
                                      Charsets const* charsets,
                                      ModuleIndex home,
                                      syntax::Expression const& node,
                                      ValueSource& known );

/// Whether an expression has a **declared value**, and what it is.
///
/// This is exactly evaluation against a source that supplies nothing: a
/// literal and a Constant over literals need no Step to have run, and anything
/// naming a position or a size does. Writing the rule this way is what makes
/// it a property of the shape of the definitions rather than of the moment it
/// is asked — see docs/spec/syntax.md.
std::optional<std::int64_t> declaredValueOf( diag::SourceManager const& sources,
                                             GlobalSymbols const& symbols,
                                             Charsets const* charsets,
                                             ModuleIndex home,
                                             syntax::Expression const& node );

} // namespace nga::model
