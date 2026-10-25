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

/// The bytes a copy of a body may cost past the call it replaces where the
/// Intent is `speed`: the frame bought for them is twelve cycles and what the
/// passes then see through. Not tuned to a program: measured over the whole
/// set, every wrap it takes is faster and the next steps take bytes for no
/// cycles — see docs/decisions/0299-a-body-no-larger-than-its-call-is-wrapped.md.
constexpr std::uint32_t SPEED_ALLOWANCE = 32;

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

/// What reading an operand costs in the text, a byte of it: an immediate, a
/// byte of the zero page a value is held in, or an object anywhere.
std::uint32_t readCost( ir::Operand const& operand )
{
  if ( std::holds_alternative<ir::Constant>( operand ) )
  {
    return 2;
  }
  return std::holds_alternative<ir::Value>( operand ) ? 2 : 3;
}

/// The bytes an instruction comes to in the text, estimated before there is
/// any: a load, an operation and a store for each byte of its type, at what
/// reading each operand costs. An estimate and not a count — no pass after
/// this one has run yet, and each takes bytes out the estimate keeps — but
/// it is the same estimate on both sides of the comparison it is made for.
std::uint32_t estimated( ir::Instruction const& instruction, ir::Function const& function )
{
  return std::visit(
      [&function]( auto const& operation ) -> std::uint32_t
      {
        using Operation = std::decay_t<decltype( operation )>;
        if constexpr ( std::is_same_v<Operation, ir::Unary> )
        {
          return ir::sizeOf( operation.type ) * ( readCost( operation.operand ) + 2 );
        }
        else if constexpr ( std::is_same_v<Operation, ir::Convert> )
        {
          // A conversion between two types of one width is a new name for
          // the same bytes.
          ir::Type const from = ir::typeOf( operation.operand, function );
          return ir::sizeOf( from ) == ir::sizeOf( operation.type )
                     ? 0
                     : ir::sizeOf( operation.type ) * ( readCost( operation.operand ) + 2 );
        }
        else if constexpr ( std::is_same_v<Operation, ir::Binary> || std::is_same_v<Operation, ir::Compare> )
        {
          return ir::sizeOf( operation.type ) * ( readCost( operation.left ) + readCost( operation.right ) + 2 );
        }
        else if constexpr ( std::is_same_v<Operation, ir::Load> )
        {
          return ( ir::sizeOf( operation.type ) * 5 ) +
                 ( std::holds_alternative<ir::Constant>( operation.index ) ? 0 : 2 );
        }
        else if constexpr ( std::is_same_v<Operation, ir::LoadIndirect> )
        {
          return 2 + ( ir::sizeOf( operation.type ) * 4 );
        }
        else if constexpr ( std::is_same_v<Operation, ir::Store> )
        {
          return ir::sizeOf( operation.type ) * ( readCost( operation.value ) + 3 );
        }
        else if constexpr ( std::is_same_v<Operation, ir::StoreElement> )
        {
          return ( ir::sizeOf( operation.type ) * ( readCost( operation.value ) + 3 ) ) +
                 ( std::holds_alternative<ir::Constant>( operation.index ) ? 0 : 2 );
        }
        else if constexpr ( std::is_same_v<Operation, ir::StoreIndirect> )
        {
          return 2 + ( ir::sizeOf( operation.type ) * ( readCost( operation.value ) + 2 ) );
        }
        else if constexpr ( std::is_same_v<Operation, ir::Copy> )
        {
          return operation.bytes * 6;
        }
        else if constexpr ( std::is_same_v<Operation, ir::Call> )
        {
          std::uint32_t bytes = 3;
          for ( ir::Argument const& argument : operation.arguments )
          {
            bytes += argument.from.has_value() ? argument.bytes * 6
                                               : ir::sizeOf( argument.type ) * ( readCost( argument.value ) + 3 );
          }
          return bytes +
                 ( operation.result.has_value() ? ir::sizeOf( function.values.at( operation.result->index ) ) * 3 : 0 );
        }
        else
        {
          // What is left is rare in a body and never short: a `switch`, a
          // `.with`.
          return 16;
        }
      },
      instruction.operation );
}

