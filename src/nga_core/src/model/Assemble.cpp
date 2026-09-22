#include "nga/model/Assemble.hpp"

#include "nga/c/Compile.hpp"

#include "nga/model/Constants.hpp"
#include "nga/model/Driver.hpp"
#include "nga/model/Evaluate.hpp"
#include "nga/model/Merge.hpp"
#include "nga/model/Panes.hpp"
#include "nga/model/Regions.hpp"

#include "nga/model/Slots.hpp"

#include "nga/model/ModuleBuilder.hpp"
#include "nga/model/Transform.hpp"
#include "nga/model/Transition.hpp"
#include "nga/syntax/Lexer.hpp"
#include "nga/syntax/Literal.hpp"
#include "nga/syntax/Parser.hpp"
#include "nga/syntax/TokenCursor.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <utility>
#include <variant>
#include <vector>

namespace nga::model
{

namespace
{

/// The most bytes a name of the assembler is memory of in C: the address
/// space.
constexpr std::int64_t MAX_MEMORY = 65536;

/// How many Constants naming one another are followed to what the last one
/// names, which is more than a program writes and bounds a cycle, reported
/// after Merge.
constexpr std::uint32_t MAX_CHAIN = 64;

bool isCompiled( diag::SourceManager const& sources, ProjectModule const& entry )
{
  return entry.generated == Generated::NONE &&
         std::filesystem::path{ sources.pathOf( entry.file ) }.extension() == ".ngc";
}

/// Whether a Symbol is the entry of a `.proc` — a Label at the first Chunk of
/// a Proc — which is the one thing of the assembler's that C may call.
bool isProcEntry( Module const& module, Symbol const& symbol )
{
  if ( symbol.kind != SymbolKind::LABEL )
  {
    return false;
  }
  auto const* position = std::get_if<LabelPosition>( &symbol.value );
  return position != nullptr && position->chunk.value == 0 && !position->inner.has_value() &&
         module.sectionAt( position->section ).isProc();
}

c::ExternalName shapeless( c::ExternalName name, std::string reason )
{
  name.kind = c::ExternalKind::SHAPELESS;
  name.reason = std::move( reason );
  return name;
}

/// Bytes C reads at a name: a `u8` of one, a `u16` of two, and an array of
/// bytes of more.
c::ExternalName memoryOf( c::ExternalName name, std::uint32_t bytes )
{
  name.kind = c::ExternalKind::MEMORY;
  name.type = bytes == 2 ? c::ir::Type::U16 : c::ir::Type::U8;
  if ( bytes > 2 )
  {
    name.isArray = true;
    name.count = bytes;
  }
  return name;
}

/// What a Label is in C: the shape of the run of one kind of directive that
/// stands at it, up to a Label at a later Chunk of its Section or the
/// Section's end — see
/// docs/decisions/0092-what-the-assembler-exports-is-typed-by-its-shape.md.
c::ExternalName labelShape( diag::SourceManager const& sources,
                            GlobalSymbols const& symbols,
                            ModuleIndex home,
                            LabelPosition position,
                            c::ExternalName name )
{
  Module const& module = symbols.moduleAt( home );
  Section const& section = module.sectionAt( position.section );
  auto end = static_cast<std::uint32_t>( section.chunks().size() );
  for ( Symbol const& other : module.symbols().symbols() )
  {
    auto const* at = std::get_if<LabelPosition>( &other.value );
    if ( other.kind == SymbolKind::LABEL && at != nullptr && at->section == position.section &&
         at->chunk.value > position.chunk.value )
    {
      end = std::min( end, at->chunk.value );
    }
  }
  if ( position.chunk.value >= end )
  {
    return shapeless( std::move( name ), "nothing stands at it" );
  }
  // Which branch of a conditional is kept is Expand's, after the compiler.
  for ( Conditional const& conditional : section.conditionals() )
  {
    for ( Branch const& branch : conditional.branches )
    {
      if ( branch.first.value < end && position.chunk.value < branch.last.value )
      {
        return shapeless( std::move( name ), "what stands at it is chosen by a conditional" );
      }
    }
  }

  std::optional<syntax::DataWidth> width;
  bool reserves = false;
  std::uint32_t count = 0;
  bool counted = true;
  for ( std::uint32_t index = position.chunk.value; index < end; ++index )
  {
    Chunk const& chunk = section.chunks()[index];
    std::span<syntax::ExpressionPtr const> const items = section.itemsOf( chunk );
    if ( auto const* data = std::get_if<DataContent>( &chunk.content ); data != nullptr && !reserves )
    {
      if ( width.has_value() && *width != data->width )
      {
        return shapeless( std::move( name ), "it holds both `.byte` and `.word`" );
      }
      width = data->width;
      for ( syntax::ExpressionPtr const& item : items )
      {
        counted = counted && item->kind != syntax::ExpressionKind::STRING;
        ++count;
      }
      continue;
    }
    if ( std::holds_alternative<ReserveContent>( chunk.content ) && !width.has_value() && items.size() == 1 )
    {
      reserves = true;
      std::optional<std::int64_t> const size = declaredValueOf( sources, symbols, nullptr, home, *items.front() );
      if ( !size.has_value() || *size < 1 || *size > MAX_MEMORY )
      {
        return shapeless( std::move( name ), "it reserves bytes of no number the compiler folds" );
      }
      count = static_cast<std::uint32_t>( std::min<std::int64_t>( count + *size, MAX_MEMORY ) );
      continue;
    }
    if ( width.has_value() || reserves )
    {
      return shapeless( std::move( name ), "it holds more than data of one kind" );
    }
    return shapeless( std::move( name ), "it holds no data" );
  }
  if ( reserves )
  {
    return memoryOf( std::move( name ), count );
  }
  name.kind = c::ExternalKind::MEMORY;
  name.type = width == syntax::DataWidth::WORD ? c::ir::Type::U16 : c::ir::Type::U8;
  name.isArray = true;
  if ( counted )
  {
    name.count = count;
  }
  return name;
}

/// What stands at a Label to the end of its Section, item by item, and past
/// other Labels: what an `extern` declaration is held to — see
/// docs/decisions/0093-extern-declares-how-c-reads-the-assembler.md. It ends
/// early, saying why, at the first Chunk that holds no data it can read.
void runOf( diag::SourceManager const& sources,
            GlobalSymbols const& symbols,
            ModuleIndex home,
            LabelPosition position,
            c::ExternalName& name )
{
  Section const& section = symbols.moduleAt( home ).sectionAt( position.section );
  for ( auto index = position.chunk.value; index < section.chunks().size(); ++index )
  {
    bool const chosen = std::ranges::any_of(
        section.conditionals(),
        [index]( Conditional const& conditional )
        {
          return std::ranges::any_of( conditional.branches,
                                      [index]( Branch const& branch )
                                      { return branch.first.value <= index && index < branch.last.value; } );
        } );
    if ( chosen )
    {
      name.stop = "what stands there is chosen by a conditional";
      return;
    }
    Chunk const& chunk = section.chunks()[index];
    std::span<syntax::ExpressionPtr const> const items = section.itemsOf( chunk );
    if ( auto const* data = std::get_if<DataContent>( &chunk.content ) )
    {
      std::uint32_t const width = data->width == syntax::DataWidth::WORD ? 2U : 1U;
      for ( syntax::ExpressionPtr const& item : items )
      {
        if ( item->kind == syntax::ExpressionKind::STRING )
        {
          // A Charset maps each character to one byte, whatever the byte is.
          std::size_t const characters =
              syntax::codePointsOf( syntax::quotedOf( sources.textOf( item->token.span() ) ).body ).size();
          name.cells.insert( name.cells.end(), characters, c::ExternalCell{ .bytes = 1, .isReserved = false } );
          continue;
        }
        name.cells.push_back( c::ExternalCell{
            .bytes = width, .isReserved = false, .value = declaredValueOf( sources, symbols, nullptr, home, *item ) } );
      }
      continue;
    }
    if ( std::holds_alternative<ReserveContent>( chunk.content ) && items.size() == 1 )
    {
      std::optional<std::int64_t> const size = declaredValueOf( sources, symbols, nullptr, home, *items.front() );
      if ( !size.has_value() || *size < 0 || *size > MAX_MEMORY )
      {
        name.stop = "it reserves bytes of no number the compiler folds";
        return;
      }
      if ( *size > 0 )
      {
        name.cells.push_back( c::ExternalCell{ .bytes = static_cast<std::uint32_t>( *size ), .isReserved = true } );
      }
      continue;
    }
    name.stop = std::holds_alternative<InstructionContent>( chunk.content ) ? "it holds code" : "it holds no data";
    return;
  }
}

/// What a Constant is in C: the number it folds to, or a `u16` only the
/// assembler knows; or nothing C reads, where it is a string or names what C
/// does not read.
c::ExternalName
constantShape( diag::SourceManager const& sources, GlobalSymbols const& symbols, SymbolRef where, c::ExternalName name )
{
  name.kind = c::ExternalKind::CONSTANT;
  Symbol const* symbol = &symbols.at( where );
  ModuleIndex home = where.module;
  std::optional<std::int64_t> value;
  if ( auto const& expression = std::get<syntax::ExpressionPtr>( symbol->value ); expression != nullptr )
  {
    value = declaredValueOf( sources, symbols, nullptr, home, *expression );
  }
  if ( value.has_value() )
  {
    name.value = value;
    return name;
  }
  // A chain of Constants each naming the next, as far as a name alone goes.
  for ( std::uint32_t step = 0; step < MAX_CHAIN; ++step )
  {
    auto const& expression = std::get<syntax::ExpressionPtr>( symbol->value );
    if ( expression == nullptr )
    {
      return name;
    }
    if ( expression->kind == syntax::ExpressionKind::STRING )
    {
      name.kind = c::ExternalKind::UNREAD;
      name.reason = "a string";
      return name;
    }
    if ( expression->kind != syntax::ExpressionKind::NAME )
    {
      return name;
    }
    std::optional<SymbolRef> const named =
        symbols.resolveText( home, *expression, sources.textOf( expression->token.span() ) );
    if ( !named.has_value() )
    {
      return name;
    }
    symbol = &symbols.at( *named );
    home = named->module;
    switch ( symbol->kind )
    {
    case SymbolKind::CONSTANT:
      continue;
    case SymbolKind::CHARSET:
    case SymbolKind::WINDOW:
    case SymbolKind::PANE:
    case SymbolKind::MACRO:
      name.kind = c::ExternalKind::UNREAD;
      name.reason = "a " + std::string{ nameOf( symbol->kind ) };
      return name;
    case SymbolKind::LABEL:
    case SymbolKind::SLOT:
    case SymbolKind::REGION:
      return name;
    }
  }
  return name;
}

/// A declared byte of a Proc as C hands it over: by its Label's name, of the
/// type written, or of the shape of its reservation where none is.
std::optional<c::ExternalByte> declaredByte( diag::SourceManager const& sources,
                                             GlobalSymbols const& symbols,
                                             ModuleIndex home,
                                             Declared const& declared )
{
  Module const& module = symbols.moduleAt( home );

  // Bytes in registers: as many as the place has letters, a `u8` or a
  // `u16`, and a name only where some of them are the reservation's — see
  // docs/decisions/0145-an-argument-in-a-register.md.
  if ( !declared.place.empty() )
  {
    c::ExternalByte placed{ .name = {},
                            .type = declared.placedBytes() == 2 ? c::ir::Type::U16 : c::ir::Type::U8,
                            .isBytes = false,
                            .bytes = declared.placedBytes(),
                            .isWritten = false,
                            .place = declared.place };
    if ( !declared.temporary.has_value() )
    {
      return placed;
    }
    auto const holder = std::ranges::find_if( module.symbols().symbols(),
                                              [&]( Symbol const& symbol )
                                              {
                                                auto const* at = std::get_if<LabelPosition>( &symbol.value );
                                                return symbol.kind == SymbolKind::LABEL && at != nullptr &&
                                                       at->section == *declared.temporary && at->chunk.value == 0;
                                              } );
    if ( holder == module.symbols().symbols().end() )
    {
      return std::nullopt;
    }
    placed.name = std::string{ holder->name };
    return placed;
  }
  if ( !declared.temporary.has_value() )
  {
    return std::nullopt;
  }
  auto const named = std::ranges::find_if( module.symbols().symbols(),
                                           [&]( Symbol const& symbol )
                                           {
                                             auto const* at = std::get_if<LabelPosition>( &symbol.value );
                                             return symbol.kind == SymbolKind::LABEL && at != nullptr &&
                                                    at->section == *declared.temporary && at->chunk.value == 0;
                                           } );
  if ( named == module.symbols().symbols().end() )
  {
    return std::nullopt;
  }
  c::ExternalByte byte{ .name = std::string{ named->name },
                        .type = c::ir::Type::U8,
                        .isBytes = false,
                        .bytes = 1,
                        .isWritten = false,
                        .place = {} };
  if ( declared.type.has_value() )
  {
    byte.isWritten = true;
    byte.bytes = declared.type->bytes();
    switch ( declared.type->kind )
    {
    case DeclaredType::Kind::U8:
      break;
    case DeclaredType::Kind::I8:
      byte.type = c::ir::Type::I8;
      break;
    case DeclaredType::Kind::U16:
      byte.type = c::ir::Type::U16;
      break;
    case DeclaredType::Kind::I16:
      byte.type = c::ir::Type::I16;
      break;
    case DeclaredType::Kind::BOOL:
      byte.type = c::ir::Type::BOOL;
      break;
    case DeclaredType::Kind::BYTES:
      byte.isBytes = true;
      break;
    }
    return byte;
  }
  Section const& section = module.sectionAt( *declared.temporary );
  std::optional<std::int64_t> size;
  if ( !section.chunks().empty() )
  {
    std::span<syntax::ExpressionPtr const> const items = section.itemsOf( section.chunks().front() );
    if ( items.size() == 1 )
    {
      size = declaredValueOf( sources, symbols, nullptr, home, *items.front() );
    }
  }
  byte.type = size == 2 ? c::ir::Type::U16 : c::ir::Type::U8;
  byte.isBytes = size != 1 && size != 2;
  byte.bytes = static_cast<std::uint32_t>( std::clamp<std::int64_t>( size.value_or( 0 ), 0, MAX_MEMORY ) );
  return byte;
}

/// The name of a Window's state by its index: a named state's own, and for a
/// Bank of a unit set none, since the source names no Bank.
std::string stateNameOf( Target const& target, Window const& window, std::uint32_t state )
{
  std::uint32_t index = 0;
  for ( WindowState const& candidate : window.states )
  {
    if ( !candidate.units.has_value() && state == index )
    {
      return candidate.name;
    }
    index += candidate.units.has_value() ? target.unitSets[candidate.units->value].count : 1;
  }
  return {};
}

/// The Pane a Label's Section is `in`, or a Proc runs `under`, by the name
/// written on it, where the Project declares one of that name; a name it
/// does not is the assembler's to refuse, and C is told of no Pane.
std::string paneOf(
    diag::SourceManager const& sources, Module const& module, Target const& target, Symbol const& symbol, bool under )
{
  auto const* const position = std::get_if<LabelPosition>( &symbol.value );
  if ( position == nullptr )
  {
    return {};
  }
  Section const& section = module.sectionAt( position->section );
  std::optional<syntax::Token> const written = under ? section.underName() : section.paneName();
  if ( !written.has_value() )
  {
    return {};
  }
  std::string_view const text = sources.textOf( written->span() );
  return target.paneNamed( text ).has_value() ? std::string{ text } : std::string{};
}

/// What an exported Symbol is in C.
c::ExternalName
externalOf( diag::SourceManager const& sources, GlobalSymbols const& symbols, Target const& target, SymbolRef where )
{
  Module const& module = symbols.moduleAt( where.module );
  Symbol const& symbol = symbols.at( where );
  c::ExternalName name{ .name = std::string{ symbol.name },
                        .kind = c::ExternalKind::UNREAD,
                        .definition = symbol.definition };
  name.pane = paneOf( sources, module, target, symbol, false );
  name.under = paneOf( sources, module, target, symbol, true );
  if ( auto const* const position = std::get_if<LabelPosition>( &symbol.value ); position != nullptr )
  {
    // The function type a Proc is declared `as`: C reads its arguments there,
    // since a member declares none of its own — see
    // docs/decisions/0065-handlers.md.
    Section const& section = module.sectionAt( position->section );
    if ( std::optional<syntax::Token> const written = section.asName(); written.has_value() )
    {
      name.as = std::string{ sources.textOf( written->span() ) };
    }
  }
  switch ( symbol.kind )
  {
  case SymbolKind::LABEL:
    if ( isProcEntry( module, symbol ) )
    {
      name.kind = c::ExternalKind::PROC;
      Signature const& signature = module.sectionAt( std::get<LabelPosition>( symbol.value ).section ).signature();
      for ( Declared const& argument : signature.arguments )
      {
        if ( std::optional<c::ExternalByte> byte = declaredByte( sources, symbols, where.module, argument ) )
        {
          name.arguments.push_back( std::move( *byte ) );
        }
      }
      if ( signature.result.has_value() )
      {
        name.result = declaredByte( sources, symbols, where.module, *signature.result );
      }
      return name;
    }
    {
      LabelPosition const position = std::get<LabelPosition>( symbol.value );
      runOf( sources, symbols, where.module, position, name );
      return labelShape( sources, symbols, where.module, position, std::move( name ) );
    }
  case SymbolKind::CONSTANT:
    return constantShape( sources, symbols, where, std::move( name ) );
  case SymbolKind::REGION:
  {
    Region const& region = target.regions[std::get<RegionValue>( symbol.value ).region.value];
    if ( region.property == RegionProperty::RESERVED )
    {
      name.kind = c::ExternalKind::RESERVED;
      return name;
    }
    name.cells.push_back( c::ExternalCell{ .bytes = region.range.size(), .isReserved = true } );
    name.isRegion = true;
    return memoryOf( std::move( name ), region.range.size() );
  }
  case SymbolKind::CHARSET:
    name.kind = c::ExternalKind::CHARSET;
    name.reason = "a character set";
    return name;
  case SymbolKind::PANE:
  {
    // A Pane or a family, with its Window, its count and the state it is
    // pinned to, which `[[in]]` and `[[with]]` are held to — see
    // docs/decisions/0096-panes-in-c.md.
    Pane const& pane = target.panes[std::get<PaneIndex>( symbol.value ).value];
    Window const& window = target.windows[pane.window.value];
    name.kind = c::ExternalKind::PANE;
    name.reason = pane.count > 1 ? "a family" : "a pane";
    name.window = window.name;
    name.paneCount = pane.count;
    name.paneState = pane.state.has_value() ? stateNameOf( target, window, *pane.state ) : std::string{};
    return name;
  }
  case SymbolKind::WINDOW:
    name.kind = c::ExternalKind::WINDOW;
    name.reason = "a window";
    return name;
  case SymbolKind::SLOT:
  case SymbolKind::MACRO:
    name.reason = "a " + std::string{ nameOf( symbol.kind ) };
    return name;
  }
  return name;
}

} // namespace

Module assembleModule( diag::SourceManager const& sources,
                       diag::FileId file,
                       std::string name,
                       Residency residency,
                       diag::DiagnosticSink& sink )
{
  Module module{ std::move( name ), file, std::move( residency ) };

  std::vector<syntax::Token> const tokens = syntax::tokenize( sources, file, sink );
  syntax::TokenCursor cursor{ tokens };
  ModuleBuilder builder{ sources, sink, module };
  syntax::Parser parser{ sources, cursor, builder, sink };

  parser.parseModule();
  builder.finish();

  return module;
}

/// The Panes pinned to a state that is the base of their Window in every
/// Phase the Module is present in — the variant's base for a Module in no
/// Phase — which the compiler does not wrap a call into, since what the
/// `.with` would show is shown already; where the Phases differ, nothing, and
/// the `.with` the compiler then writes is what `NGA3010` refuses. See
/// docs/decisions/0096-panes-in-c.md.
std::vector<std::string> shownByBase( Project const& project, ProjectModule const& entry )
{
  Target const& target = project.target;
  std::vector<std::string> shown;
  for ( Pane const& pane : target.panes )
  {
    if ( !pane.state.has_value() )
    {
      continue;
    }
    std::optional<std::uint32_t> base;
    bool any = false;
    bool differs = false;
    for ( std::uint32_t phase = 0; phase < entry.residency.phaseCount(); ++phase )
    {
      if ( !entry.residency.includes( PhaseIndex{ phase } ) )
      {
        continue;
      }
      std::optional<std::uint32_t> const here = baseIn( project, PhaseIndex{ phase }, pane.window );
      differs = differs || ( any && here != base );
      base = here;
      any = true;
    }
    if ( !any )
    {
      base = target.windows[pane.window.value].base;
    }
    if ( !differs && base == pane.state )
    {
      shown.push_back( pane.name );
    }
  }
  return shown;
}

std::vector<Module> assembleProject( diag::SourceManager& sources, Project& project, diag::DiagnosticSink& sink )
{
  std::vector<Module> modules;
  modules.reserve( project.modules.size() );

  for ( ProjectModule const& entry : project.modules )
  {
    if ( isCompiled( sources, entry ) )
    {
      // Stands at its entry's index until the second wave compiles it, and
      // stays as it is, holding no Sections, when the file holds an error.
      modules.emplace_back( entry.name, entry.file, entry.residency );
      continue;
    }
    if ( entry.generated == Generated::REGIONS )
    {
      modules.push_back( buildRegionModule( sources, entry, project.target ) );
      continue;
    }
    if ( entry.generated == Generated::CONSTANTS )
    {
      modules.push_back( buildConstantModule( sources, entry, project.constants ) );
      continue;
    }
    if ( entry.generated != Generated::NONE )
    {
      // A table or the Cell: built rather than assembled, and reporting
      // nothing, since there is no text to be wrong.
      modules.push_back( buildGeneratedModule( sources, entry ) );
      if ( entry.outsideWindow )
      {
        modules.back().keepOutsideWindow();
      }
      continue;
    }
    diag::DiagnosticSink found{ sink.policy() };
    modules.push_back( assembleModule( sources, entry.file, entry.name, entry.residency, found ) );
    if ( entry.outsideWindow )
    {
      modules.back().keepOutsideWindow();
    }
    sink.merge( std::move( found ) );
  }

  // The second wave: every `.ngc` of the program compiles once every other
  // Module has assembled, against the names those export, and the text each
  // becomes assembles like any other — see docs/decisions/0062-a-subset-of-c.md.
  // The text is ranked beside its `.ngc`, so a finding on it sorts with its
  // Module.
  std::vector<c::SourceFile> files;
  std::vector<std::size_t> compiledAt;
  std::vector<c::ExternalName> externals;
  GlobalSymbols const symbols{ modules };
  for ( std::size_t index = 0; index < project.modules.size(); ++index )
  {
    ProjectModule const& entry = project.modules[index];
    if ( isCompiled( sources, entry ) )
    {
      files.push_back(
          c::SourceFile{ .file = entry.file, .path = entry.path, .shownByBase = shownByBase( project, entry ) } );
      compiledAt.push_back( index );
      continue;
    }
    std::vector<Symbol> const& table = modules[index].symbols().symbols();
    for ( std::uint32_t at = 0; at < table.size(); ++at )
    {
      if ( table[at].exported )
      {
        externals.push_back(
            externalOf( sources,
                        symbols,
                        project.target,
                        SymbolRef{ .module = ModuleIndex{ static_cast<std::uint32_t>( index ) }, .index = at } ) );
      }
    }
  }

  // The Target's registers, which C reads and writes as `volatile` whatever
  // it declares — see docs/decisions/0151-volatile.md.
  std::vector<c::AddressRange> registers;
  for ( Region const& region : project.target.regions )
  {
    if ( region.property == RegionProperty::REGISTER )
    {
      registers.push_back(
          c::AddressRange{ .begin = region.range.begin, .end = region.range.end, .name = region.name } );
    }
  }
  // The Intent is the Project's and the compiler is told it, so that what a
  // Project asked for reaches the one pass that reads it — see
  // docs/decisions/0177-intent.md.
  auto const intentOf = []( Intent const stated )
  {
    switch ( stated )
    {
    case Intent::SPEED:
      return c::Intent::SPEED;
    case Intent::SIZE:
      return c::Intent::SIZE;
    case Intent::FIT:
      break;
    }
    return c::Intent::FIT;
  };
  c::Cpu const cpu = project.target.cpu == Cpu::WDC65SC02 ? c::Cpu::WDC65SC02 : c::Cpu::MOS6502;
  std::vector<std::optional<std::string>> texts =
      c::compile( sources, files, externals, sink, registers, intentOf( project.intent ), cpu );
  for ( std::size_t at = 0; at < texts.size(); ++at )
  {
    std::optional<std::string>& text = texts[at];
    if ( !text.has_value() )
    {
      continue;
    }
    ProjectModule& entry = project.modules[compiledAt[at]];
    diag::FileId const written =
        sources.addFileAfter( entry.file, "<generated>/" + entry.name + ".asm", std::move( *text ) );
    entry.compiledText = written;
    diag::DiagnosticSink found{ sink.policy() };
    modules[compiledAt[at]] = assembleModule( sources, written, entry.name, entry.residency, found );
    sink.merge( std::move( found ) );
  }

  // What each Module read as `.source` is handed to the SourceManager here,
  // once, with every Module in hand: a finding of any later Step is rendered
  // against it, and the Modules were assembled with the manager shared.
  for ( Module const& module : modules )
  {
    for ( SourceMark const& mark : module.sourceMarks() )
    {
      sources.addSourceMark( mark.from, mark.path, mark.line );
    }
  }

  // The end of Assemble: a Slot needs its Cell, a `.transition` names a
  // Phase, a table needs its load sets and counts the Slots, a `transform`
  // names a Section, the driver's roles are bound — all of which need every
  // Module in hand. A macro use waits for Expand, after Merge, since it is
  // looked up as any Symbol is — see docs/decisions/0049-recursive-macros.md.
  addSlotCells( project, modules, sink );
  if ( project.transitionRoutine.has_value() )
  {
    resolveTransitions( sources, project, modules, sink );
  }
  resolveTransforms( sources, project, modules, sink );
  resolveDriver( sources, project, modules, sink );
  resolvePanes( sources, project, modules, sink );
  if ( project.transitionRoutine.has_value() )
  {
    addTransformDispatcher( sources, project, modules, sink );
  }

  return modules;
}

} // namespace nga::model
