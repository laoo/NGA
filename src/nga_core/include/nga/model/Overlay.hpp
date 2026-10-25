#pragma once

#include "nga/model/ProjectFile.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>

namespace nga::model
{

/// A reader that answers some paths with text it was handed rather than what
/// is on disk — what an editor holds and has not saved — and every other path
/// from the reader beneath it.
///
/// Every file a build opens is opened through one FileReader, so a Module, an
/// `include`, a file of the library and a Generator's input are overlaid alike,
/// and a Project may name a file that exists only in an overlay. The path a
/// file is registered under stays the one it was asked for, so every finding,
/// token and Symbol names the file the reader is looking at — see
/// docs/decisions/0258-an-editor-reads-the-facts.md.
///
/// Paths are compared through `canonical`, which the caller supplies: making a
/// path canonical reads the disk, and `nga_core` does no I/O of its own.
class OverlaidFiles final : public FileReader
{
public:
  using Canonical = std::function<std::filesystem::path( std::filesystem::path const& )>;

  OverlaidFiles( FileReader& under, Canonical canonical );

  /// From now on, a read of `path` answers `text`.
  void overlay( std::filesystem::path const& path, std::string text );

  [[nodiscard]] std::optional<std::string> read( std::filesystem::path const& path ) override;

private:
  FileReader* mUnder;
  Canonical mCanonical;
  std::map<std::string, std::string> mTexts;
};

} // namespace nga::model
