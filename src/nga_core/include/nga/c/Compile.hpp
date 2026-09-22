#pragma once

#include "nga/c/Ir.hpp"
#include "nga/diag/DiagnosticSink.hpp"
#include "nga/diag/SourceManager.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace nga::c
{

/// One `.ngc` file of the program.
struct SourceFile
{
  diag::FileId file;

  /// What the text's `.source` marks name: the file as its entry wrote it,
  /// so that the text is the same wherever the tool runs.
  std::string path;

  /// The Panes pinned to a state that every Phase the Module is present in
  /// gives their Window as its base: shown wherever the Module's code runs,
  /// so a call into one is not wrapped — see
  /// docs/decisions/0096-panes-in-c.md.
  std::vector<std::string> shownByBase{};
};

/// What a name an `.asm` Module exports is in C — see
/// docs/decisions/0092-what-the-assembler-exports-is-typed-by-its-shape.md.
enum class ExternalKind : std::uint8_t
{
  /// The entry of a `.proc`, which C calls.
  PROC,

  /// Bytes C reads and writes at the name: a Label by the shape of what
  /// stands at it, or a named Region by its length.
  MEMORY,

  /// A Constant: a constant of C where `value` holds what it folds to, and
  /// otherwise a `u16` only the assembler knows.
  CONSTANT,

  /// A Label whose bytes have no shape, which `reason` says.
  SHAPELESS,

  /// A `reserved` Region, which C does not reach.
  RESERVED,

  /// What C reads nothing of: a Slot, a Window, a Pane, a macro, or a
  /// Constant that is one of those or a string, as `reason` says.
  UNREAD,

  /// A Charset, which C names only as a literal's prefix — see
  /// docs/decisions/0095-literals-and-the-runtime.md.
  CHARSET,

  /// A Pane, or a family of `paneCount` Panes, of Window `window`, which C
  /// names in `[[in]]` and `[[with]]` — see docs/decisions/0096-panes-in-c.md.
  PANE,

  /// A Window, which C names in `[[with]]`.
  WINDOW,
};

/// A byte of a Proc's Signature: the name its Label is written by outside
/// the Module, and what it holds — see
/// docs/decisions/0092-what-the-assembler-exports-is-typed-by-its-shape.md#calls--task-4b.
struct ExternalByte
{
  std::string name;
  ir::Type type = ir::Type::U8;

  /// `u8[N]`: bytes and no value, which C does not hand over.
  bool isBytes = false;

  /// How many bytes it is, and whether the `.declare` wrote its type, which
  /// an `extern` of C is held to — see
  /// docs/decisions/0094-extern-declares-how-c-calls-a-proc.md.
  std::uint32_t bytes = 1;
  bool isWritten = false;

  /// Where each byte is, from the low one: `a`, `x`, `y`, or `m` for a byte of
  /// `name`, in order. Empty where every byte is `name`'s — see
  /// docs/decisions/0145-an-argument-in-a-register.md.
  std::string place{};
};

/// One item of what stands at a Label, or one reservation: what an `extern`
/// declaration is held to — see
/// docs/decisions/0093-extern-declares-how-c-reads-the-assembler.md.
struct ExternalCell
{
  std::uint32_t bytes = 1;

  /// Bytes reserved and written by no item, which any value may lie across.
  bool isReserved = false;

  /// What the item folds to, where it does.
  std::optional<std::int64_t> value{};
};

/// A name an `.asm` Module exports, as far as the compiler asks about it.
struct ExternalName
{
  std::string name;
  ExternalKind kind = ExternalKind::UNREAD;

  /// Where the name is defined, which a finding about it points at beneath.
  std::optional<diag::SourceSpan> definition{};

  /// MEMORY: a `u8` or a `u16`, or an array of them, of `count` elements
  /// where that is known.
  ir::Type type = ir::Type::U8;
  bool isArray = false;
  std::optional<std::uint32_t> count{};

  /// CONSTANT: what it folds to, where it does.
  std::optional<std::int64_t> value{};

  /// SHAPELESS and UNREAD: why, as a finding says it.
  std::string reason{};

  /// MEMORY and SHAPELESS: what stands at the name, to the end of its
  /// Section, or to where `stop` says what stands that is no data; a named
  /// Region's length, as one reservation.
  std::vector<ExternalCell> cells{};
  std::string stop{};
  bool isRegion = false;

  /// PROC: the bytes its `.declare arg`s name, in order, and the one its
  /// `.declare ret` names.
  std::vector<ExternalByte> arguments{};
  std::optional<ExternalByte> result{};

  /// PROC and MEMORY: the Pane the Section is `in`, or empty; a call into a
  /// Proc of one is wrapped, and data of one is reached inside a block that
  /// shows it — see docs/decisions/0096-panes-in-c.md.
  std::string pane{};

  /// PROC: the Pane the Proc runs `under`, or empty; a call of it stands
  /// where the Pane is shown — see
  /// docs/decisions/0098-a-proc-declares-what-is-shown.md.
  std::string under{};

  /// PROC: the function type the Proc is declared `as`, or empty: its
  /// arguments are that type's Temporaries and not its own, so C reads its
  /// signature there — see docs/decisions/0065-handlers.md.
  std::string as{};

  /// PANE: its Window, how many Panes the family is — one for a Pane — and
  /// the named state it is pinned to, or empty for one the solver gives a
  /// Bank.
  std::string window{};
  std::uint32_t paneCount = 1;
  std::string paneState{};
};

/// What each `.ngc` file of the program becomes in the compiler's own form, in
/// the order the files are given, and nothing for a file that holds an error,
/// which has then been reported. The passes `compile` runs before it emits
/// the text; see there.
/// What the program is optimised for, as the compiler is told it. The model's
/// `Intent` by another name, copied rather than depended on for the reason
/// `AddressRange` below is: the compiler reads no model type — see
/// docs/decisions/0177-intent.md.
enum class Intent : std::uint8_t
{
  SPEED,
  SIZE,
  FIT,
};

/// The processor the program is written for, as the compiler is told it. The
/// model's `Cpu` by another name, copied rather than depended on for the
/// reason `AddressRange` below is — see
/// docs/decisions/0182-the-target-names-its-processor.md.
enum class Cpu : std::uint8_t
{
  MOS6502,
  WDC65SC02,
};

/// A run of addresses, `begin` included and `end` not: a register of the
/// Target, which C reads and writes as `volatile` whatever it declares, by
/// its name where the Target names it.
struct AddressRange
{
  std::uint32_t begin = 0;
  std::uint32_t end = 0;
  std::string name{};
};

std::vector<std::optional<ir::Unit>> lower( diag::SourceManager const& sources,
                                            std::span<SourceFile const> files,
                                            std::span<ExternalName const> externals,
                                            diag::DiagnosticSink& sink,
                                            std::span<AddressRange const> registers = {},
                                            Intent intent = Intent::FIT );

/// The text each `.ngc` file of the program compiles to, in the order the
/// files are given, and nothing for a file that holds an error, which has
/// then been reported — see docs/spec/c-subset.md#the-text-a-module-compiles-to.
///
/// The files are compiled together because a name is visible to the whole
/// program: the signatures of every file are gathered, beside what the
/// assembler exports, before any body is checked — see
/// docs/decisions/0062-a-subset-of-c.md#no-declaration-on-either-side.
std::vector<std::optional<std::string>> compile( diag::SourceManager const& sources,
                                                 std::span<SourceFile const> files,
                                                 std::span<ExternalName const> externals,
                                                 diag::DiagnosticSink& sink,
                                                 std::span<AddressRange const> registers = {},
                                                 Intent intent = Intent::FIT,
                                                 Cpu cpu = Cpu::MOS6502 );

} // namespace nga::c
