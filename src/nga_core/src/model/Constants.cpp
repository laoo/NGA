#include "nga/model/Constants.hpp"

#include "nga/model/Symbol.hpp"
#include "nga/syntax/Expression.hpp"

namespace nga::model
{

Module buildConstantModule( diag::SourceManager const& sources,
                            ProjectModule const& entry,
                            std::span<ProjectConstant const> constants )
{
  Module module{ entry.name, entry.file, entry.residency };
  for ( ProjectConstant const& constant : constants )
  {
    // The number as the document wrote it, unevaluated: a Constant's value is
    // an expression everywhere else in the model, and a Project's is no
    // different for having been written in a different grammar.
    syntax::Token const token{ .kind = syntax::TokenKind::NUMBER,
                               .location = constant.valueSpan.begin,
                               .length = constant.valueSpan.length };
    syntax::ExpressionPtr value = syntax::makeExpression( syntax::ExpressionKind::NUMBER, token, constant.valueSpan );
    // A sign is the negation the assembler reads in `-1`, so a negative value
    // is no new kind of value for anything that reads one.
    if ( constant.signSpan.has_value() )
    {
      syntax::Token const sign{ .kind = syntax::TokenKind::MINUS,
                                .location = constant.signSpan->begin,
                                .length = constant.signSpan->length };
      syntax::ExpressionPtr negated = syntax::makeExpression(
          syntax::ExpressionKind::UNARY, sign, syntax::spanning( *constant.signSpan, constant.valueSpan ) );
      negated->unaryOperator = syntax::UnaryOperator::NEGATE;
      negated->left = std::move( value );
      value = std::move( negated );
    }
    // A name given a value twice was refused where the second was read, so
    // nothing here can fail to be added.
    module.symbols().add( Symbol{ .name = sources.textOf( constant.nameSpan ),
                                  .kind = SymbolKind::CONSTANT,
                                  .definition = constant.nameSpan,
                                  .exported = true,
                                  .value = std::move( value ) } );
  }
  return module;
}

} // namespace nga::model