/// The bytes a copy of the body adds where it is spliced: every instruction
/// but the store of its result, which its caller's readers read in place of
/// the call's, and every way between its blocks but the returns, which become
/// the fall onto what followed the call.
std::uint32_t bodyCost( ir::Function const& callee )
{
  std::uint32_t bytes = 0;
  for ( ir::Block const& block : callee.blocks )
  {
    for ( ir::Instruction const& instruction : block.instructions )
    {
      auto const* const store = std::get_if<ir::Store>( &instruction.operation );
      if ( store == nullptr || store->name != ir::RESULT )
      {
        bytes += estimated( instruction, callee );
      }
    }
    switch ( block.terminator.kind )
    {
    case ir::TerminatorKind::BRANCH:
      bytes += 2 + readCost( block.terminator.condition );
      break;
    case ir::TerminatorKind::JUMP:
      bytes += 3;
      break;
    case ir::TerminatorKind::DISPATCH:
      bytes += 16;
      break;
    default:
      break;
    }
  }
  return bytes;
}

/// The bytes a call of the callee takes that a copy of its body does not: the
/// `jsr`, a store into each byte of its parameters, and a read of each byte
/// of its result.
std::uint32_t callCost( ir::Function const& callee )
{
  auto const width = []( ir::Type type, std::uint32_t bytes )
  { return type == ir::Type::BLOCK ? bytes : ir::sizeOf( type ); };
  std::uint32_t bytes = 3;
  for ( ir::Local const& parameter : callee.parameters )
  {
    bytes += width( parameter.type, parameter.bytes ) * 3;
  }
  return bytes + ( callee.result.has_value() ? width( *callee.result, callee.resultBytes ) * 3 : 0 );
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

void renameName( std::string& name, Renaming const& names )
{
  std::string const base = ir::baseOf( name );
  if ( auto const found = names.find( base ); found != names.end() )
  {
    name.replace( 0, base.size(), found->second );
  }
}

void renameConstant( ir::Operand& operand, Renaming const& names )
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
}

void renameIn( ir::Instruction& instruction, Renaming const& names )
{
  ir::eachName( instruction, [&names]( std::string& name ) { renameName( name, names ); } );
  ir::eachOperand( instruction, [&names]( ir::Operand& operand ) { renameConstant( operand, names ); } );
}

/// The same of what ends a block: a BRANCH's condition.
void renameIn( ir::Terminator& terminator, Renaming const& names )
{
  ir::eachName( terminator.condition, [&names]( std::string& name ) { renameName( name, names ); } );
  renameConstant( terminator.condition, names );
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
    for ( ir::Block& block : caller.blocks )
    {
      ir::eachOperand( block,
                       [index, &returned, type]( ir::Operand& operand )
                       {
                         auto const* const value = std::get_if<ir::Value>( &operand );
                         if ( value != nullptr && value->index == index )
                         {
                           operand = ir::Object{ .name = returned, .type = type };
                         }
                       } );
    }
  }

  std::string const was = callee + "." + std::string{ ir::RESULT };
  for ( ir::Block& block : caller.blocks )
  {
    for ( ir::Instruction& instruction : block.instructions )
    {
      ir::eachName( instruction,
                    [&was, &returned]( std::string& name )
                    {
                      if ( ir::baseOf( name ) == was )
                      {
                        name.replace( 0, was.size(), returned );
                      }
                    } );
    }
  }
}

/// What each name of the callee's own is called in the caller, and the byte
/// its `__ret` becomes there, empty for a callee that returns nothing.
struct Names
{
  Renaming names;
  std::string returned;
};

Names namesFor( ir::Function const& caller, ir::Function const& callee )
{
  std::set<std::string> taken = namesOf( caller );
  Names out;
  for ( ir::Local const& parameter : callee.parameters )
  {
    out.names.emplace( parameter.name, freshName( taken, callee.name, parameter.name ) );
  }
  for ( ir::Local const& local : callee.locals )
  {
    out.names.emplace( local.name, freshName( taken, callee.name, local.name ) );
  }
  for ( ir::NamedConstant const& constant : callee.constants )
  {
    out.names.emplace( constant.name, freshName( taken, callee.name, constant.name ) );
  }
  if ( callee.result.has_value() )
  {
    out.returned = freshName( taken, callee.name, "ret" );
    out.names.emplace( std::string{ ir::RESULT }, out.returned );
  }
  return out;
}

