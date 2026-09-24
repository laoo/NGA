#include "nga/model/ProjectFile.hpp"

#include "nga/model/Atr.hpp"

#include <spdlog/fmt/fmt.h>

#include "nga/model/Evaluate.hpp"
#include "nga/model/Generator.hpp"
#include "nga/model/Runtime.hpp"
#include "nga/model/Transition.hpp"
#include "nga/syntax/Lexer.hpp"
#include "nga/syntax/Literal.hpp"
#include "nga/syntax/ProjectParser.hpp"
#include "nga/syntax/TokenCursor.hpp"

#include <algorithm>
#include <cstddef>
#include <span>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nga::model
{

namespace
{

/// A unit is named in a byte: in a Frame, and in the accumulator the driver
/// is handed it in.
constexpr std::int64_t MOST_UNITS = 256;

/// A family's members are consecutive Banks, and a Bank is named in a byte.
constexpr std::int64_t MOST_PANE_MEMBERS = 255;

/// A name already in a set adds nothing and is not reported: an include
/// repeating what the including file said is the normal case.
template <typename List, typename Value>
void addOnce( List& list, Value value )
{
  if ( std::ranges::find( list, value ) == list.end() )
  {
    list.push_back( value );
  }
}

/// Reads one document and everything it includes, in document order, and
/// resolves what the text named once the last include has returned.
class Loader final : public syntax::ProjectBuilder, public GeneratorFiles
{
public:
  Loader( diag::SourceManager& sources,
          FileReader& files,
          std::filesystem::path library,
          diag::SeverityPolicy& policy,
          diag::DiagnosticSink& sink )
      : mSources( &sources ), mFiles( &files ), mLibrary( std::move( library ) ), mPolicy( &policy ), mSink( &sink )
  {
  }

  void readDocument( std::filesystem::path const& document, std::optional<syntax::Token> from );

  /// Turns what every `phase`, `resident` and `entry` said into the PhaseGraph
  /// and each Module's Residency. Runs once, after the whole document: a block
  /// may name a Module declared below it or in a file included later, and
  /// resolving as each entry was met would work on every example written by
  /// hand and fail on a generated one.
  void resolvePhases( std::filesystem::path const& root );

  void addModule( syntax::Token path, std::optional<syntax::Token> alias, std::optional<syntax::Token> group ) override;
  void addGeneratedModule( syntax::Token generator,
                           std::vector<syntax::ProjectBuilder::GeneratorArgument> arguments,
                           std::optional<syntax::Token> alias,
                           std::optional<syntax::Token> group ) override;

  /// A generator reads a file as a Module's path is read: beside the document
  /// that wrote the entry, and then in the library.
  [[nodiscard]] std::optional<std::string> read( std::string const& path, std::string& resolved ) const override;
  void declareGroup( syntax::Token name ) override;
  void groupMember( syntax::Token group, syntax::Token member ) override;
  void setSeverity( syntax::Token code, diag::SeverityOverride action ) override;
  void addConstant( syntax::Token name, syntax::Token value ) override;
  void setContainer( syntax::Token name ) override;
  void addAcceptedContainer( syntax::Token keyword, syntax::Token name ) override;
  void setCpu( syntax::Token keyword, syntax::Token name ) override;
  void setIntent( syntax::Token name ) override;
  void includeDocument( syntax::Token path ) override;
  void declarePhase( syntax::Token name ) override;
  void phaseNeeds( syntax::Token phase, syntax::Token module ) override;
  void phaseLeadsTo( syntax::Token phase, syntax::Token next ) override;
  void phaseBase( syntax::Token phase, syntax::Token window, syntax::Token state ) override;
  void groupBase( syntax::Token group, syntax::Token window, syntax::Token state ) override;
  void addResident( syntax::Token module ) override;
  void applyTransform( syntax::Token transform, syntax::Token module, std::vector<syntax::Token> section ) override;
  void setEntry( syntax::Token phase ) override;
  void setStorageUnits( syntax::Token keyword, syntax::Token name ) override;
  void setUnitCount( syntax::Token keyword, syntax::ExpressionPtr value ) override;
  void setUnitSize( syntax::Token keyword, syntax::ExpressionPtr value ) override;
  void addUnitSet( syntax::Token keyword, syntax::Token name, syntax::ExpressionPtr count ) override;
  void beginPanes( syntax::Token keyword, syntax::Token window, std::optional<syntax::Token> state ) override;
  void addPane( syntax::Token name, syntax::ExpressionPtr count ) override;
  void addWindow( syntax::Token keyword,
                  syntax::Token name,
                  std::vector<std::pair<syntax::ExpressionPtr, syntax::ExpressionPtr>> ranges,
                  std::vector<syntax::Token> states,
                  std::optional<syntax::Token> base ) override;
  void addRegion( syntax::Token keyword,
                  std::optional<syntax::Token> name,
                  syntax::ExpressionPtr begin,
                  syntax::ExpressionPtr end,
                  syntax::Token property ) override;
  void addRegister( syntax::Token keyword,
                    syntax::Token name,
                    syntax::ExpressionPtr address,
                    syntax::ExpressionPtr width ) override;
  void phaseEntry( syntax::Token phase, std::vector<syntax::Token> label ) override;

  /// What only the whole document settles about the Target: what every
  /// Window shows and which is its base, whose names may be sets declared
  /// later; the units storage is and what one holds; the pools the Regions
  /// leave; and the Module whose Symbols are the named Regions, the unit sets
  /// and the Windows. After resolvePhases, since that Module is present in
  /// every Phase.
  void resolveTarget();
  void addConstantModule();
  void checkContainer() const;

  [[nodiscard]] Project take() &&
  {
    return std::move( mProject );
  }

private:
  /// Where a severity was first set, so the second one can point at it.
  struct SeveritySite
  {
    diag::SourceSpan span;
    std::string code;
  };

  /// A name that may be dotted into a Namespace, joined as the Module will
  /// know it, and where it was written.
  struct DottedText
  {
    std::string name;
    diag::SourceSpan span;
  };

  /// What one `phase` block said — or several with one name, which sum.
  /// `base WINDOW = STATE` as written, in a Phase or a group.
  struct BaseText
  {
    syntax::Token window;
    syntax::Token state;
  };

  struct PhaseText
  {
    syntax::Token name;
    std::vector<syntax::Token> needs;
    std::vector<syntax::Token> then;
    std::optional<DottedText> entry;
    std::vector<BaseText> bases;

    /// What the groups the Phase needs said, gathered once the lists are
    /// resolved, and settled against the Windows once those are.
    std::vector<BaseText> inherited;
  };

  [[nodiscard]] DottedText dottedTextOf( std::vector<syntax::Token> const& path ) const;

  /// The text of a quoted literal, escapes resolved. Nothing when the literal
  /// names a character set, which a path never may: it is a name for a file
  /// rather than a value, so no table translates it — and by the same token it
  /// may hold whatever a filesystem holds.
  [[nodiscard]] std::optional<std::string> pathTextOf( syntax::Token token );

  /// Resolved against the document that named it, and normalised without
  /// touching the filesystem so that the result is the same everywhere.
  [[nodiscard]] std::filesystem::path resolve( std::string const& relative ) const;

  /// The text of the file a document named, and the path it was found at:
  /// beside the document, or in the library. Nothing when neither has it.
  [[nodiscard]] std::optional<std::pair<std::filesystem::path, std::string>>
  readNamed( std::string const& relative ) const;

  /// The text of the Phase with this name, created on first sight: a block
  /// header reaches this for the side effect alone.
  PhaseText& phaseTextOf( syntax::Token name );

  /// What one `group` block or named `modules` block said — or several with
  /// one name, which sum: the Modules it declared and the names it listed.
  /// A group is a name for a list of Modules and nothing below the Project
  /// learns it exists — see docs/decisions/0042-groups.md.
  struct GroupText
  {
    syntax::Token name;
    std::vector<ModuleIndex> modules;
    std::vector<syntax::Token> members;
    std::vector<BaseText> bases;
  };

  /// The bases every group a name reaches gave, a group's own and its
  /// members' through any depth, added to `into`; a cycle has been reported
  /// where the lists were flattened and is not followed here.
  void gatherBases( syntax::Token name, std::vector<BaseText>& into, std::vector<std::size_t>& walking ) const;

  /// The text of the group with this name, created on first sight.
  GroupText& groupTextOf( syntax::Token name );

  /// The Module a name refers to, where one Module is expected — left of the
  /// dot in a `transform` entry — or nothing, which has been reported.
  [[nodiscard]] std::optional<ModuleIndex> moduleNamed( syntax::Token name ) const;

  /// The Modules a name in a list stands for: one, for a Module's name, and
  /// every Module a group holds, flattened, for a group's. Empty for a name
  /// that is neither, which has been reported.
  [[nodiscard]] std::vector<ModuleIndex> modulesNamed( syntax::Token name );

  /// Adds what a name stands for to `into`, flattening a group through the
  /// groups it names the first time it is met. `asking` is the group whose
  /// member the name is, absent for a name in `needs` or `resident`.
  void expandInto( std::vector<ModuleIndex>& into, syntax::Token name, std::optional<std::size_t> asking );

  /// Once the whole document is read: a group's name is a Module's, and a
  /// group that was named and holds nothing.
  void checkGroupNames() const;
  void reportEmptyGroups() const;

  /// The Phase a name refers to, or nothing, which has been reported.
  [[nodiscard]] std::optional<PhaseIndex> phaseNamed( syntax::Token name ) const;

  void reportUnreachablePhases() const;

  /// The number a value written out comes to, or nothing, which has been
  /// reported. No Symbols and no Steps have run, so a value that does not come
  /// to a number here never will.
  [[nodiscard]] std::optional<std::int64_t> valueOf( syntax::Expression const& node ) const;

  void report( diag::Diagnostic value ) const
  {
    mSink->add( std::move( value ) );
  }

  diag::SourceManager* mSources;
  FileReader* mFiles;
  std::filesystem::path mLibrary;
  diag::SeverityPolicy* mPolicy;
  diag::DiagnosticSink* mSink;

  Project mProject;

  /// The documents currently being read, innermost last. Both the cycle check
  /// and the directory a relative path resolves against.
  std::vector<std::filesystem::path> mOpen;

  /// Every token stream stays alive for as long as the load runs: a Token is a
  /// position, and an included document's tokens are read while its parent's
  /// parser is still on the stack.
  std::vector<std::vector<syntax::Token>> mTokens;

  std::unordered_map<std::string, SeveritySite> mSeveritySet;

  /// Where a Project constant was first given a value, so the second one can
  /// point at it.
  std::unordered_map<std::string, diag::SourceSpan> mConstantSet;
  std::unordered_map<std::string, diag::SourceSpan> mModuleNames;

  /// Where each Module was named, parallel to the Project's list: the place a
  /// finding about the Module as a whole points at.
  std::vector<diag::SourceSpan> mModuleSites;
  std::unordered_map<std::string, ModuleIndex> mModuleByName;

  /// In order of first declaration, which is Phase order.
  std::vector<PhaseText> mPhaseTexts;
  std::unordered_map<std::string, std::size_t> mPhaseByName;
  std::vector<syntax::Token> mResidentTexts;

  std::vector<GroupText> mGroupTexts;
  std::unordered_map<std::string, std::size_t> mGroupByName;

  /// How far each group's expansion has got, parallel to mGroupTexts: a
  /// group met again while its own expansion is under way is a cycle.
  enum class Expansion : std::uint8_t
  {
    NOT_YET,
    UNDER_WAY,
    DONE,
  };
  std::vector<Expansion> mExpansion;
  std::vector<std::vector<ModuleIndex>> mExpanded;

  /// Whether some list named the group, directly or through another group:
  /// an empty group nobody names adds nothing to nothing and is not reported.
  std::vector<bool> mGroupNamed;

  /// Whether a cycle cut the group's expansion short, in it or in a group it
  /// names: what it lacks has been reported, so it is not also called empty.
  std::vector<bool> mCutByCycle;

  /// A `transform` entry as written, held until the Modules are named.
  struct TransformText
  {
    syntax::Token transform;
    syntax::Token module;
    DottedText section;
  };

  std::vector<TransformText> mTransformTexts;
  std::optional<syntax::Token> mEntryText;

  std::optional<syntax::Token> mStorageUnitsText;
  std::optional<diag::SourceSpan> mUnitCountSite;
  std::optional<diag::SourceSpan> mUnitSizeSite;

  /// A `window` entry's names as written, held until every unit set is
  /// declared: one per Window of the Target, in the same order.
  struct WindowText
  {
    std::vector<syntax::Token> states;
    std::optional<syntax::Token> base;
  };

  std::vector<WindowText> mWindowTexts;

  /// A Pane as written, held until every Window is declared: its block's
  /// header, and its own name and count.
  struct PaneText
  {
    syntax::Token keyword;
    syntax::Token window;
    std::optional<syntax::Token> state;
    syntax::Token name;
    std::uint32_t count = 1;
  };

  std::vector<PaneText> mPaneTexts;

  /// The `panes` block being read: what every entry of it is under.
  struct PanesHeader
  {
    syntax::Token keyword;
    syntax::Token window;
    std::optional<syntax::Token> state;
  };

  std::optional<PanesHeader> mOpenPanes;

  /// Whether a document has declared a Region: the first one replaces the
  /// stand-in, since a variant that says what memory is says all of it.
  bool mRegionsDeclared = false;

  /// Every name the Target declares — a Region's, a unit set's, a Window's —
  /// and what kind it named, since all three are one name space: each is a
  /// Symbol in scope in every Module.
  struct TargetName
  {
    diag::SourceSpan span;
    std::string_view kind;
  };

  std::unordered_map<std::string, TargetName> mTargetNames;

  /// Takes a name for the Target, reporting the second of one name; false
  /// when it was taken already.
  bool claimTargetName( syntax::Token name, std::string_view kind );

  /// Adds one Region, named or not, once its numbers are known to be a range.
  void declareRegion( syntax::Token keyword,
                      std::optional<syntax::Token> name,
                      AddressRange range,
                      RegionProperty property );
};

std::optional<std::string> Loader::pathTextOf( syntax::Token token )
{
  std::string_view const text = mSources->textOf( token.span() );
  if ( !syntax::quotedOf( text ).charset.empty() )
  {
    report( diag::diagnostic( diag::DiagnosticId::PATH_NAMES_CHARSET ).at( token.location, token.length ) );
    return std::nullopt;
  }

  std::optional<std::vector<std::uint8_t>> const bytes = syntax::plainBytesOf( text );
  if ( !bytes.has_value() )
  {
    // A malformed escape, already reported by the lexer.
    return std::nullopt;
  }
  return std::string{ bytes->begin(), bytes->end() };
}

std::filesystem::path Loader::resolve( std::string const& relative ) const
{
  std::filesystem::path const here{ relative };
  if ( mOpen.empty() )
  {
    return here.lexically_normal();
  }
  return ( mOpen.back().parent_path() / here ).lexically_normal();
}

std::optional<std::pair<std::filesystem::path, std::string>> Loader::readNamed( std::string const& relative ) const
{
  std::filesystem::path const beside = resolve( relative );
  if ( std::optional<std::string> contents = mFiles->read( beside ); contents.has_value() )
  {
    return std::pair{ beside, std::move( *contents ) };
  }
  if ( mLibrary.empty() )
  {
    return std::nullopt;
  }
  std::filesystem::path const shipped = ( mLibrary / std::filesystem::path{ relative } ).lexically_normal();
  if ( std::optional<std::string> contents = mFiles->read( shipped ); contents.has_value() )
  {
    return std::pair{ shipped, std::move( *contents ) };
  }
  return std::nullopt;
}

void Loader::readDocument( std::filesystem::path const& document, std::optional<syntax::Token> from )
{
  // The root is read from where it was given; an included document from
  // beside the one that named it, or from the library.
  std::optional<std::pair<std::filesystem::path, std::string>> found;
  if ( from.has_value() )
  {
    found = readNamed( document.generic_string() );
  }
  else if ( std::optional<std::string> contents = mFiles->read( document ); contents.has_value() )
  {
    found = std::pair{ document, std::move( *contents ) };
  }
  if ( !found.has_value() )
  {
    diag::Diagnostic missing = diag::diagnostic( diag::DiagnosticId::CANNOT_READ_FILE )
                                   .arg( "path", resolve( document.generic_string() ).generic_string() );
    if ( from.has_value() )
    {
      missing = std::move( missing ).at( from->location, from->length );
    }
    else
    {
      missing = std::move( missing ).sortedBy( document.generic_string() );
    }
    report( std::move( missing ) );
    return;
  }

  auto const isOpen = [&found]( std::filesystem::path const& open ) { return open == found->first; };
  if ( std::ranges::any_of( mOpen, isOpen ) )
  {
    diag::Diagnostic cycle = diag::diagnostic( diag::DiagnosticId::INCLUDE_CYCLE );
    if ( from.has_value() )
    {
      cycle = std::move( cycle ).at( from->location, from->length );
    }
    report( std::move( cycle ) );
    return;
  }

  std::filesystem::path const& path = found->first;
  diag::FileId const file = mSources->addFile( path.generic_string(), std::move( found->second ) );
  mTokens.push_back( syntax::tokenize( *mSources, file, *mSink ) );

  mOpen.push_back( path );
  syntax::TokenCursor cursor{ mTokens.back(), true };
  syntax::ProjectParser parser{ *mSources, cursor, *this, *mSink };
  parser.parseDocument();
  mOpen.pop_back();
}

void Loader::addModule( syntax::Token path, std::optional<syntax::Token> alias, std::optional<syntax::Token> group )
{
  std::optional<std::string> const relative = pathTextOf( path );
  if ( !relative.has_value() )
  {
    return;
  }

  std::optional<std::pair<std::filesystem::path, std::string>> found = readNamed( *relative );
  std::filesystem::path const resolved = found.has_value() ? found->first : resolve( *relative );
  std::string name = alias.has_value() ? std::string{ mSources->textOf( alias->span() ) } : resolved.stem().string();
  diag::SourceSpan const site = alias.has_value() ? alias->span() : path.span();

  auto const [existing, added] = mModuleNames.emplace( name, site );
  if ( !added )
  {
    // The message says naming Modules is the Project file's business, and here
    // there is one to say it to: `as` is what the author writes.
    report( diag::diagnostic( diag::DiagnosticId::MODULE_NAME_COLLISION )
                .at( site.begin, site.length )
                .arg( "module", name ) );
    return;
  }

  if ( !found.has_value() )
  {
    report( diag::diagnostic( diag::DiagnosticId::CANNOT_READ_FILE )
                .at( path.location, path.length )
                .arg( "path", resolved.generic_string() ) );
    return;
  }

  diag::FileId const file = mSources->addFile( resolved.generic_string(), std::move( found->second ) );
  ModuleIndex const index{ static_cast<std::uint32_t>( mProject.modules.size() ) };
  mModuleByName.emplace( name, index );
  mModuleSites.push_back( site );
  mProject.modules.push_back(
      ProjectModule{ .name = std::move( name ), .file = file, .residency = {}, .path = *relative } );
  if ( group.has_value() )
  {
    groupTextOf( *group ).modules.push_back( index );
  }
}

std::optional<std::string> Loader::read( std::string const& path, std::string& resolved ) const
{
  std::optional<std::pair<std::filesystem::path, std::string>> found = readNamed( path );
  resolved = ( found.has_value() ? found->first : resolve( path ) ).generic_string();
  if ( !found.has_value() )
  {
    return std::nullopt;
  }
  return std::move( found->second );
}

void Loader::addGeneratedModule( syntax::Token generator,
                                 std::vector<syntax::ProjectBuilder::GeneratorArgument> arguments,
                                 std::optional<syntax::Token> alias,
                                 std::optional<syntax::Token> group )
{
  GeneratorCall call;
  call.generator = std::string{ mSources->textOf( generator.span() ) };
  call.generatorSpan = generator.span();

  // The Module is named by the alias, or by the stem of the file the generator
  // works from, which is the rule a path entry follows.
  std::optional<std::string> file;
  for ( syntax::ProjectBuilder::GeneratorArgument const& argument : arguments )
  {
    model::GeneratorArgument reduced;
    reduced.span = argument.span;
    if ( argument.name.has_value() )
    {
      reduced.name = std::string{ mSources->textOf( argument.name->span() ) };
    }
    if ( argument.literal.has_value() )
    {
      reduced.text = pathTextOf( *argument.literal );
      if ( !reduced.text.has_value() )
      {
        return;
      }
      if ( reduced.name.empty() && !file.has_value() )
      {
        file = reduced.text;
      }
    }
    if ( argument.word.has_value() )
    {
      reduced.word = std::string{ mSources->textOf( argument.word->span() ) };
    }
    if ( argument.value != nullptr )
    {
      reduced.number = valueOf( *argument.value );
      if ( !reduced.number.has_value() )
      {
        return;
      }
    }
    call.arguments.push_back( std::move( reduced ) );
  }

  std::string name = alias.has_value() ? std::string{ mSources->textOf( alias->span() ) }
                                       : std::filesystem::path{ file.value_or( call.generator ) }.stem().string();
  diag::SourceSpan const site = alias.has_value() ? alias->span() : generator.span();
  call.module = name;

  auto const [existing, added] = mModuleNames.emplace( name, site );
  if ( !added )
  {
    report( diag::diagnostic( diag::DiagnosticId::MODULE_NAME_COLLISION )
                .at( site.begin, site.length )
                .arg( "module", name ) );
    return;
  }

  std::optional<GeneratedModule> const description = runGenerator( call, *this, *mSink );
  if ( !description.has_value() )
  {
    return;
  }

  if ( std::optional<std::string> const fault = checkModule( *description ); fault.has_value() )
  {
    report( diag::diagnostic( diag::DiagnosticId::GENERATOR_DESCRIPTION )
                .at( generator.location, generator.length )
                .arg( "generator", call.generator )
                .arg( "where", *fault ) );
    return;
  }

  // A generated Module is a Module: its text is registered where the entry
  // stands, so the diagnostic ordering contract needs no exception, and it is
  // assembled in the same wave as every other.
  diag::FileId const written = mSources->addFile( "<generated>/" + name + ".asm", emitModule( *description ) );
  ModuleIndex const index{ static_cast<std::uint32_t>( mProject.modules.size() ) };
  mModuleByName.emplace( name, index );
  mModuleSites.push_back( site );
  mProject.modules.push_back(
      ProjectModule{ .name = std::move( name ), .file = written, .residency = {}, .fromGenerator = true } );
  if ( group.has_value() )
  {
    groupTextOf( *group ).modules.push_back( index );
  }
}

Loader::GroupText& Loader::groupTextOf( syntax::Token name )
{
  // Two blocks with one name sum, as two `phase` blocks do, so a generated
  // file may add to a group the including file declared.
  auto const [entry, added] =
      mGroupByName.emplace( std::string{ mSources->textOf( name.span() ) }, mGroupTexts.size() );
  if ( added )
  {
    mGroupTexts.push_back( GroupText{ .name = name, .modules = {}, .members = {}, .bases = {} } );
  }
  return mGroupTexts[entry->second];
}

void Loader::declareGroup( syntax::Token name )
{
  groupTextOf( name );
}

void Loader::groupMember( syntax::Token group, syntax::Token member )
{
  groupTextOf( group ).members.push_back( member );
}

void Loader::setSeverity( syntax::Token code, diag::SeverityOverride action )
{
  std::string text{ mSources->textOf( code.span() ) };
  std::optional<diag::DiagnosticId> const id = diag::diagnosticIdForCode( text );
  if ( !id.has_value() )
  {
    // Refused rather than skipped, for the reason a mistyped test marker is: a
    // line that quietly asserted nothing would look like it worked.
    report( diag::diagnostic( diag::DiagnosticId::UNKNOWN_DIAGNOSTIC_CODE )
                .at( code.location, code.length )
                .arg( "code", text ) );
    return;
  }

  auto const [existing, added] = mSeveritySet.emplace( text, SeveritySite{ .span = code.span(), .code = text } );
  if ( !added )
  {
    // Last-one-wins would make a diagnostic's severity depend on the order
    // files were included, which is the quiet order-dependence that summing
    // blocks are otherwise free of.
    report( diag::diagnostic( diag::DiagnosticId::SEVERITY_ALREADY_SET )
                .at( code.location, code.length )
                .arg( "code", text )
                .note( diag::diagnostic( diag::DiagnosticId::PREVIOUS_SEVERITY )
                           .at( existing->second.span.begin, existing->second.span.length )
                           .arg( "code", existing->second.code ) ) );
    return;
  }

  mPolicy->set( *id, action );
}

void Loader::addConstant( syntax::Token name, syntax::Token value )
{
  std::string text{ mSources->textOf( name.span() ) };
  auto const [existing, added] = mConstantSet.emplace( text, name.span() );
  if ( !added )
  {
    // The same refusal as a severity set twice, and for the same reason: a
    // value that depended on which file an `include` reached first would be
    // the one quiet order-dependence summing blocks are otherwise free of —
    // see docs/decisions/0178-a-configuration-is-a-document.md.
    report( diag::diagnostic( diag::DiagnosticId::CONSTANT_ALREADY_SET )
                .at( name.location, name.length )
                .arg( "name", text )
                .note( diag::diagnostic( diag::DiagnosticId::PREVIOUS_CONSTANT )
                           .at( existing->second.begin, existing->second.length )
                           .arg( "name", text ) ) );
    return;
  }

  mProject.constants.push_back( ProjectConstant{ .nameSpan = name.span(), .valueSpan = value.span() } );
}

/// Every container the tool writes, as a finding lists them.
std::string knownContainers()
{
  return fmt::format(
      "`{}`, `{}` and `{}`", nameOf( Container::RAW_IMAGE ), nameOf( Container::XEX ), nameOf( Container::ATR ) );
}

void Loader::setContainer( syntax::Token name )
{
  std::string const text{ mSources->textOf( name.span() ) };
  std::optional<Container> const named = containerNamed( text );
  if ( !named.has_value() )
  {
    report( diag::diagnostic( diag::DiagnosticId::UNKNOWN_CONTAINER )
                .at( name.location, name.length )
                .arg( "name", text )
                .arg( "known", knownContainers() ) );
    return;
  }

  if ( mProject.containerSite.has_value() )
  {
    // The same refusal as an `entry` given twice: what the program is cannot
    // depend on which file an `include` reached first.
    report( diag::diagnostic( diag::DiagnosticId::CONTAINER_ALREADY_SET )
                .at( name.location, name.length )
                .note( diag::diagnostic( diag::DiagnosticId::PREVIOUS_CONTAINER )
                           .at( mProject.containerSite->begin, mProject.containerSite->length ) ) );
    return;
  }

  mProject.container = *named;
  mProject.containerSite = name.span();
}

void Loader::addAcceptedContainer( syntax::Token keyword, syntax::Token name )
{
  std::string const text{ mSources->textOf( name.span() ) };
  std::optional<Container> const named = containerNamed( text );
  if ( !named.has_value() )
  {
    report( diag::diagnostic( diag::DiagnosticId::UNKNOWN_CONTAINER )
                .at( name.location, name.length )
                .arg( "name", text )
                .arg( "known", knownContainers() ) );
    return;
  }

  Target& target = mProject.target;
  if ( target.containers.empty() )
  {
    target.containersSite = keyword.span();
  }
  if ( std::ranges::find( target.containers, *named ) == target.containers.end() )
  {
    target.containers.push_back( *named );
  }
}

void Loader::setCpu( syntax::Token keyword, syntax::Token name )
{
  std::optional<std::string> const quoted = pathTextOf( name );
  if ( !quoted.has_value() )
  {
    return;
  }
  std::string const& text = *quoted;
  std::optional<Cpu> const named = cpuNamed( text );
  if ( !named.has_value() )
  {
    report( diag::diagnostic( diag::DiagnosticId::UNKNOWN_CPU )
                .at( name.location, name.length )
                .arg( "name", text )
                .arg( "known", fmt::format( "`{}` and `{}`", nameOf( Cpu::MOS6502 ), nameOf( Cpu::WDC65SC02 ) ) ) );
    return;
  }

  Target& target = mProject.target;
  if ( target.cpuSite.length != 0 )
  {
    report( diag::diagnostic( diag::DiagnosticId::CPU_ALREADY_SET )
                .at( name.location, name.length )
                .note( diag::diagnostic( diag::DiagnosticId::PREVIOUS_CPU )
                           .at( target.cpuSite.begin, target.cpuSite.length ) ) );
    return;
  }

  target.cpu = *named;
  target.cpuSite = keyword.span();
}

void Loader::setIntent( syntax::Token name )
{
  std::string const text{ mSources->textOf( name.span() ) };
  std::optional<Intent> const named = intentNamed( text );
  if ( !named.has_value() )
  {
    report( diag::diagnostic( diag::DiagnosticId::UNKNOWN_INTENT )
                .at( name.location, name.length )
                .arg( "name", text )
                .arg( "known",
                      fmt::format( "`{}`, `{}` and `{}`",
                                   nameOf( Intent::SPEED ),
                                   nameOf( Intent::SIZE ),
                                   nameOf( Intent::FIT ) ) ) );
    return;
  }

  if ( mProject.intentSite.has_value() )
  {
    report( diag::diagnostic( diag::DiagnosticId::INTENT_ALREADY_SET )
                .at( name.location, name.length )
                .note( diag::diagnostic( diag::DiagnosticId::PREVIOUS_INTENT )
                           .at( mProject.intentSite->begin, mProject.intentSite->length ) ) );
    return;
  }

  mProject.intent = *named;
  mProject.intentSite = name.span();
}

void Loader::checkContainer() const
{
  Target const& target = mProject.target;
  // A Project that named none is the raw image, which nobody chose and no
  // machine refuses; a Target that named none is a stand-in and claims
  // nothing — see docs/decisions/0179-a-container-is-chosen-in-the-project.md.
  if ( !mProject.containerSite.has_value() || target.containers.empty() )
  {
    return;
  }
  if ( std::ranges::find( target.containers, mProject.container ) != target.containers.end() )
  {
    return;
  }

  std::string taken;
  for ( Container const one : target.containers )
  {
    taken += taken.empty() ? "" : ", ";
    taken += fmt::format( "`{}`", nameOf( one ) );
  }
  report( diag::diagnostic( diag::DiagnosticId::CONTAINER_NOT_TAKEN )
              .at( mProject.containerSite->begin, mProject.containerSite->length )
              .arg( "name", std::string{ nameOf( mProject.container ) } )
              .arg( "taken", taken )
              .note( diag::diagnostic( diag::DiagnosticId::CONTAINERS_DECLARED_HERE )
                         .at( target.containersSite.begin, target.containersSite.length ) ) );
}

void Loader::includeDocument( syntax::Token path )
{
  std::optional<std::string> const relative = pathTextOf( path );
  if ( relative.has_value() )
  {
    readDocument( *relative, path );
  }
}

Loader::PhaseText& Loader::phaseTextOf( syntax::Token name )
{
  // Two blocks with one name sum, exactly as two `modules` blocks do: that is
  // how a generated asset list attaches its Modules to a Phase from a file of
  // its own.
  auto const [entry, added] =
      mPhaseByName.emplace( std::string{ mSources->textOf( name.span() ) }, mPhaseTexts.size() );
  if ( added )
  {
    mPhaseTexts.push_back(
        PhaseText{ .name = name, .needs = {}, .then = {}, .entry = std::nullopt, .bases = {}, .inherited = {} } );
  }
  return mPhaseTexts[entry->second];
}

void Loader::declarePhase( syntax::Token name )
{
  phaseTextOf( name );
}

void Loader::phaseNeeds( syntax::Token phase, syntax::Token module )
{
  phaseTextOf( phase ).needs.push_back( module );
}

void Loader::phaseLeadsTo( syntax::Token phase, syntax::Token next )
{
  phaseTextOf( phase ).then.push_back( next );
}

void Loader::phaseBase( syntax::Token phase, syntax::Token window, syntax::Token state )
{
  phaseTextOf( phase ).bases.push_back( BaseText{ .window = window, .state = state } );
}

void Loader::groupBase( syntax::Token group, syntax::Token window, syntax::Token state )
{
  groupTextOf( group ).bases.push_back( BaseText{ .window = window, .state = state } );
}

void Loader::gatherBases( syntax::Token name, std::vector<BaseText>& into, std::vector<std::size_t>& walking ) const
{
  auto const group = mGroupByName.find( std::string{ mSources->textOf( name.span() ) } );
  if ( group == mGroupByName.end() || std::ranges::find( walking, group->second ) != walking.end() )
  {
    return;
  }
  walking.push_back( group->second );
  GroupText const& text = mGroupTexts[group->second];
  into.insert( into.end(), text.bases.begin(), text.bases.end() );
  for ( syntax::Token const member : text.members )
  {
    gatherBases( member, into, walking );
  }
  walking.pop_back();
}

Loader::DottedText Loader::dottedTextOf( std::vector<syntax::Token> const& path ) const
{
  DottedText text;
  for ( syntax::Token const& segment : path )
  {
    if ( !text.name.empty() )
    {
      text.name += '.';
    }
    text.name += mSources->textOf( segment.span() );
  }
  text.span = syntax::spanning( path.front().span(), path.back().span() );
  return text;
}

void Loader::applyTransform( syntax::Token transform, syntax::Token module, std::vector<syntax::Token> section )
{
  // The Module is resolvable now and the Section is not, so only the second
  // name travels as text — see TransformRequest.
  mTransformTexts.push_back(
      TransformText{ .transform = transform, .module = module, .section = dottedTextOf( section ) } );
}

void Loader::addResident( syntax::Token module )
{
  mResidentTexts.push_back( module );
}

void Loader::setEntry( syntax::Token phase )
{
  if ( mEntryText.has_value() )
  {
    // A value and not a set, so a second one is a contradiction rather than a
    // repetition — the same rule as setting one severity twice.
    report( diag::diagnostic( diag::DiagnosticId::ENTRY_ALREADY_SET )
                .at( phase.location, phase.length )
                .note( diag::diagnostic( diag::DiagnosticId::PREVIOUS_ENTRY )
                           .at( mEntryText->location, mEntryText->length ) ) );
    return;
  }
  mEntryText = phase;
}

std::optional<std::int64_t> Loader::valueOf( syntax::Expression const& node ) const
{
  // The grammar has refused every name, so nothing here looks a Symbol up in
  // a Module this Project does not have; what is left is arithmetic over what
  // was written.
  GlobalSymbols const none{ std::span<Module const>{} };
  std::optional<std::int64_t> const value =
      evaluate( *mSources, none, nullptr, ModuleIndex{ 0 }, node, nothingIsKnown() );
  if ( !value.has_value() )
  {
    report( diag::diagnostic( diag::DiagnosticId::VALUE_HAS_NO_NUMBER ).at( node.span.begin, node.span.length ) );
  }
  return value;
}

bool Loader::claimTargetName( syntax::Token name, std::string_view kind )
{
  std::string text{ mSources->textOf( name.span() ) };
  auto const [existing, added] = mTargetNames.emplace( text, TargetName{ .span = name.span(), .kind = kind } );
  if ( added )
  {
    return true;
  }
  // A name is a Symbol every Module sees, so two of them is the collision
  // Merge would report, caught where both are declared.
  bool const regions = existing->second.kind == "region" && kind == "region";
  report( diag::diagnostic( regions ? diag::DiagnosticId::REGION_ALREADY_NAMED : diag::DiagnosticId::TARGET_NAME_TAKEN )
              .at( name.location, name.length )
              .arg( "name", text )
              .arg( "kind", std::string{ existing->second.kind } )
              .note( diag::diagnostic( regions ? diag::DiagnosticId::PREVIOUS_REGION
                                               : diag::DiagnosticId::PREVIOUS_TARGET_NAME )
                         .at( existing->second.span.begin, existing->second.span.length )
                         .arg( "name", text ) ) );
  return false;
}

void Loader::setStorageUnits( syntax::Token keyword, syntax::Token name )
{
  if ( mUnitCountSite.has_value() || mStorageUnitsText.has_value() )
  {
    diag::SourceSpan const previous = mUnitCountSite.has_value() ? *mUnitCountSite : mStorageUnitsText->span();
    report( diag::diagnostic( diag::DiagnosticId::STORAGE_UNITS_TWICE )
                .at( keyword.location, keyword.length )
                .note( diag::diagnostic( diag::DiagnosticId::PREVIOUS_STORAGE_UNITS )
                           .at( previous.begin, previous.length ) ) );
    return;
  }
  // Which set, and what a unit holds, are settled once the whole document
  // is read, since the set may be declared after this or in a file included
  // later.
  mStorageUnitsText = name;
}

void Loader::addUnitSet( syntax::Token keyword, syntax::Token name, syntax::ExpressionPtr count )
{
  std::optional<std::int64_t> const value = valueOf( *count );
  if ( !value.has_value() )
  {
    return;
  }
  if ( *value < 1 )
  {
    report( diag::diagnostic( diag::DiagnosticId::UNIT_SET_EMPTY ).at( count->span.begin, count->span.length ) );
    return;
  }
  if ( *value > MOST_UNITS )
  {
    report( diag::diagnostic( diag::DiagnosticId::TOO_MANY_UNITS )
                .at( count->span.begin, count->span.length )
                .arg( "count", *value ) );
    return;
  }
  if ( !claimTargetName( name, "unit set" ) )
  {
    return;
  }
  mProject.target.unitSets.push_back( UnitSet{ .nameSpan = name.span(),
                                               .name = std::string{ mSources->textOf( name.span() ) },
                                               .count = static_cast<std::uint32_t>( *value ),
                                               .site = keyword.span() } );
}

void Loader::beginPanes( syntax::Token keyword, syntax::Token window, std::optional<syntax::Token> state )
{
  mOpenPanes = PanesHeader{ .keyword = keyword, .window = window, .state = state };
}

void Loader::addPane( syntax::Token name, syntax::ExpressionPtr count )
{
  std::uint32_t members = 1;
  if ( count != nullptr )
  {
    std::optional<std::int64_t> const value = valueOf( *count );
    if ( !value.has_value() )
    {
      return;
    }
    if ( *value < 1 || *value > MOST_PANE_MEMBERS )
    {
      report( diag::diagnostic( diag::DiagnosticId::PANE_FAMILY_COUNT )
                  .at( count->span.begin, count->span.length )
                  .arg( "count", *value ) );
      return;
    }
    members = static_cast<std::uint32_t>( *value );
  }
  if ( !mOpenPanes.has_value() )
  {
    return;
  }
  PanesHeader const header = *mOpenPanes;
  if ( !claimTargetName( name, "pane" ) )
  {
    return;
  }
  // Which Window, and which of its states, is settled once the whole
  // document is read, since the Window may be declared later or in a file
  // included later.
  mPaneTexts.push_back( PaneText{
      .keyword = header.keyword, .window = header.window, .state = header.state, .name = name, .count = members } );
}

void Loader::addWindow( syntax::Token keyword,
                        syntax::Token name,
                        std::vector<std::pair<syntax::ExpressionPtr, syntax::ExpressionPtr>> ranges,
                        std::vector<syntax::Token> states,
                        std::optional<syntax::Token> base )
{
  // Every range is a range, or the entry is one finding at the keyword: a
  // Window whose ranges are half right is not one the solver can use.
  std::vector<AddressRange> evaluated;
  for ( auto const& [begin, end] : ranges )
  {
    std::optional<std::int64_t> const low = valueOf( *begin );
    std::optional<std::int64_t> const high = valueOf( *end );
    if ( !low.has_value() || !high.has_value() )
    {
      return;
    }
    if ( *low < 0 || *high < *low || std::cmp_greater_equal( *high, ADDRESS_SPACE_END ) )
    {
      report( diag::diagnostic( diag::DiagnosticId::WINDOW_NOT_A_RANGE ).at( keyword.location, keyword.length ) );
      return;
    }
    evaluated.push_back(
        AddressRange{ .begin = static_cast<std::uint32_t>( *low ), .end = static_cast<std::uint32_t>( *high + 1 ) } );
  }
  if ( !claimTargetName( name, "window" ) )
  {
    return;
  }
  mProject.target.windows.push_back( Window{ .nameSpan = name.span(),
                                             .name = std::string{ mSources->textOf( name.span() ) },
                                             .ranges = std::move( evaluated ),
                                             .states = {},
                                             .base = std::nullopt,
                                             .site = keyword.span() } );
  mWindowTexts.push_back( WindowText{ .states = std::move( states ), .base = base } );
}

void Loader::declareRegion( syntax::Token keyword,
                            std::optional<syntax::Token> name,
                            AddressRange range,
                            RegionProperty property )
{
  if ( !mRegionsDeclared )
  {
    mProject.target.regions.clear();
    mRegionsDeclared = true;
  }

  Region region{ .nameSpan = std::nullopt, .name = {}, .range = range, .property = property, .site = keyword.span() };
  if ( name.has_value() )
  {
    if ( !claimTargetName( *name, "region" ) )
    {
      return;
    }
    region.nameSpan = name->span();
    region.name = std::string{ mSources->textOf( name->span() ) };
  }
  mProject.target.regions.push_back( std::move( region ) );
}

void Loader::addRegion( syntax::Token keyword,
                        std::optional<syntax::Token> name,
                        syntax::ExpressionPtr begin,
                        syntax::ExpressionPtr end,
                        syntax::Token property )
{
  std::string_view const word = mSources->textOf( property.span() );
  std::optional<RegionProperty> kind;
  if ( word == "ram" )
  {
    kind = RegionProperty::RAM;
  }
  else if ( word == "register" )
  {
    kind = RegionProperty::REGISTER;
  }
  else if ( word == "reserved" )
  {
    kind = RegionProperty::RESERVED;
  }
  if ( !kind.has_value() )
  {
    report( diag::diagnostic( diag::DiagnosticId::UNKNOWN_REGION_PROPERTY )
                .at( property.location, property.length )
                .arg( "word", std::string{ word } ) );
    return;
  }

  std::optional<std::int64_t> const first = valueOf( *begin );
  std::optional<std::int64_t> const last = valueOf( *end );
  if ( !first.has_value() || !last.has_value() )
  {
    return;
  }
  // Both ends inclusive, as the window's are: `$D000 .. $D7FF` is the page
  // it has always meant.
  if ( *first < 0 || *last < *first || std::cmp_greater_equal( *last, ADDRESS_SPACE_END ) )
  {
    report( diag::diagnostic( diag::DiagnosticId::REGION_NOT_A_RANGE ).at( keyword.location, keyword.length ) );
    return;
  }
  declareRegion(
      keyword,
      name,
      AddressRange{ .begin = static_cast<std::uint32_t>( *first ), .end = static_cast<std::uint32_t>( *last + 1 ) },
      *kind );
}

void Loader::addRegister( syntax::Token keyword,
                          syntax::Token name,
                          syntax::ExpressionPtr address,
                          syntax::ExpressionPtr width )
{
  std::optional<std::int64_t> const at = valueOf( *address );
  if ( !at.has_value() )
  {
    return;
  }
  std::int64_t bytes = 1;
  if ( width != nullptr )
  {
    std::optional<std::int64_t> const given = valueOf( *width );
    if ( !given.has_value() )
    {
      return;
    }
    if ( *given != 1 && *given != 2 )
    {
      report( diag::diagnostic( diag::DiagnosticId::REGISTER_WIDTH )
                  .at( width->span.begin, width->span.length )
                  .arg( "width", *given ) );
      return;
    }
    bytes = *given;
  }
  if ( *at < 0 || std::cmp_greater( *at + bytes, ADDRESS_SPACE_END ) )
  {
    report( diag::diagnostic( diag::DiagnosticId::REGISTER_NOT_AN_ADDRESS )
                .at( address->span.begin, address->span.length )
                .arg( "value", *at ) );
    return;
  }
  declareRegion(
      keyword,
      name,
      AddressRange{ .begin = static_cast<std::uint32_t>( *at ), .end = static_cast<std::uint32_t>( *at + bytes ) },
      RegionProperty::REGISTER );
}

void Loader::setUnitCount( syntax::Token keyword, syntax::ExpressionPtr value )
{
  std::optional<std::int64_t> const count = valueOf( *value );
  if ( !count.has_value() )
  {
    return;
  }
  if ( mUnitCountSite.has_value() || mStorageUnitsText.has_value() )
  {
    diag::SourceSpan const previous = mUnitCountSite.has_value() ? *mUnitCountSite : mStorageUnitsText->span();
    report( diag::diagnostic( diag::DiagnosticId::STORAGE_UNITS_TWICE )
                .at( keyword.location, keyword.length )
                .note( diag::diagnostic( diag::DiagnosticId::PREVIOUS_STORAGE_UNITS )
                           .at( previous.begin, previous.length ) ) );
    return;
  }
  if ( *count < 0 || *count > MOST_UNITS )
  {
    report( diag::diagnostic( diag::DiagnosticId::TOO_MANY_UNITS )
                .at( value->span.begin, value->span.length )
                .arg( "count", *count ) );
    return;
  }
  mUnitCountSite = keyword.span();
  // A unit known by its number: the value the driver sees is the index.
  mProject.target.unitCount = static_cast<std::uint32_t>( *count );
}

void Loader::setUnitSize( syntax::Token keyword, syntax::ExpressionPtr value )
{
  std::optional<std::int64_t> const size = valueOf( *value );
  if ( !size.has_value() )
  {
    return;
  }
  if ( mUnitSizeSite.has_value() )
  {
    report( diag::diagnostic( diag::DiagnosticId::STORAGE_SIZE_TWICE )
                .at( keyword.location, keyword.length )
                .note( diag::diagnostic( diag::DiagnosticId::PREVIOUS_STORAGE_SIZE )
                           .at( mUnitSizeSite->begin, mUnitSizeSite->length ) ) );
    return;
  }
  mUnitSizeSite = keyword.span();
  if ( *size < 1 || std::cmp_greater( *size, ADDRESS_SPACE_END ) )
  {
    // Set, and wrong: one finding, not one and "missing" besides.
    report( diag::diagnostic( diag::DiagnosticId::UNIT_SIZE_NOT_A_SIZE )
                .at( value->span.begin, value->span.length )
                .arg( "value", *size ) );
    mProject.target.unitSize = 1;
    return;
  }
  mProject.target.unitSize = static_cast<std::uint32_t>( *size );
}

void Loader::phaseEntry( syntax::Token phase, std::vector<syntax::Token> label )
{
  PhaseText& text = phaseTextOf( phase );
  DottedText given = dottedTextOf( label );
  if ( text.entry.has_value() )
  {
    // A value and not a set, like `entry` at document level: the second is a
    // contradiction, whichever block it stood in.
    report( diag::diagnostic( diag::DiagnosticId::PHASE_ENTRY_ALREADY_SET )
                .at( given.span.begin, given.span.length )
                .arg( "phase", std::string{ mSources->textOf( phase.span() ) } )
                .note( diag::diagnostic( diag::DiagnosticId::PREVIOUS_PHASE_ENTRY )
                           .at( text.entry->span.begin, text.entry->span.length ) ) );
    return;
  }
  text.entry = std::move( given );
}

void Loader::resolveTarget()
{
  // Settled once the whole document is read, since a set may be declared
  // after the Window that shows it or the storage that is it, and the size
  // after the units, in a file included later.
  Target& target = mProject.target;

  for ( std::uint32_t index = 0; index < target.windows.size(); ++index )
  {
    Window& window = target.windows[index];
    WindowText const& text = mWindowTexts[index];
    for ( syntax::Token const state : text.states )
    {
      std::string_view const name = mSources->textOf( state.span() );
      if ( std::ranges::any_of( window.states, [name]( WindowState const& one ) { return one.name == name; } ) )
      {
        report( diag::diagnostic( diag::DiagnosticId::WINDOW_STATE_REPEATED )
                    .at( state.location, state.length )
                    .arg( "state", std::string{ name } )
                    .arg( "window", window.name ) );
        continue;
      }
      // A name that is a unit set contributes its Banks; any other is a
      // state of the hardware's own, named here and nowhere else.
      window.states.push_back(
          WindowState{ .nameSpan = state.span(), .name = std::string{ name }, .units = target.unitSetNamed( name ) } );
    }
    if ( text.base.has_value() )
    {
      std::string_view const name = mSources->textOf( text.base->span() );
      for ( std::uint32_t state = 0; state < window.states.size(); ++state )
      {
        if ( window.states[state].name == name && !window.states[state].units.has_value() )
        {
          window.base = state;
        }
      }
      if ( !window.base.has_value() )
      {
        report( diag::diagnostic( diag::DiagnosticId::WINDOW_BASE_UNKNOWN )
                    .at( text.base->location, text.base->length )
                    .arg( "base", std::string{ name } )
                    .arg( "window", window.name ) );
      }
    }
  }

  for ( PaneText const& text : mPaneTexts )
  {
    std::string_view const windowName = mSources->textOf( text.window.span() );
    std::optional<WindowIndex> const window = target.windowNamed( windowName );
    if ( !window.has_value() )
    {
      report( diag::diagnostic( diag::DiagnosticId::PANES_WINDOW_UNKNOWN )
                  .at( text.window.location, text.window.length )
                  .arg( "name", std::string{ windowName } ) );
      continue;
    }
    Window const& shown = target.windows[window->value];
    Pane pane{ .nameSpan = text.name.span(),
               .name = std::string{ mSources->textOf( text.name.span() ) },
               .window = *window,
               .state = std::nullopt,
               .count = text.count,
               .site = text.keyword.span() };
    if ( text.state.has_value() )
    {
      // Pinned to a named state, which the solver then has no say in; a
      // family is a run of Banks, so one state cannot hold it.
      std::string_view const stateName = mSources->textOf( text.state->span() );
      std::uint32_t index = 0;
      for ( WindowState const& state : shown.states )
      {
        if ( state.name == stateName && !state.units.has_value() )
        {
          pane.state = index;
        }
        index += state.units.has_value() ? target.unitSets[state.units->value].count : 1;
      }
      if ( !pane.state.has_value() )
      {
        report( diag::diagnostic( diag::DiagnosticId::PANES_STATE_UNKNOWN )
                    .at( text.state->location, text.state->length )
                    .arg( "state", std::string{ stateName } )
                    .arg( "window", shown.name ) );
        continue;
      }
      if ( pane.count > 1 )
      {
        report( diag::diagnostic( diag::DiagnosticId::PANE_FAMILY_PINNED )
                    .at( text.name.location, text.name.length )
                    .arg( "pane", pane.name ) );
        continue;
      }
    }
    else if ( !target.unitSetShownBy( *window ).has_value() )
    {
      report( diag::diagnostic( diag::DiagnosticId::PANES_WINDOW_HAS_NO_UNITS )
                  .at( text.keyword.location, text.keyword.length )
                  .arg( "window", shown.name ) );
      continue;
    }
    target.panes.push_back( std::move( pane ) );
  }

  // Every Phase's bases: what it said, else what the groups it needs agree
  // on, else the variant's — a fact of the Phase once names are resolved, so
  // no Step asks where it came from.
  for ( std::size_t index = 0; index < mPhaseTexts.size(); ++index )
  {
    PhaseText const& text = mPhaseTexts[index];
    Phase& phase = mProject.phases.phases[index];
    phase.bases.assign( target.windows.size(), std::nullopt );
    std::string const phaseName{ mSources->textOf( text.name.span() ) };
    auto const resolve = [&]( BaseText const& base ) -> std::optional<std::pair<WindowIndex, std::uint32_t>>
    {
      std::string_view const windowName = mSources->textOf( base.window.span() );
      std::optional<WindowIndex> const window = target.windowNamed( windowName );
      if ( !window.has_value() )
      {
        report( diag::diagnostic( diag::DiagnosticId::BASE_WINDOW_UNKNOWN )
                    .at( base.window.location, base.window.length )
                    .arg( "name", std::string{ windowName } ) );
        return std::nullopt;
      }
      std::string_view const stateName = mSources->textOf( base.state.span() );
      std::optional<std::uint32_t> const state =
          target.windows[window->value].namedStateOf( stateName, target.unitSets );
      if ( !state.has_value() )
      {
        report( diag::diagnostic( diag::DiagnosticId::BASE_STATE_UNKNOWN )
                    .at( base.state.location, base.state.length )
                    .arg( "state", std::string{ stateName } )
                    .arg( "window", target.windows[window->value].name ) );
        return std::nullopt;
      }
      return std::pair{ *window, *state };
    };
    // The Phase's own word first, and once per Window.
    for ( BaseText const& base : text.bases )
    {
      std::optional<std::pair<WindowIndex, std::uint32_t>> const found = resolve( base );
      if ( !found.has_value() )
      {
        continue;
      }
      if ( phase.bases[found->first.value].has_value() )
      {
        report( diag::diagnostic( diag::DiagnosticId::BASE_TWICE )
                    .at( base.window.location, base.window.length )
                    .arg( "phase", phaseName )
                    .arg( "window", target.windows[found->first.value].name ) );
        continue;
      }
      phase.bases[found->first.value] = found->second;
    }
    // Then the groups', which agree or are refused; the Phase's own word
    // outranks them where it was given.
    std::vector<std::optional<std::pair<std::uint32_t, syntax::Token>>> fromGroups( target.windows.size() );
    for ( BaseText const& base : text.inherited )
    {
      std::optional<std::pair<WindowIndex, std::uint32_t>> const found = resolve( base );
      if ( !found.has_value() )
      {
        continue;
      }
      std::optional<std::pair<std::uint32_t, syntax::Token>>& earlier = fromGroups[found->first.value];
      if ( earlier.has_value() && earlier->first != found->second )
      {
        Window const& window = target.windows[found->first.value];
        report( diag::diagnostic( diag::DiagnosticId::BASES_DISAGREE )
                    .at( base.state.location, base.state.length )
                    .arg( "phase", phaseName )
                    .arg( "window", window.name )
                    .arg( "state", std::string{ mSources->textOf( base.state.span() ) } )
                    .arg( "other", std::string{ mSources->textOf( earlier->second.span() ) } )
                    .note( diag::diagnostic( diag::DiagnosticId::BASE_GIVEN_HERE )
                               .at( earlier->second.location, earlier->second.length )
                               .arg( "state", std::string{ mSources->textOf( earlier->second.span() ) } ) ) );
        continue;
      }
      earlier = std::pair{ found->second, base.state };
    }
    for ( std::uint32_t window = 0; window < target.windows.size(); ++window )
    {
      if ( !phase.bases[window].has_value() && fromGroups[window].has_value() )
      {
        phase.bases[window] = fromGroups[window]->first;
      }
    }
  }

  if ( mStorageUnitsText.has_value() )
  {
    std::string_view const name = mSources->textOf( mStorageUnitsText->span() );
    std::optional<UnitSetIndex> const set = target.unitSetNamed( name );
    if ( !set.has_value() )
    {
      report( diag::diagnostic( diag::DiagnosticId::STORAGE_UNITS_UNKNOWN )
                  .at( mStorageUnitsText->location, mStorageUnitsText->length )
                  .arg( "name", std::string{ name } ) );
    }
    else
    {
      // What a unit holds is what the Window showing the set holds; two
      // Windows showing it agree, or a unit has no one size.
      target.storageUnits = set;
      target.unitCount = target.unitSets[set->value].count;
      std::optional<std::uint32_t> shownBy;
      for ( std::uint32_t index = 0; index < target.windows.size(); ++index )
      {
        Window const& window = target.windows[index];
        if ( !window.firstStateOf( *set, target.unitSets ).has_value() )
        {
          continue;
        }
        if ( shownBy.has_value() && target.windows[*shownBy].size() != window.size() )
        {
          report( diag::diagnostic( diag::DiagnosticId::WINDOW_SIZES_DIFFER )
                      .at( window.site.begin, window.site.length )
                      .arg( "window", window.name )
                      .arg( "other", target.windows[*shownBy].name )
                      .arg( "name", std::string{ name } ) );
          continue;
        }
        if ( !shownBy.has_value() )
        {
          shownBy = index;
          target.unitSize = window.size();
        }
      }
      if ( !shownBy.has_value() )
      {
        report( diag::diagnostic( diag::DiagnosticId::STORAGE_UNITS_NOT_SHOWN )
                    .at( mStorageUnitsText->location, mStorageUnitsText->length )
                    .arg( "name", std::string{ name } ) );
      }
      if ( mUnitSizeSite.has_value() )
      {
        report( diag::diagnostic( diag::DiagnosticId::STORAGE_SIZE_DERIVED )
                    .at( mUnitSizeSite->begin, mUnitSizeSite->length ) );
      }
    }
  }
  else if ( target.unitCount != 0 && target.unitSize == 0 )
  {
    diag::SourceSpan const site = mUnitCountSite.value_or( diag::SourceSpan{} );
    report( diag::diagnostic( diag::DiagnosticId::STORAGE_SIZE_MISSING ).at( site.begin, site.length ) );
  }

  target.pools = poolsOf( target.regions );

  bool const anyNamed =
      std::ranges::any_of( target.regions, []( Region const& region ) { return region.nameSpan.has_value(); } ) ||
      !target.unitSets.empty() || !target.windows.empty() || !target.panes.empty();
  if ( !anyNamed )
  {
    return;
  }

  // The named Regions, the unit sets and the Windows reach every Module as
  // the exports of a Module the tool added, present in every Phase and
  // holding no Section, so that Merge and every Step after it see one kind
  // of name — see docs/decisions/0038-regions.md.
  diag::FileId const file = mSources->addFile( "<nga>/regions", std::string{} );
  ModuleIndex const index{ static_cast<std::uint32_t>( mProject.modules.size() ) };
  mProject.modules.push_back( ProjectModule{
      .name = "nga.regions", .file = file, .residency = {}, .generated = Generated::REGIONS, .outsideWindow = false } );
  mProject.regionModule = index;
  for ( Phase& phase : mProject.phases.phases )
  {
    phase.needs.push_back( index );
  }
  deriveResidency( mProject );
}

void Loader::addConstantModule()
{
  if ( mProject.constants.empty() )
  {
    return;
  }

  // The same road the named Regions take: a name the Project declares reaches
  // every Module as the export of a Module the tool added, so Merge and every
  // Step after it see one kind of name and none of them learns that a name
  // can come from a document — see
  // docs/decisions/0178-a-configuration-is-a-document.md.
  diag::FileId const file = mSources->addFile( "<nga>/constants", std::string{} );
  ModuleIndex const index{ static_cast<std::uint32_t>( mProject.modules.size() ) };
  mProject.modules.push_back( ProjectModule{ .name = "nga.constants",
                                             .file = file,
                                             .residency = {},
                                             .generated = Generated::CONSTANTS,
                                             .outsideWindow = false } );
  mProject.constantModule = index;
  for ( Phase& phase : mProject.phases.phases )
  {
    phase.needs.push_back( index );
  }
  deriveResidency( mProject );
}

std::optional<ModuleIndex> Loader::moduleNamed( syntax::Token name ) const
{
  std::string const text{ mSources->textOf( name.span() ) };
  auto const found = mModuleByName.find( text );
  if ( found == mModuleByName.end() )
  {
    // A group stands for its Modules where a list of them is expected, and
    // here one is expected: `group.section` names nothing.
    if ( mGroupByName.contains( text ) )
    {
      report( diag::diagnostic( diag::DiagnosticId::GROUP_IS_NOT_A_MODULE )
                  .at( name.location, name.length )
                  .arg( "name", text ) );
      return std::nullopt;
    }
    report(
        diag::diagnostic( diag::DiagnosticId::UNKNOWN_MODULE ).at( name.location, name.length ).arg( "module", text ) );
    return std::nullopt;
  }
  return found->second;
}

std::vector<ModuleIndex> Loader::modulesNamed( syntax::Token name )
{
  std::vector<ModuleIndex> found;
  expandInto( found, name, std::nullopt );
  return found;
}

void Loader::expandInto( std::vector<ModuleIndex>& into, syntax::Token name, std::optional<std::size_t> asking )
{
  std::string const text{ mSources->textOf( name.span() ) };
  if ( auto const module = mModuleByName.find( text ); module != mModuleByName.end() )
  {
    // A name that is both is reported once, where the group is declared, and
    // means the Module everywhere it is used.
    addOnce( into, module->second );
    return;
  }

  auto const found = mGroupByName.find( text );
  if ( found == mGroupByName.end() )
  {
    report(
        diag::diagnostic( diag::DiagnosticId::UNKNOWN_MODULE ).at( name.location, name.length ).arg( "module", text ) );
    return;
  }

  std::size_t const group = found->second;
  mGroupNamed[group] = true;
  if ( mExpansion[group] == Expansion::UNDER_WAY )
  {
    // The group is being flattened further up the stack and this name would
    // ask for it again: reported at the name that closes the cycle. The group
    // that asked is left with what it gathered without it, and is not then
    // called empty, since the error already says what it lacks.
    report( diag::diagnostic( diag::DiagnosticId::GROUP_CYCLE ).at( name.location, name.length ).arg( "group", text ) );
    if ( asking.has_value() )
    {
      mCutByCycle[*asking] = true;
    }
    return;
  }

  if ( mExpansion[group] == Expansion::NOT_YET )
  {
    mExpansion[group] = Expansion::UNDER_WAY;
    std::vector<ModuleIndex> members = mGroupTexts[group].modules;
    for ( syntax::Token const member : mGroupTexts[group].members )
    {
      expandInto( members, member, group );
    }
    mExpanded[group] = std::move( members );
    mExpansion[group] = Expansion::DONE;
  }

  for ( ModuleIndex const module : mExpanded[group] )
  {
    addOnce( into, module );
  }
  if ( asking.has_value() && mCutByCycle[group] )
  {
    mCutByCycle[*asking] = true;
  }
}

void Loader::checkGroupNames() const
{
  for ( GroupText const& group : mGroupTexts )
  {
    std::string const text{ mSources->textOf( group.name.span() ) };
    auto const module = mModuleNames.find( text );
    if ( module == mModuleNames.end() )
    {
      continue;
    }
    // Both stand in the same lists, so one name for both would mean two
    // things in `needs`. Reported at the group, since the Module's name came
    // from a file and the group's was chosen.
    report( diag::diagnostic( diag::DiagnosticId::GROUP_NAME_COLLISION )
                .at( group.name.location, group.name.length )
                .arg( "name", text )
                .note( diag::diagnostic( diag::DiagnosticId::MODULE_DECLARED_HERE )
                           .at( module->second.begin, module->second.length )
                           .arg( "name", text ) ) );
  }
}

void Loader::reportEmptyGroups() const
{
  for ( std::size_t group = 0; group < mGroupTexts.size(); ++group )
  {
    if ( !mGroupNamed[group] || mCutByCycle[group] || mExpansion[group] != Expansion::DONE ||
         !mExpanded[group].empty() )
    {
      continue;
    }
    // A list that named it expected Modules to arrive and none did: said
    // once, at the group, for the reason a `.off` that silenced nothing is.
    syntax::Token const name = mGroupTexts[group].name;
    report( diag::diagnostic( diag::DiagnosticId::EMPTY_GROUP )
                .at( name.location, name.length )
                .arg( "group", std::string{ mSources->textOf( name.span() ) } ) );
  }
}

std::optional<PhaseIndex> Loader::phaseNamed( syntax::Token name ) const
{
  std::string const text{ mSources->textOf( name.span() ) };
  auto const found = mPhaseByName.find( text );
  if ( found == mPhaseByName.end() )
  {
    report(
        diag::diagnostic( diag::DiagnosticId::UNKNOWN_PHASE ).at( name.location, name.length ).arg( "phase", text ) );
    return std::nullopt;
  }
  return PhaseIndex{ static_cast<std::uint32_t>( found->second ) };
}

void Loader::reportUnreachablePhases() const
{
  PhaseGraph const& graph = mProject.phases;
  std::vector<bool> reached( graph.phases.size(), false );
  std::vector<PhaseIndex> pending{ graph.entry };
  reached[graph.entry.value] = true;

  while ( !pending.empty() )
  {
    PhaseIndex const here = pending.back();
    pending.pop_back();
    for ( PhaseIndex const next : graph.phases[here.value].then )
    {
      if ( !reached[next.value] )
      {
        reached[next.value] = true;
        pending.push_back( next );
      }
    }
  }

  for ( std::size_t phase = 0; phase < graph.phases.size(); ++phase )
  {
    if ( !reached[phase] )
    {
      syntax::Token const name = mPhaseTexts[phase].name;
      report( diag::diagnostic( diag::DiagnosticId::UNREACHABLE_PHASE )
                  .at( name.location, name.length )
                  .arg( "phase", std::string{ mSources->textOf( name.span() ) } ) );
    }
  }
}

void Loader::resolvePhases( std::filesystem::path const& root )
{
  PhaseGraph& graph = mProject.phases;
  for ( PhaseText const& text : mPhaseTexts )
  {
    std::optional<std::string> entry;
    if ( text.entry.has_value() )
    {
      entry = text.entry->name;
    }
    graph.phases.push_back( Phase{ .name = std::string{ mSources->textOf( text.name.span() ) },
                                   .needs = {},
                                   .then = {},
                                   .entry = std::move( entry ),
                                   .bases = {} } );
  }

  // Nothing declared a Phase: one, unnamed, with every Module in it. That is
  // the synthesised Project written in the grammar, and it needs no `entry`.
  bool const implicit = mPhaseTexts.empty();
  if ( implicit )
  {
    Phase everything;
    for ( std::uint32_t module = 0; module < mProject.modules.size(); ++module )
    {
      everything.needs.push_back( ModuleIndex{ module } );
    }
    graph.phases.push_back( std::move( everything ) );
  }

  // A group is flattened the first time a list names it, and every group
  // exists by now, so a name in a group may be declared after the group.
  mExpansion.assign( mGroupTexts.size(), Expansion::NOT_YET );
  mExpanded.assign( mGroupTexts.size(), {} );
  mGroupNamed.assign( mGroupTexts.size(), false );
  mCutByCycle.assign( mGroupTexts.size(), false );
  checkGroupNames();

  for ( std::size_t index = 0; index < mPhaseTexts.size(); ++index )
  {
    Phase& phase = graph.phases[index];
    for ( syntax::Token const name : mPhaseTexts[index].needs )
    {
      for ( ModuleIndex const module : modulesNamed( name ) )
      {
        addOnce( phase.needs, module );
      }
      // A group's base, and its members' groups', reaches every Phase that
      // needs the group — see docs/decisions/0056-a-phase-chooses-a-base.md.
      std::vector<std::size_t> walking;
      gatherBases( name, mPhaseTexts[index].inherited, walking );
    }
    for ( syntax::Token const next : mPhaseTexts[index].then )
    {
      if ( std::optional<PhaseIndex> const found = phaseNamed( next ); found.has_value() )
      {
        addOnce( phase.then, *found );
      }
    }
  }

  // `resident` puts a Module in the needs of every Phase, declared before it or
  // after. The one place the direction reverses, and it reverses here rather
  // than as the block was read because "every Phase" is not known until the
  // last include has returned.
  for ( syntax::Token const name : mResidentTexts )
  {
    for ( ModuleIndex const module : modulesNamed( name ) )
    {
      for ( Phase& phase : graph.phases )
      {
        addOnce( phase.needs, module );
      }
    }
  }
  reportEmptyGroups();

  // A `transform` names a Module, which is resolvable here, and a Section,
  // which is not: no Module has been assembled yet.
  for ( TransformText const& request : mTransformTexts )
  {
    if ( std::optional<ModuleIndex> const found = moduleNamed( request.module ); found.has_value() )
    {
      mProject.transforms.push_back(
          TransformRequest{ .transform = std::string{ mSources->textOf( request.transform.span() ) },
                            .module = *found,
                            .section = request.section.name,
                            .span = request.section.span } );
    }
  }

  bool entryKnown = implicit;
  if ( mEntryText.has_value() )
  {
    if ( std::optional<PhaseIndex> const found = phaseNamed( *mEntryText ); found.has_value() )
    {
      graph.entry = *found;
      entryKnown = true;
    }
  }
  else if ( !implicit )
  {
    report( diag::diagnostic( diag::DiagnosticId::PROJECT_HAS_NO_ENTRY ).sortedBy( root.generic_string() ) );
  }

  // Reachability is from the entry, so without one there is nothing to say
  // that has not been said.
  if ( entryKnown && !implicit )
  {
    reportUnreachablePhases();
  }

  deriveResidency( mProject );

  if ( !implicit )
  {
    for ( std::size_t module = 0; module < mProject.modules.size(); ++module )
    {
      if ( mProject.modules[module].residency.isEmpty() )
      {
        report( diag::diagnostic( diag::DiagnosticId::MODULE_IN_NO_PHASE )
                    .at( mModuleSites[module].begin, mModuleSites[module].length )
                    .arg( "module", mProject.modules[module].name ) );
      }
    }
  }
}

} // namespace

Project loadProject( diag::SourceManager& sources,
                     FileReader& files,
                     std::filesystem::path const& path,
                     std::filesystem::path const& library,
                     diag::SeverityPolicy& policy,
                     diag::DiagnosticSink& sink )
{
  Loader loader{ sources, files, library, policy, sink };
  loader.readDocument( path.lexically_normal(), std::nullopt );
  loader.resolvePhases( path );
  loader.resolveTarget();
  loader.addConstantModule();
  loader.checkContainer();

  Project project = std::move( loader ).take();
  if ( project.modules.empty() && !sink.hasErrors() )
  {
    // A Project with no Modules is not a program. Said only when nothing else
    // went wrong, since every other failure here produces one already.
    sink.add( diag::diagnostic( diag::DiagnosticId::PROJECT_HAS_NO_MODULES ).sortedBy( path.generic_string() ) );
  }

  // A Project with an edge takes Transitions, and the Modules that take them
  // are the tool's to add — see docs/decisions/0019-transition-mechanism.md.
  addTransitionModules( project, sources, sink );
  // An `.atr` is booted by its own first sectors, which are the tool's to
  // write — see docs/decisions/0211-a-diskette-is-a-container-and-its-sectors-are-storage.md.
  if ( project.container == Container::ATR )
  {
    addBootRecord( project, sources );
  }
  // A Project of C is given the runtime its operators call — see
  // docs/decisions/0095-literals-and-the-runtime.md.
  if ( holdsC( sources, project ) )
  {
    addCRuntime( project, sources );
  }
  return project;
}

} // namespace nga::model
