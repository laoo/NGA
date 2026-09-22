#pragma once

#include "nga/diag/SourceLocation.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace nga::diag
{

/// Identifies a file registered with a SourceManager.
struct FileId
{
  std::uint32_t value = 0;

  friend bool operator==( FileId, FileId ) = default;
};

/// A SourceLocation expanded for display. Line and column are one-based, as
/// every editor and every compiler message in the world reports them.
struct ExpandedLocation
{
  std::string_view path;
  std::uint32_t line = 0;
  std::uint32_t column = 0;
};

/// Where a position's text came from, when a `.source` mark says so: the
/// file and line the generator read, beside the position in what it wrote.
struct SourceReference
{
  std::string_view path;
  std::uint32_t line = 0;
};

/// Owns the text of every file and lays them out end to end in one virtual
/// offset space.
///
/// Files are ranked in Project order: by when they are registered, except for
/// text the tool writes for a Module once later files are registered, which is
/// ranked beside the file it was written from — see addFileAfter and the
/// ordering contract in docs/spec/diagnostics.md.
class SourceManager
{
public:
  FileId addFile( std::string path, std::string contents );

  /// Registers a file ranked immediately after `after`, and after every file
  /// already ranked there, rather than after every file registered so far.
  /// What a `.ngc` Module compiles to is written during Assemble, when the whole
  /// Project is registered, and a finding on it belongs with its Module and not
  /// behind the last one.
  FileId addFileAfter( FileId after, std::string path, std::string contents );

  /// The file that the file holding `location` was registered after, when it
  /// was registered by addFileAfter: for text the tool wrote, the file it was
  /// written from.
  [[nodiscard]] std::optional<FileId> registeredAfter( SourceLocation location ) const;

  [[nodiscard]] std::uint32_t fileCount() const
  {
    return static_cast<std::uint32_t>( mFiles.size() );
  }

  [[nodiscard]] std::string_view pathOf( FileId file ) const;
  [[nodiscard]] std::string_view contentsOf( FileId file ) const;

  /// Location of a byte offset within a file. Offsets one past the last byte
  /// are valid, so a diagnostic can point at the end of a file.
  [[nodiscard]] SourceLocation locationOf( FileId file, std::uint32_t offsetInFile ) const;

  [[nodiscard]] ExpandedLocation expand( SourceLocation location ) const;

  /// The key that orders locations for output: the file's rank, then its place
  /// among the files ranked after the same one, then its offset within the file.
  ///
  /// This is deliberately not the raw SourceLocation offset. Raw offsets are
  /// handed out in registration order, so a location in text registered later —
  /// what a `.ngc` compiles to, or a macro expansion range once those exist —
  /// would sort behind every real file, and its offset would depend on which
  /// thread reached it first. See docs/spec/diagnostics.md.
  [[nodiscard]] std::uint64_t orderKeyFor( SourceLocation location ) const;

  /// The text a span covers. Tokens carry a position and a length instead of a
  /// copy, so this is how a consumer reads one.
  [[nodiscard]] std::string_view textOf( SourceSpan span ) const;

  /// The whole line containing the location, without its terminator.
  [[nodiscard]] std::string_view lineTextAt( SourceLocation location ) const;

  /// A `.source` mark: every location in the same file from `from` to the
  /// next mark is said to come from `line` of `path`. Registered once every
  /// Module is assembled, from what each carries — see
  /// docs/decisions/0063-source-marks.md.
  void addSourceMark( SourceLocation from, std::string path, std::uint32_t line );

  /// The mark covering a location, if one does. A position before the first
  /// mark of its file, or in a file with none, has no source but itself.
  [[nodiscard]] std::optional<SourceReference> sourceOf( SourceLocation location ) const;

private:
  struct Mark
  {
    std::uint32_t offset = 0;
    std::string path;
    std::uint32_t line = 0;
  };

  struct File
  {
    std::string path;
    std::string contents;
    std::uint32_t start = 0;

    /// Where the file sorts: its registration index, or the rank of the file
    /// it was registered after, with `follower` counting from one among those.
    std::uint32_t rank = 0;
    std::uint32_t follower = 0;

    /// The file this one was registered after, for one addFileAfter added.
    std::optional<std::uint32_t> anchor;

    std::vector<std::uint32_t> lineStarts;
    std::vector<Mark> marks; ///< sorted by offset
  };

  /// Index into mFiles, or mFiles.size() when the location belongs to no file.
  [[nodiscard]] std::size_t indexOfFileContaining( SourceLocation location ) const;

  /// A deque and not a vector: every name in the model is a view into a
  /// file's text, and a file registered late — the dispatcher the end of
  /// Assemble writes — must not move the ones before it. A short pseudo-source
  /// lives inside its std::string, and moving that string moves the text.
  std::deque<File> mFiles;

  // Offset zero is reserved for the invalid location, so the first file starts
  // at one. Each file also owns one offset past its last byte.
  std::uint32_t mNextStart = 1;
};

} // namespace nga::diag