/// The callee's locals and `const` locals made the caller's. Its parameters
/// and its `__ret` are the caller's to add, since a splice in place may need
/// neither.
void adoptLocals( ir::Function& caller, ir::Function const& callee, Renaming const& names )
{
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
}

/// The argument of a call written to its parameter's byte, as the call wrote
/// it to the callee's: a copy where it is a `struct`, and a store otherwise.
ir::Instruction argumentStore( ir::Argument const& argument, std::string const& name, diag::SourceLocation site )
{
  if ( argument.from.has_value() )
  {
    return ir::Instruction{ .operation =
                                ir::Copy{ .from = *argument.from,
                                          .to = ir::Place{ .name = name, .pointer = std::nullopt, .offset = 0 },
                                          .bytes = argument.bytes },
                            .at = site };
  }
  return ir::Instruction{ .operation = ir::Store{ .name = name, .type = argument.type, .value = argument.value },
                          .at = site };
}

/// Whether every mention the body makes of the byte `name` is an operand read
/// of the whole of it in `type`: no store to it, no element of it, no address
/// of it and no narrower read — the shape in which a value of `type` can be
/// read in its place.
bool onlyRead( ir::Block& body, std::string const& name, ir::Type type )
{
  std::size_t mentions = 0;
  std::size_t reads = 0;
  bool other = false;
  for ( ir::Instruction& instruction : body.instructions )
  {
    ir::eachName( instruction,
                  [&name, &mentions]( std::string const& one )
                  {
                    if ( ir::baseOf( one ) == name )
                    {
                      ++mentions;
                    }
                  } );
    if ( auto const* const copy = std::get_if<ir::Copy>( &instruction.operation ) )
    {
      // A pointer a copy goes by is a byte of the zero page the text names,
      // which a value is not.
      for ( auto const* pointer : { &copy->from.pointer, &copy->to.pointer } )
      {
        auto const* const object = pointer->has_value() ? std::get_if<ir::Object>( &**pointer ) : nullptr;
        other = other || ( object != nullptr && ir::baseOf( object->name ) == name );
      }
    }
    ir::eachOperand( instruction,
                     [&name, type, &reads, &other]( ir::Operand const& operand )
                     {
                       if ( auto const* const object = std::get_if<ir::Object>( &operand ) )
                       {
                         if ( object->name == name && object->type == type )
                         {
                           ++reads;
                         }
                       }
                       else if ( auto const* const constant = std::get_if<ir::Constant>( &operand ) )
                       {
                         other = other || constant->name == name;
                       }
                     } );
  }
  return !other && mentions == reads;
}

/// Where the call is read as a value, the operand the body's one store to
/// `__ret` writes, if the call's readers can read that instead: a value or a
/// constant of the result's own type, stored once, and `__ret` read nowhere —
/// in the body, or by the caller as `CALLEE.__ret`, which is how a `struct`
/// it returns is reached.
std::optional<std::size_t> resultStore( ir::Function& caller,
                                        ir::Call const& call,
                                        ir::Block& body,
                                        std::string const& returned,
                                        ir::Type type,
                                        std::string const& callee )
{
  if ( !call.result.has_value() || caller.values.at( call.result->index ) != type )
  {
    return std::nullopt;
  }
  std::string const was = callee + "." + std::string{ ir::RESULT };
  for ( ir::Block& block : caller.blocks )
  {
    for ( ir::Instruction& instruction : block.instructions )
    {
      bool named = false;
      ir::eachName( instruction,
                    [&was, &named]( std::string const& name ) { named = named || ir::baseOf( name ) == was; } );
      if ( named )
      {
        return std::nullopt;
      }
    }
  }
  std::optional<std::size_t> found;
  std::size_t mentions = 0;
  for ( std::size_t index = 0; index < body.instructions.size(); ++index )
  {
    ir::eachName( body.instructions[index],
                  [&returned, &mentions]( std::string const& name )
                  {
                    if ( ir::baseOf( name ) == returned )
                    {
                      ++mentions;
                    }
                  } );
    auto const* const store = std::get_if<ir::Store>( &body.instructions[index].operation );
    if ( store != nullptr && store->name == returned )
    {
      found = index;
    }
  }
  if ( !found.has_value() || mentions != 1 )
  {
    return std::nullopt;
  }
  ir::Operand const& value = std::get<ir::Store>( body.instructions[*found].operation ).value;
  if ( std::holds_alternative<ir::Object>( value ) )
  {
    // An object may be written between the body and the call's readers.
    return std::nullopt;
  }
  return found;
}

