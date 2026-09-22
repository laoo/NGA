#include "nga/c/Inline.hpp"

#include "nga/c/Loops.hpp"

#include "nga/diag/Diagnostic.hpp"
#include "nga/diag/DiagnosticSink.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace nga::c
{

namespace
{

/// A function of the program, and the unit it was written in: a `static` one
/// is its unit's alone, so a name is looked up in its own unit first.
struct Where
{
  std::size_t unit = 0;
  ir::Function* function = nullptr;
};

/// Why the splice has no meaning for this callee, or empty where it has one
/// — the table of docs/decisions/0172-a-call-with-one-site-is-wrapped.md, in
/// the order it has the rows. Not a judgement of what it would cost: a shape
/// refused here is one the substitution says nothing about. The words are
/// what a finding says after `inline`, so they name the shape and not the
/// pass.
std::string_view refusalOf( ir::Function const& callee )
{
  for ( ir::Block const& block : callee.blocks )
  {
    for ( ir::Instruction const& instruction : block.instructions )
    {
      if ( std::holds_alternative<ir::Switch>( instruction.operation ) )
      {
        // Its cases are Procs of their own, chained by `then`, that reach the
        // function's bytes — see
        // docs/decisions/0083-a-procs-bytes-are-shared-by-trace.md.
        return "holds a `switch` whose cases are Procs of their own";
      }
    }
    if ( block.terminator.kind == ir::TerminatorKind::FALL )
    {
      return "leaves by falling into the Proc chained after it";
    }
  }
  if ( callee.blocks.empty() )
  {
    return "has no body here";
  }
  if ( !callee.pane.empty() )
  {
    return "is in a Pane, and its body must run under `.with`";
  }
  if ( !callee.under.empty() )
  {
    return "runs under a Pane, and its body must run under `.with`";
  }
  if ( !callee.withs.empty() )
  {
    return "holds a `[[with]]` block, whose blocks the text writes apart";
  }
  if ( !callee.memberOf.empty() )
  {
    return "is a member of a function type, whose bytes it reads its arguments from";
  }
  if ( callee.isTrampoline )
  {
    return "is a function type's own Proc";
  }
  if ( !callee.implements.empty() )
  {
    return "fills a Slot, and is reached through its Cell";
  }
  if ( !callee.then.empty() )
  {
    return "is chained to the Proc after it by `then`";
  }
  if ( !callee.sections.empty() )
  {
    return "holds a `static` local or a local array, which is one object however many callers there are";
  }
  return {};
}

/// Why the call may not be wrapped where it stands, or empty: a call wrapped
/// in `.with`, and one inside a `[[with]]` block of the caller, whose blocks
/// the text writes apart as a macro and which splitting a block would take
/// instructions out of.
std::string_view refusalAt( ir::Function const& caller, ir::Call const& call, std::uint32_t block )
{
  if ( !call.with.empty() )
  {
    return "is called here under a `.with`, which its body would have to run inside";
  }
  bool const within = std::ranges::any_of(
      caller.withs, [block]( ir::WithRegion const& region ) { return block >= region.begin && block < region.end; } );
  return within ? std::string_view{ "is called here inside a `[[with]]` block, whose blocks the text writes apart" }
                : std::string_view{};
}

/// The names the caller has already, which a name of the callee's may not be
/// once it is a byte of the caller's Proc.
std::set<std::string> namesOf( ir::Function const& function )
{
  std::set<std::string> taken{ std::string{ ir::RESULT } };
  for ( ir::Local const& local : function.parameters )
  {
    taken.insert( local.name );
  }
  for ( ir::Local const& local : function.locals )
  {
    taken.insert( local.name );
  }
  for ( ir::NamedConstant const& constant : function.constants )
  {
    taken.insert( constant.name );
  }
  for ( ir::Global const& section : function.sections )
  {
    taken.insert( section.name );
  }
  return taken;
}

/// What a name of the callee's becomes in the caller: `CALLEE__NAME`, which
/// says in the text where the byte came from, and a number after it where the
/// caller has that name already.
std::string freshName( std::set<std::string>& taken, std::string_view callee, std::string_view name )
{
  std::string const stem = std::string{ callee } + "__" + std::string{ name };
  std::string candidate = stem;
  for ( std::uint32_t counter = 1; !taken.insert( candidate ).second; ++counter )
  {
    candidate = stem + std::to_string( counter );
  }
  return candidate;
}

void raiseValue( ir::Operand& operand, std::uint32_t by )
{
  if ( auto* const value = std::get_if<ir::Value>( &operand ) )
  {
    value->index += by;
  }
}

/// The values the callee's instructions define and read, moved past the
/// caller's own, so that the two numberings become one.
void raiseValues( ir::Instruction& instruction, std::uint32_t by )
{
  std::visit(
      [by]( auto& operation )
      {
        using Operation = std::decay_t<decltype( operation )>;
        if constexpr ( std::is_same_v<Operation, ir::Unary> || std::is_same_v<Operation, ir::Binary> ||
                       std::is_same_v<Operation, ir::Compare> || std::is_same_v<Operation, ir::Convert> ||
                       std::is_same_v<Operation, ir::Load> || std::is_same_v<Operation, ir::LoadIndirect> )
        {
          operation.result.index += by;
        }
        else if constexpr ( std::is_same_v<Operation, ir::Call> )
        {
          if ( operation.result.has_value() )
          {
            operation.result->index += by;
          }
        }
      },
      instruction.operation );
  ir::eachOperand( instruction, [by]( ir::Operand& operand ) { raiseValue( operand, by ); } );
}

/// Every name of the callee's own — its parameters, its locals, its `const`
/// locals and `__ret` — made the name it has in the caller. A name it does
/// not hold is another Proc's or an object of the Module's, and is left as it
/// is.
using Renaming = std::map<std::string, std::string, std::less<>>;

void renameIn( ir::Instruction& instruction, Renaming const& names )
{
  ir::eachName( instruction,
                [&names]( std::string& name )
                {
                  std::string const base = baseOf( name );
                  if ( auto const found = names.find( base ); found != names.end() )
                  {
                    name.replace( 0, base.size(), found->second );
                  }
                } );
  ir::eachOperand( instruction,
                   [&names]( ir::Operand& operand )
                   {
                     auto* const constant = std::get_if<ir::Constant>( &operand );
                     if ( constant == nullptr || constant->name.empty() )
                     {
                       return;
                     }
                     if ( auto const found = names.find( constant->name ); found != names.end() )
                     {
                       constant->name = found->second;
                     }
                   } );
}

/// Every operand of the function, the one a BRANCH ends its block with
/// included — which `eachOperand` does not reach, since it is the block's and
/// not an instruction's.
template <typename Visit>
void eachOperandOf( ir::Function& function, Visit const& visit )
{
  for ( ir::Block& block : function.blocks )
  {
    for ( ir::Instruction& instruction : block.instructions )
    {
      ir::eachOperand( instruction, visit );
    }
    visit( block.terminator.condition );
  }
}

/// What the call left behind, read by the caller: the value it was defined as,
/// and — where it returns a `struct`, whose members the caller reaches one at
/// a time — the name `CALLEE.__ret`. Both become the byte the wrapped body
/// writes its result to.
void readResultFrom(
    ir::Function& caller, ir::Call const& call, std::string const& returned, ir::Type type, std::string const& callee )
{
  if ( call.result.has_value() )
  {
    std::uint32_t const index = call.result->index;
    eachOperandOf( caller,
                   [index, &returned, type]( ir::Operand& operand )
                   {
                     auto const* const value = std::get_if<ir::Value>( &operand );
                     if ( value != nullptr && value->index == index )
                     {
                       operand = ir::Object{ .name = returned, .type = type };
                     }
                   } );
  }

  std::string const was = callee + "." + std::string{ ir::RESULT };
  for ( ir::Block& block : caller.blocks )
  {
    for ( ir::Instruction& instruction : block.instructions )
    {
      ir::eachName( instruction,
                    [&was, &returned]( std::string& name )
                    {
                      if ( baseOf( name ) == was )
                      {
                        name.replace( 0, was.size(), returned );
                      }
                    } );
    }
  }
}

/// The callee's body where the call stood: its blocks in the caller's own
/// order, so that a loop the call is in stays the run of blocks the passes
/// after this one take a loop to be, and the text falls through where the
/// call fell through.
void splice( ir::Function& caller, ir::Function const& callee, std::uint32_t at, std::size_t index, bool sameFile )
{
  ir::Block& block = caller.blocks[at];
  auto const call = std::get<ir::Call>( block.instructions[index].operation );
  diag::SourceLocation const site = block.instructions[index].at;

  std::set<std::string> taken = namesOf( caller );
  Renaming names;
  for ( ir::Local const& parameter : callee.parameters )
  {
    names.emplace( parameter.name, freshName( taken, callee.name, parameter.name ) );
  }
  for ( ir::Local const& local : callee.locals )
  {
    names.emplace( local.name, freshName( taken, callee.name, local.name ) );
  }
  for ( ir::NamedConstant const& constant : callee.constants )
  {
    names.emplace( constant.name, freshName( taken, callee.name, constant.name ) );
  }
  std::string const returned = callee.result.has_value() ? freshName( taken, callee.name, "ret" ) : std::string{};
  if ( callee.result.has_value() )
  {
    names.emplace( std::string{ ir::RESULT }, returned );
  }

  // The bytes the body needs are the caller's now. A byte the body turns out
  // never to read is `dropDeadStores`'s to take out, not this pass's.
  for ( ir::Local const& parameter : callee.parameters )
  {
    caller.locals.push_back( ir::Local{
        .name = names.at( parameter.name ), .type = parameter.type, .bytes = parameter.bytes, .place = {} } );
  }
  for ( ir::Local const& local : callee.locals )
  {
    caller.locals.push_back(
        ir::Local{ .name = names.at( local.name ), .type = local.type, .bytes = local.bytes, .place = {} } );
  }
  for ( ir::NamedConstant const& constant : callee.constants )
  {
    caller.constants.push_back( ir::NamedConstant{ .name = names.at( constant.name ),
                                                   .value = constant.value,
                                                   .isStatic = constant.isStatic,
                                                   .at = constant.at } );
  }
  if ( callee.result.has_value() )
  {
    caller.locals.push_back(
        ir::Local{ .name = returned, .type = *callee.result, .bytes = callee.resultBytes, .place = {} } );
  }

  auto const base = static_cast<std::uint32_t>( caller.values.size() );
  caller.values.insert( caller.values.end(), callee.values.begin(), callee.values.end() );

  // Where the body lands: its first block just after the call's, and the rest
  // of the call's block after all of them.
  std::uint32_t const entry = at + 1;
  auto const count = static_cast<std::uint32_t>( callee.blocks.size() );
  std::uint32_t const after = entry + count;

  std::vector<ir::Block> body = callee.blocks;
  for ( ir::Block& one : body )
  {
    for ( ir::Instruction& instruction : one.instructions )
    {
      raiseValues( instruction, base );
      renameIn( instruction, names );
      if ( !sameFile )
      {
        instruction.at = site;
      }
    }
    raiseValue( one.terminator.condition, base );
    if ( !sameFile )
    {
      one.terminator.at = site;
    }
    switch ( one.terminator.kind )
    {
    case ir::TerminatorKind::RETURN:
      one.terminator.kind = ir::TerminatorKind::JUMP;
      one.terminator.target = after;
      one.terminator.otherwise = 0;
      break;
    case ir::TerminatorKind::DISPATCH:
      for ( std::uint32_t& into : one.terminator.targets )
      {
        into += entry;
      }
      break;
    case ir::TerminatorKind::BRANCH:
      one.terminator.otherwise += entry;
      [[fallthrough]];
    case ir::TerminatorKind::JUMP:
      one.terminator.target += entry;
      break;
    case ir::TerminatorKind::TRANSITION:
    case ir::TerminatorKind::FALL:
      break;
    }
  }

  // The call's block is cut in two, and everything the caller says about a
  // block after it says about one that many further on.
  auto const cut = block.instructions.begin() + static_cast<std::ptrdiff_t>( index );
  std::vector<ir::Instruction> tail{ std::make_move_iterator( cut + 1 ),
                                     std::make_move_iterator( block.instructions.end() ) };
  block.instructions.erase( cut, block.instructions.end() );
  ir::Terminator end = block.terminator;

  auto const raiseBlock = [entry, count]( std::uint32_t& target )
  {
    if ( target >= entry )
    {
      target += count + 1;
    }
  };
  for ( ir::Block& one : caller.blocks )
  {
    raiseBlock( one.terminator.target );
    raiseBlock( one.terminator.otherwise );
  }
  raiseBlock( end.target );
  raiseBlock( end.otherwise );
  for ( ir::WithRegion& region : caller.withs )
  {
    raiseBlock( region.begin );
    raiseBlock( region.end );
  }

  // Each argument written to its parameter's byte, as the call wrote it to
  // the callee's: a copy where it is a `struct`, and a store otherwise.
  for ( std::size_t which = 0; which < call.arguments.size(); ++which )
  {
    ir::Argument const& argument = call.arguments[which];
    std::string const& name = names.at( callee.parameters[which].name );
    if ( argument.from.has_value() )
    {
      block.instructions.push_back(
          ir::Instruction{ .operation = ir::Copy{ .from = *argument.from,
                                                  .to = ir::Place{ .name = name, .pointer = std::nullopt, .offset = 0 },
                                                  .bytes = argument.bytes },
                           .at = site } );
      continue;
    }
    block.instructions.push_back( ir::Instruction{
        .operation = ir::Store{ .name = name, .type = argument.type, .value = argument.value }, .at = site } );
  }
  block.terminator = ir::Terminator{
    .kind = ir::TerminatorKind::JUMP, .at = site, .target = entry, .otherwise = 0, .condition = {}, .phase = {}
  };

  body.push_back( ir::Block{ .instructions = std::move( tail ), .terminator = std::move( end ) } );
  caller.blocks.insert( caller.blocks.begin() + static_cast<std::ptrdiff_t>( entry ),
                        std::make_move_iterator( body.begin() ),
                        std::make_move_iterator( body.end() ) );

  if ( callee.result.has_value() )
  {
    readResultFrom( caller, call, returned, *callee.result, callee.name );
  }
}

} // namespace

void wrapCalls( std::span<std::optional<ir::Unit>> units,
                std::span<diag::DiagnosticSink* const> sinks,
                bool forConstants )
{
  std::map<std::string, Where> exported;
  std::vector<std::map<std::string, ir::Function*>> own( units.size() );
  for ( std::size_t index = 0; index < units.size(); ++index )
  {
    if ( !units[index].has_value() )
    {
      continue;
    }
    for ( ir::Definition& definition : units[index]->definitions )
    {
      if ( auto* const function = std::get_if<ir::Function>( &definition ) )
      {
        own[index][function->name] = function;
        if ( !function->isStatic )
        {
          exported.emplace( function->name, Where{ .unit = index, .function = function } );
        }
      }
    }
  }
  auto const resolve = [&own, &exported]( std::size_t unit, std::string const& name ) -> std::optional<Where>
  {
    if ( auto const found = own[unit].find( name ); found != own[unit].end() )
    {
      return Where{ .unit = unit, .function = found->second };
    }
    if ( auto const found = exported.find( name ); found != exported.end() )
    {
      return found->second;
    }
    return std::nullopt;
  };

  // Counted once, before anything is wrapped, and never counted again: a body
  // spliced into its caller takes its calls along, so what had two sites has
  // two of them still, one of which now stands in the caller. There is no
  // fixed point to iterate to.
  std::map<ir::Function const*, std::uint32_t> sites;
  for ( std::size_t index = 0; index < units.size(); ++index )
  {
    if ( !units[index].has_value() )
    {
      continue;
    }
    for ( ir::Definition& definition : units[index]->definitions )
    {
      auto const* const function = std::get_if<ir::Function>( &definition );
      if ( function == nullptr )
      {
        continue;
      }
      for ( ir::Block const& block : function->blocks )
      {
        for ( ir::Instruction const& instruction : block.instructions )
        {
          auto const* const call = std::get_if<ir::Call>( &instruction.operation );
          if ( call == nullptr )
          {
            continue;
          }
          if ( std::optional<Where> const callee = resolve( index, call->name ); callee.has_value() )
          {
            ++sites[callee->function];
          }
        }
      }
    }
  }

  // Callees first, so that a callee is wrapped into its caller with whatever
  // was wrapped into it already — the walk terminates because the call graph
  // has no cycle, recursion being refused.
  std::vector<Where> order;
  std::set<ir::Function const*> seen;
  std::function<void( Where )> visit = [&]( Where where )
  {
    if ( !seen.insert( where.function ).second )
    {
      return;
    }
    for ( ir::Block const& block : where.function->blocks )
    {
      for ( ir::Instruction const& instruction : block.instructions )
      {
        if ( auto const* const call = std::get_if<ir::Call>( &instruction.operation ) )
        {
          if ( std::optional<Where> const callee = resolve( where.unit, call->name ); callee.has_value() )
          {
            visit( *callee );
          }
        }
      }
    }
    order.push_back( where );
  };
  for ( std::size_t index = 0; index < units.size(); ++index )
  {
    for ( auto const& [name, function] : own[index] )
    {
      visit( Where{ .unit = index, .function = function } );
    }
  }

  // A refusal is silent where nothing was written and a finding where
  // `inline` was: the programmer named a function the compiler will not wrap,
  // and a promise quietly not kept is worse than one refused. The one about
  // the callee is said at the word, once however many calls meet it; the one
  // about a call is said at the call.
  std::set<ir::Function const*> told;
  auto const refuse =
      [&]( Where const& callee, std::size_t unit, diag::SourceSpan span, std::string_view what, bool atTheCall )
  {
    if ( !callee.function->isInline || ( !atTheCall && !told.insert( callee.function ).second ) )
    {
      return;
    }
    diag::DiagnosticSink* const sink = unit < sinks.size() ? sinks[unit] : nullptr;
    if ( sink != nullptr )
    {
      sink->add( diagnostic( diag::DiagnosticId::C_INLINE_REFUSED )
                     .at( span.begin, span.length )
                     .arg( "name", callee.function->name )
                     .arg( "what", std::string{ what } ) );
    }
  };

  for ( Where const& where : order )
  {
    ir::Function& caller = *where.function;
    for ( std::uint32_t at = 0; at < caller.blocks.size(); ++at )
    {
      for ( std::size_t index = 0; index < caller.blocks[at].instructions.size(); ++index )
      {
        ir::Instruction const& instruction = caller.blocks[at].instructions[index];
        auto const* const call = std::get_if<ir::Call>( &instruction.operation );
        if ( call == nullptr )
        {
          continue;
        }
        std::optional<Where> const callee = resolve( where.unit, call->name );
        if ( !callee.has_value() || callee->function == &caller )
        {
          continue;
        }

        // What asks for it: a call site that is its callee's only one, which
        // needs no justification of any other kind; `inline` written on the
        // callee, which is the programmer's to ask for; or, under the Intent
        // `speed`, a constant handed to the callee, which is what makes a
        // copy of the body worth more than the call — see
        // docs/decisions/0177-intent.md.
        bool const handsAConstant =
            forConstants && std::ranges::any_of( call->arguments,
                                                 []( ir::Argument const& argument )
                                                 { return std::holds_alternative<ir::Constant>( argument.value ); } );
        if ( sites[callee->function] != 1 && !callee->function->isInline && !handsAConstant )
        {
          continue;
        }

        // Every way out from here but the splice is a refusal, and a refusal
        // is never silent where `inline` was written — the arity among them,
        // which a member of a function type meets because its arguments are
        // the type's bytes and it declares none of its own.
        if ( std::string_view const why = refusalAt( caller, *call, at ); !why.empty() )
        {
          refuse( *callee, where.unit, diag::SourceSpan{ .begin = instruction.at, .length = 0 }, why, true );
          continue;
        }
        if ( std::string_view const why = refusalOf( *callee->function ); !why.empty() )
        {
          refuse( *callee, callee->unit, callee->function->inlineSpan, why, false );
          continue;
        }
        if ( call->arguments.size() != callee->function->parameters.size() )
        {
          refuse( *callee,
                  callee->unit,
                  callee->function->inlineSpan,
                  "takes its arguments in bytes that are not its own",
                  false );
          continue;
        }
        splice( caller, *callee->function, at, index, callee->unit == where.unit );

        // The block was cut at the call, so what is left of it holds no
        // instruction this loop has not seen.
        break;
      }
    }
  }
}

} // namespace nga::c
