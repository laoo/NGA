#pragma once

#include "nga/diag/DiagnosticSink.hpp"
#include "nga/diag/SeverityPolicy.hpp"
#include "nga/diag/SourceManager.hpp"
#include "nga/model/Project.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace nga::model
{

/// How the loader reaches a file it was told to read.
///
/// Injected because `nga_core` does no I/O of its own: the CLI hands it the
/// filesystem and the regression suite hands it documents it holds in memory,
/// and neither is a special case of the other.
class FileReader
{
public:
  FileReader() = default;
  FileReader( FileReader const& ) = delete;
  FileReader( FileReader&& ) = delete;
  FileReader& operator=( FileReader const& ) = delete;
  FileReader& operator=( FileReader&& ) = delete;
  virtual ~FileReader() = default;

  /// The text at `path`, or nothing when it cannot be read.
  [[nodiscard]] virtual std::optional<std::string> read( std::filesystem::path const& path ) = 0;
};

/// Reads a `.ngp` and everything it includes.
///
/// Files are registered with the SourceManager as they are met, so registration
/// order is document order and the ordering contract in
/// docs/spec/diagnostics.md needs no exception for inclusion.
///
/// A path a document names is resolved against the document's directory,
/// and where no file is there, against `library`: the directory of what the
/// tool ships as source — drivers and decoders — so that a variant lists
/// `atari/portb.asm` as it lists a file of its own. See
/// docs/spec/project-file.md.
///
/// The severity overrides the document declares are applied to `policy`, and
/// therefore take effect only for what happens *after* this returns: a Project
/// file cannot suppress the diagnostics raised while reading it, which is the
/// only order that does not depend on where in the file the block was written.
Project loadProject( diag::SourceManager& sources,
                     FileReader& files,
                     std::filesystem::path const& path,
                     std::filesystem::path const& library,
                     diag::SeverityPolicy& policy,
                     diag::DiagnosticSink& sink );

} // namespace nga::model