/// Every operand of the caller that reads the value `from`, made `to`.
void replaceValue( ir::Function& caller, std::uint32_t from, ir::Operand const& to )
{
  for ( ir::Block& block : caller.blocks )
  {
    ir::eachOperand( block,
                     [from, &to]( ir::Operand& operand )
                     {
                       auto const* const value = std::get_if<ir::Value>( &operand );
                       if ( value != nullptr && value->index == from )
                       {
                         operand = to;
                       }
                     } );
  }
}

/// A body of one block where the call stood, in the call's own block: nothing
/// is cut and no block is added, so a value of the caller's is still defined
/// in the block that reads it. That is what lets a parameter the body only
/// reads be the argument's value itself rather than a byte the argument is
/// stored to, and the call's readers read what the body stored to `__ret` —
/// a wrapped call costs what its body costs, and not that and the frame's
/// bytes besides.
void spliceInPlace(
    ir::Function& caller, ir::Function const& callee, std::uint32_t at, std::size_t index, bool sameFile )
{
  auto const call = std::get<ir::Call>( caller.blocks[at].instructions[index].operation );
  diag::SourceLocation const site = caller.blocks[at].instructions[index].at;
  auto const [names, returned] = namesFor( caller, callee );
  adoptLocals( caller, callee, names );

  auto const base = static_cast<std::uint32_t>( caller.values.size() );
  caller.values.insert( caller.values.end(), callee.values.begin(), callee.values.end() );

  ir::Block body = callee.blocks.front();
  for ( ir::Instruction& instruction : body.instructions )
  {
    raiseValues( instruction, base );
    renameIn( instruction, names );
    if ( !sameFile )
    {
      instruction.at = site;
    }
  }

  std::vector<ir::Instruction> spliced;
  for ( std::size_t which = 0; which < call.arguments.size(); ++which )
  {
    ir::Argument const& argument = call.arguments[which];
    ir::Local const& parameter = callee.parameters[which];
    std::string const& name = names.at( parameter.name );
    bool const asValue = !argument.from.has_value() && std::holds_alternative<ir::Value>( argument.value ) &&
                         ir::typeOf( argument.value, caller ) == parameter.type &&
                         onlyRead( body, name, parameter.type );
    if ( !asValue )
    {
      caller.locals.push_back(
          ir::Local{ .name = name, .type = parameter.type, .bytes = parameter.bytes, .place = {} } );
      spliced.push_back( argumentStore( argument, name, site ) );
      continue;
    }
    for ( ir::Instruction& instruction : body.instructions )
    {
      ir::eachOperand( instruction,
                       [&name, &argument]( ir::Operand& operand )
                       {
                         auto const* const object = std::get_if<ir::Object>( &operand );
                         if ( object != nullptr && object->name == name )
                         {
                           operand = argument.value;
                         }
                       } );
    }
  }

  std::optional<std::size_t> const stored =
      callee.result.has_value() ? resultStore( caller, call, body, returned, *callee.result, callee.name )
                                : std::nullopt;
  std::optional<ir::Operand> result;
  if ( stored.has_value() )
  {
    result = std::get<ir::Store>( body.instructions[*stored].operation ).value;
    body.instructions.erase( body.instructions.begin() + static_cast<std::ptrdiff_t>( *stored ) );
  }
  else if ( callee.result.has_value() )
  {
    caller.locals.push_back(
        ir::Local{ .name = returned, .type = *callee.result, .bytes = callee.resultBytes, .place = {} } );
  }
  spliced.insert( spliced.end(),
                  std::make_move_iterator( body.instructions.begin() ),
                  std::make_move_iterator( body.instructions.end() ) );

  std::vector<ir::Instruction>& instructions = caller.blocks[at].instructions;
  auto const where = instructions.erase( instructions.begin() + static_cast<std::ptrdiff_t>( index ) );
  instructions.insert( where, std::make_move_iterator( spliced.begin() ), std::make_move_iterator( spliced.end() ) );

  // `resultStore` found one only where the call is read as a value.
  if ( result.has_value() && call.result.has_value() )
  {
    replaceValue( caller, call.result->index, *result );
  }
  else if ( callee.result.has_value() )
  {
    readResultFrom( caller, call, returned, *callee.result, callee.name );
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

  auto const [names, returned] = namesFor( caller, callee );

  // The bytes the body needs are the caller's now. A byte the body turns out
  // never to read is `dropDeadStores`'s to take out, not this pass's.
  for ( ir::Local const& parameter : callee.parameters )
  {
    caller.locals.push_back( ir::Local{
        .name = names.at( parameter.name ), .type = parameter.type, .bytes = parameter.bytes, .place = {} } );
  }
  adoptLocals( caller, callee, names );
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
    renameIn( one.terminator, names );
    if ( !sameFile )
    {
      one.terminator.at = site;
    }
    if ( one.terminator.kind == ir::TerminatorKind::RETURN )
    {
      one.terminator.kind = ir::TerminatorKind::JUMP;
      one.terminator.target = after;
      one.terminator.otherwise = 0;
    }
    else
    {
      ir::eachTarget( one.terminator, [entry]( std::uint32_t& into ) { into += entry; } );
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
    ir::eachTarget( one.terminator, raiseBlock );
  }
  ir::eachTarget( end, raiseBlock );
  for ( ir::WithRegion& region : caller.withs )
  {
    raiseBlock( region.begin );
    raiseBlock( region.end );
  }

  for ( std::size_t which = 0; which < call.arguments.size(); ++which )
  {
    block.instructions.push_back(
        argumentStore( call.arguments[which], names.at( callee.parameters[which].name ), site ) );
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

void wrapCalls( std::span<std::optional<ir::Unit>> units, std::span<diag::DiagnosticSink* const> sinks, bool forSpeed )
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
        ir::Function const& body = *callee->function;

        // The programmer's word, and nothing that would have asked for a wrap
        // outweighs it; `inline` beside it is the one that is refused.
        if ( body.isNoinline )
        {
          refuse( *callee, callee->unit, body.inlineSpan, "is written `[[noinline]]`", false );
          continue;
        }

        // What asks for it: a call site that is its callee's only one, which
        // needs no justification of any other kind; `inline` written on the
        // callee, which is the programmer's to ask for; a body that costs no
        // more than the call, or under `speed` no more than that and
        // SPEED_ALLOWANCE, which is a copy that buys the frame — see
        // docs/decisions/0299-a-body-no-larger-than-its-call-is-wrapped.md;
        // or, under `speed`, a constant handed to the callee, which is what
        // makes a copy of the body worth more than the call — see
        // docs/decisions/0177-intent.md.
        bool const handsAConstant =
            forSpeed && std::ranges::any_of( call->arguments,
                                             []( ir::Argument const& argument )
                                             { return std::holds_alternative<ir::Constant>( argument.value ); } );
        bool const small = bodyCost( body ) <= callCost( body ) + ( forSpeed ? SPEED_ALLOWANCE : 0 );
        if ( sites[callee->function] != 1 && !body.isInline && !handsAConstant && !small )
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
        if ( body.blocks.size() == 1 && body.blocks.front().terminator.kind == ir::TerminatorKind::RETURN )
        {
          // What stands where the call stood is looked at next, as a body
          // spliced in blocks of its own is in the block after this one.
          spliceInPlace( caller, body, at, index, callee->unit == where.unit );
          --index;
          continue;
        }
        splice( caller, body, at, index, callee->unit == where.unit );

        // The block was cut at the call, so what is left of it holds no
        // instruction this loop has not seen.
        break;
      }
    }
  }
}

} // namespace nga::c
