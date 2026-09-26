#pragma once

#include "nga/diag/DiagnosticSink.hpp"
#include "nga/diag/SourceLocation.hpp"
#include "nga/model/PlacementClass.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace nga::model
{

/// Bytes a Section holds, written out as a `.base64` payload.
struct GeneratedBytes
{
  std::vector<std::uint8_t> bytes{};
};

/// Space that occupies addresses and emits nothing: `.res`.
struct GeneratedReserve
{
  std::int64_t size = 0;
};

/// A name for a number the generator computed: `NAME = VALUE`.
struct GeneratedConstant
{
  std::string name{};
  std::int64_t value = 0;
};

/// What an item holds beside the label it may carry. `std::monostate` is a
/// label and nothing else, which is how a position past the last byte is named.
using GeneratedContent = std::variant<std::monostate, GeneratedBytes, GeneratedReserve, GeneratedConstant>;

/// One item of a Section: one statement of the emitted text, with the label in
/// front of it on the same line.
struct GeneratedItem
{
  std::optional<std::string> label{};
  GeneratedContent content{};
};

/// One Section of a generated Module: what a `.section` carries, its name —
/// the Label emitted at its start, which is what names a Section — and the
/// items in the order they are emitted.
///
/// Plain values rather than the parser's `SectionAttributes`, because nothing
/// here comes from source: a script returns this. It is also not held to the
/// assembler's rules — `temporary` beside `in` is refused where the emitted
/// text is parsed, exactly as it is in text anyone wrote.
struct GeneratedSection
{
  std::string name{};
  PlacementClass placement = PlacementClass::ABSOLUTE;
  std::optional<std::int64_t> pinnedAddress{};
  std::optional<std::int64_t> alignment{};
  std::optional<std::int64_t> boundary{};
  std::optional<std::string> pane{};
  bool movable = false;
  bool root = false;
  bool temporary = false;
  bool readOnly = false;
  std::vector<GeneratedItem> items{};
};

/// What a generator produces, and all the emitter reads — see
/// docs/spec/generators.md and docs/decisions/0069-generators.md.
struct GeneratedModule
{
  /// The file the generator worked from, which the `.source` mark names.
  std::string source{};

  /// A Namespace around everything the Module declares, or none. It keeps two
  /// generated Modules that name a Section alike out of each other's way, since
  /// every name a generator defines is exported.
  std::optional<std::string> namespaceName{};

  std::vector<GeneratedSection> sections{};
};

/// Where the description is one the emitter cannot use, as
/// `sections[1].items[3]` names it, and nothing where it can.
///
/// A generator of the tool's own cannot produce a bad description; a script
/// can, and a fault in one has to be findable without a debugger.
[[nodiscard]] std::optional<std::string> checkModule( GeneratedModule const& description );

/// The text of the `.asm` Module the description stands for.
///
/// The text is the contract: it is what `--emit-asm` writes, what the suite
/// holds to a golden, and what Assemble reads. The description is this
/// function's input and may change without changing anyone's result.
[[nodiscard]] std::string emitModule( GeneratedModule const& description );

/// One argument of a generator call, reduced to what the Project wrote: a
/// quoted literal, the value of an expression, a bare word after `=` — a name
/// the generator reads, and no Symbol, since the Project has none — or nothing
/// at all, which is a flag.
struct GeneratorArgument
{
  /// The parameter named, empty for the one positional argument.
  std::string name{};

  /// Absent where the argument is a flag, `root`.
  std::optional<std::string> text{};
  std::optional<std::int64_t> number{};
  std::optional<std::string> word{};

  /// What a diagnostic about this argument underlines.
  diag::SourceSpan span{};
};

/// A generator call as the Project wrote it.
struct GeneratorCall
{
  std::string generator{};
  diag::SourceSpan generatorSpan{};

  /// The Module's name, for a description that wants to derive one.
  std::string module{};

  std::vector<GeneratorArgument> arguments{};
};

/// How a generator reaches a file it was told to read: resolved beside the
/// document that wrote the entry and then in the library, as a Module's path
/// is. `resolved` is the path a message should name whether the read worked or
/// not.
class GeneratorFiles
{
public:
  GeneratorFiles() = default;
  GeneratorFiles( GeneratorFiles const& ) = delete;
  GeneratorFiles( GeneratorFiles&& ) = delete;
  GeneratorFiles& operator=( GeneratorFiles const& ) = delete;
  GeneratorFiles& operator=( GeneratorFiles&& ) = delete;
  virtual ~GeneratorFiles() = default;

  [[nodiscard]] virtual std::optional<std::string> read( std::string const& path, std::string& resolved ) const = 0;
};

/// Runs the JavaScript a `script` entry names, in the engine the tool embeds.
///
/// Declared beside `runGenerator` because `script` is a generator of the tool's
/// own; nothing outside the model calls it.
[[nodiscard]] std::optional<GeneratedModule>
runScript( GeneratorCall const& call, GeneratorFiles const& files, diag::DiagnosticSink& sink );

/// Runs the generator the call names, reporting what it refuses and answering
/// nothing where no Module came of it.
[[nodiscard]] std::optional<GeneratedModule>
runGenerator( GeneratorCall const& call, GeneratorFiles const& files, diag::DiagnosticSink& sink );

} // namespace nga::model
