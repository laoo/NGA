#include "nga/model/References.hpp"

#include "nga/model/Isa.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace nga::model
{

namespace
{

void walk( GlobalSymbols const& symbols,
           diag::SourceManager const& sources,
           ModuleIndex from,
           syntax::Expression const& node,
           ReferenceVisitor& visitor,
           std::vector<SymbolRef>& walking );

/// What naming one Symbol reaches.
void visitSymbol( GlobalSymbols const& symbols,
                  diag::SourceManager const& sources,
                  SymbolRef where,
                  ReferenceVisitor& visitor,
                  std::vector<SymbolRef>& walking )
{
  Symbol const& symbol = symbols.at( where );
  switch ( symbol.kind )
  {
  case SymbolKind::LABEL:
  case SymbolKind::SLOT:
    visitor.reference( where );
    return;
  case SymbolKind::CONSTANT:
  {
    auto const& value = std::get<syntax::ExpressionPtr>( symbol.value );
    if ( value == nullptr || std::ranges::find( walking, where ) != walking.end() )
    {
      return;
    }
    walking.push_back( where );
    walk( symbols, sources, where.module, *value, visitor, walking );
    walking.pop_back();
    return;
  }
  case SymbolKind::CHARSET:
  case SymbolKind::REGION:
  case SymbolKind::WINDOW:
  case SymbolKind::PANE:
  case SymbolKind::MACRO:
    // A Region is addresses and never a Section: nothing is reached, kept
    // or held still by naming one. A Window and a macro are not values at
    // all.
    return;
  }
}

/// The Section an expression is a position in, where it plainly is one: a
/// Label, a local label, a Constant defined as one, `.runtimeSectionAddress`
/// of one, or any of those plus or minus a number. Nothing for anything else,
/// which is an answer about the shape and not about the type: an expression
/// this does not recognise is walked as it always was.
std::optional<SectionRef> positionIn( GlobalSymbols const& symbols,
                                      diag::SourceManager const& sources,
                                      ModuleIndex from,
                                      syntax::Expression const& node,
                                      std::vector<SymbolRef>& walking );

std::optional<SectionRef> positionOfSymbol( GlobalSymbols const& symbols,
                                            diag::SourceManager const& sources,
                                            SymbolRef where,
                                            std::vector<SymbolRef>& walking )
{
  Symbol const& symbol = symbols.at( where );
  if ( symbol.kind == SymbolKind::LABEL )
  {
    return sectionNamedBy( symbols, where );
  }
  if ( symbol.kind != SymbolKind::CONSTANT )
  {
    return std::nullopt;
  }
  auto const& value = std::get<syntax::ExpressionPtr>( symbol.value );
  if ( value == nullptr || std::ranges::find( walking, where ) != walking.end() )
  {
    return std::nullopt;
  }
  walking.push_back( where );
  std::optional<SectionRef> const section = positionIn( symbols, sources, where.module, *value, walking );
  walking.pop_back();
  return section;
}

std::optional<SectionRef> positionIn( GlobalSymbols const& symbols,
                                      diag::SourceManager const& sources,
                                      ModuleIndex from,
                                      syntax::Expression const& node,
                                      std::vector<SymbolRef>& walking )
{
  switch ( node.kind )
  {
  case syntax::ExpressionKind::NAME:
  {
    std::optional<SymbolRef> const where = symbols.resolveText( from, node, sources.textOf( node.token.span() ) );
    return where.has_value() ? positionOfSymbol( symbols, sources, *where, walking ) : std::nullopt;
  }
  case syntax::ExpressionKind::LOCAL_NAME:
  {
    std::optional<LabelPosition> const target = symbols.moduleAt( from ).localTarget( &node );
    return target.has_value() ? std::optional{ SectionRef{ .module = from, .section = target->section } }
                              : std::nullopt;
  }
  case syntax::ExpressionKind::ATTRIBUTE:
  {
    if ( std::optional<std::string> const dotted = syntax::dottedNameOf( sources, node ); dotted.has_value() )
    {
      if ( std::optional<SymbolRef> const where = symbols.resolveText( from, syntax::leftmostOf( node ), *dotted );
           where.has_value() )
      {
        return positionOfSymbol( symbols, sources, *where, walking );
      }
    }
    if ( node.left == nullptr ||
         sectionAttributeNamed( sources.textOf( node.token.span() ) ) != SectionAttribute::RUNTIME_SECTION_ADDRESS )
    {
      return std::nullopt;
    }
    return positionIn( symbols, sources, from, *node.left, walking );
  }
  case syntax::ExpressionKind::BINARY:
  {
    if ( node.left == nullptr || node.right == nullptr ||
         ( node.binaryOperator != syntax::BinaryOperator::ADD &&
           node.binaryOperator != syntax::BinaryOperator::SUBTRACT ) )
    {
      return std::nullopt;
    }
    std::optional<SectionRef> const left = positionIn( symbols, sources, from, *node.left, walking );
    std::optional<SectionRef> const right = positionIn( symbols, sources, from, *node.right, walking );
    if ( left.has_value() == right.has_value() )
    {
      return std::nullopt;
    }
    // A number may stand before a position only where it is added to one.
    if ( right.has_value() && node.binaryOperator == syntax::BinaryOperator::SUBTRACT )
    {
      return std::nullopt;
    }
    return left.has_value() ? left : right;
  }
  case syntax::ExpressionKind::ERROR:
  case syntax::ExpressionKind::UNARY:
  case syntax::ExpressionKind::NUMBER:
  case syntax::ExpressionKind::VALUE:
  case syntax::ExpressionKind::SPREAD:
  case syntax::ExpressionKind::CHARACTER:
  case syntax::ExpressionKind::STRING:
    return std::nullopt;
  }
  return std::nullopt;
}

/// The two sides of a difference of positions in one Section, told to the
/// visitor less that Section: the difference is a size, known after Size,
/// and reaches nothing, as `.runtimeSectionSize` does not. Whatever else the
/// sides name is reached as ever.
class WithoutSection final : public ReferenceVisitor
{
public:
  WithoutSection( GlobalSymbols const& symbols, ModuleIndex from, SectionRef section, ReferenceVisitor& inner )
      : mSymbols( &symbols ), mFrom( from ), mSection( section ), mInner( &inner )
  {
  }

  void reference( SymbolRef target ) override
  {
    if ( sectionNamedBy( *mSymbols, target ) != mSection )
    {
      mInner->reference( target );
    }
  }

  void localReference( LabelPosition target ) override
  {
    if ( SectionRef{ .module = mFrom, .section = target.section } != mSection )
    {
      mInner->localReference( target );
    }
  }

  void linearReference( SymbolRef target ) override
  {
    if ( sectionNamedBy( *mSymbols, target ) != mSection )
    {
      mInner->linearReference( target );
    }
  }

private:
  GlobalSymbols const* mSymbols;
  ModuleIndex mFrom;
  SectionRef mSection;
  ReferenceVisitor* mInner;
};

/// What a linear attribute's receiver names, told to the visitor as linear
/// References: the medium reads it, and the CPU does not.
class LinearVisitor final : public ReferenceVisitor
{
public:
  explicit LinearVisitor( ReferenceVisitor& inner ) : mInner( &inner ) {}

  void reference( SymbolRef target ) override
  {
    mInner->linearReference( target );
  }

  void localReference( LabelPosition target ) override
  {
    mInner->localReference( target );
  }

  void linearReference( SymbolRef target ) override
  {
    mInner->linearReference( target );
  }

private:
  ReferenceVisitor* mInner;
};

void walk( GlobalSymbols const& symbols,
           diag::SourceManager const& sources,
           ModuleIndex from,
           syntax::Expression const& node,
           ReferenceVisitor& visitor,
           std::vector<SymbolRef>& walking )
{
  switch ( node.kind )
  {
  case syntax::ExpressionKind::NAME:
  {
    std::optional<SymbolRef> const where = symbols.resolveText( from, node, sources.textOf( node.token.span() ) );
    if ( where.has_value() )
    {
      visitSymbol( symbols, sources, *where, visitor, walking );
    }
    return;
  }
  case syntax::ExpressionKind::LOCAL_NAME:
  {
    std::optional<LabelPosition> const target = symbols.moduleAt( from ).localTarget( &node );
    if ( target.has_value() )
    {
      visitor.localReference( *target );
    }
    return;
  }
  case syntax::ExpressionKind::ATTRIBUTE:
  {
    // A dotted name that is a Symbol — `one.gfx`, a member of a Namespace —
    // before an attribute of what stands left of the dot.
    if ( std::optional<std::string> const dotted = syntax::dottedNameOf( sources, node ); dotted.has_value() )
    {
      if ( std::optional<SymbolRef> const where = symbols.resolveText( from, syntax::leftmostOf( node ), *dotted );
           where.has_value() )
      {
        visitSymbol( symbols, sources, *where, visitor, walking );
        return;
      }
    }
    std::optional<SectionAttribute> const attribute = sectionAttributeNamed( sources.textOf( node.token.span() ) );
    if ( attribute == SectionAttribute::RUNTIME_SECTION_SIZE || attribute == SectionAttribute::RESIDENT )
    {
      return;
    }
    if ( node.left != nullptr && ( attribute == SectionAttribute::LINEAR_SECTION_ADDRESS ||
                                   attribute == SectionAttribute::LINEAR_FAMILY_ADDRESS ) )
    {
      LinearVisitor linear{ visitor };
      walk( symbols, sources, from, *node.left, linear, walking );
      return;
    }
    if ( node.left != nullptr )
    {
      walk( symbols, sources, from, *node.left, visitor, walking );
    }
    return;
  }
  case syntax::ExpressionKind::BINARY:
    if ( node.binaryOperator == syntax::BinaryOperator::SUBTRACT && node.left != nullptr && node.right != nullptr )
    {
      std::optional<SectionRef> const left = positionIn( symbols, sources, from, *node.left, walking );
      if ( left.has_value() && left == positionIn( symbols, sources, from, *node.right, walking ) )
      {
        WithoutSection without{ symbols, from, *left, visitor };
        walk( symbols, sources, from, *node.left, without, walking );
        walk( symbols, sources, from, *node.right, without, walking );
        return;
      }
    }
    [[fallthrough]];
  case syntax::ExpressionKind::UNARY:
    if ( node.left != nullptr )
    {
      walk( symbols, sources, from, *node.left, visitor, walking );
    }
    if ( node.right != nullptr )
    {
      walk( symbols, sources, from, *node.right, visitor, walking );
    }
    return;
  case syntax::ExpressionKind::ERROR:
  case syntax::ExpressionKind::NUMBER:
  case syntax::ExpressionKind::VALUE:
  case syntax::ExpressionKind::SPREAD:
  case syntax::ExpressionKind::CHARACTER:
  case syntax::ExpressionKind::STRING:
    return;
  }
}

} // namespace

std::string_view nameOf( ReferenceKind kind )
{
  switch ( kind )
  {
  case ReferenceKind::CALL:
    return "call";
  case ReferenceKind::JUMP:
    return "jump";
  case ReferenceKind::READ:
    return "read";
  case ReferenceKind::WRITE:
    return "write";
  case ReferenceKind::READ_WRITE:
    return "read-write";
  case ReferenceKind::ESCAPE:
    return "escape";
  }
  return "escape";
}

ReferenceKind referenceKindOf( std::string_view mnemonic, syntax::OperandShape shape )
{
  switch ( shape )
  {
  case syntax::OperandShape::NONE:
  case syntax::OperandShape::IMMEDIATE:
    return ReferenceKind::ESCAPE;
  case syntax::OperandShape::INDIRECT:
  case syntax::OperandShape::INDIRECT_Y:
  case syntax::OperandShape::INDEXED_INDIRECT:
    // The operand names the pointer, and the instruction reads it to find
    // where to go or what to touch; `jmp (ptr)` is a read of `ptr` here and
    // a jump to wherever an address escaped to.
    return ReferenceKind::READ;
  case syntax::OperandShape::DIRECT:
  case syntax::OperandShape::DIRECT_X:
  case syntax::OperandShape::DIRECT_Y:
    break;
  }
  switch ( accessOf( mnemonic ) )
  {
  case MemoryAccess::READ:
    return ReferenceKind::READ;
  case MemoryAccess::WRITE:
    return ReferenceKind::WRITE;
  case MemoryAccess::READ_WRITE:
    return ReferenceKind::READ_WRITE;
  case MemoryAccess::CALL:
    return ReferenceKind::CALL;
  case MemoryAccess::JUMP:
    return ReferenceKind::JUMP;
  case MemoryAccess::NONE:
    break;
  }
  return ReferenceKind::ESCAPE;
}

ReferenceKind referenceKindOf( diag::SourceManager const& sources, Chunk const& chunk )
{
  if ( auto const* const instruction = std::get_if<InstructionContent>( &chunk.content ); instruction != nullptr )
  {
    return referenceKindOf( sources.textOf( instruction->mnemonic.span() ), instruction->shape );
  }
  if ( std::holds_alternative<DataContent>( chunk.content ) || std::holds_alternative<ReserveContent>( chunk.content ) )
  {
    return ReferenceKind::ESCAPE;
  }
  if ( std::holds_alternative<MacroUseContent>( chunk.content ) )
  {
    // Never asked: what a use encodes is its inner Chunks, each of a kind of
    // its own. Escape is the kind that assumes the least.
    return ReferenceKind::ESCAPE;
  }
  // A `.transition` to the routine and the Phase it enters, a Cell to what
  // the edges write into it, a `.dispatch` to each position it names.
  return ReferenceKind::JUMP;
}

void walkReferences( GlobalSymbols const& symbols,
                     diag::SourceManager const& sources,
                     ModuleIndex from,
                     syntax::Expression const& node,
                     ReferenceVisitor& visitor )
{
  std::vector<SymbolRef> walking;
  walk( symbols, sources, from, node, visitor, walking );
}

} // namespace nga::model
