#include "nga/model/References.hpp"

#include "nga/model/Isa.hpp"

#include <algorithm>
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
    std::string_view const attribute = sources.textOf( node.token.span() );
    if ( attribute == "runtimeSectionSize" || attribute == "resident" )
    {
      return;
    }
    if ( node.left != nullptr )
    {
      walk( symbols, sources, from, *node.left, visitor, walking );
    }
    return;
  }
  case syntax::ExpressionKind::UNARY:
  case syntax::ExpressionKind::BINARY:
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
