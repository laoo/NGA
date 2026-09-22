#pragma once

#include "nga/diag/SourceManager.hpp"
#include "nga/model/Merge.hpp"
#include "nga/model/Project.hpp"

#include <span>

namespace nga::model
{

class Bytes;
class Freezes;
class Interference;
class Layout;
class Reachable;
class Sizes;
class Storage;

/// What the build has produced up to the end of Merge, and what every Step
/// after it is handed.
///
/// Each Step takes some prefix of one sequence of results and none takes all
/// of it, so the prefix is what a Step names: it asks for the stage it needs
/// and reads what that stage carries. Nothing is merged here — every result is
/// still its own object, produced by its own Step, and a stage holds
/// references to them rather than copies, so
/// docs/decisions/0012-step-results-are-separate-objects.md is untouched.
///
/// The stages are **types** and not fields of one object, because that is what
/// keeps the forbidden loop unwritable rather than merely forbidden: Size is
/// handed a Merged, which has no Layout in it, so an instruction's width
/// cannot be decided from an address the solver chose. See
/// docs/decisions/0032-a-step-is-handed-the-build-so-far.md.
///
/// The Target is the Project's, so a run has exactly one and a Step cannot
/// read a different one than it was placed against.
class Merged
{
public:
  Merged( diag::SourceManager const& sources,
          Project const& project,
          GlobalSymbols const& symbols,
          Charsets const& charsets )
      : mSources( sources ), mProject( project ), mSymbols( symbols ), mCharsets( charsets )
  {
  }

  [[nodiscard]] diag::SourceManager const& sources() const
  {
    return mSources;
  }

  [[nodiscard]] Project const& project() const
  {
    return mProject;
  }

  [[nodiscard]] PhaseGraph const& phases() const
  {
    return mProject.phases;
  }

  [[nodiscard]] Target const& target() const
  {
    return mProject.target;
  }

  [[nodiscard]] GlobalSymbols const& symbols() const
  {
    return mSymbols;
  }

  [[nodiscard]] Charsets const& charsets() const
  {
    return mCharsets;
  }

  [[nodiscard]] std::span<Module const> modules() const
  {
    return mSymbols.modules();
  }

private:
  diag::SourceManager const& mSources;
  Project const& mProject;
  GlobalSymbols const& mSymbols;
  Charsets const& mCharsets;
};

/// Merged, and what Prune kept. What the type check is handed, so that a
/// Reference from a Section nothing reaches holds nothing still.
class Pruned : public Merged
{
public:
  Pruned( Merged const& merged, Reachable const& reachable ) : Merged( merged ), mReachable( reachable ) {}

  [[nodiscard]] Reachable const& reachable() const
  {
    return mReachable;
  }

private:
  Reachable const& mReachable;
};

/// Pruned, and which Temporaries Trace found never live at once. What the
/// type check and Size are handed, though neither reads it: it stands here
/// because Trace walks the References Prune walked and needs nothing of what
/// comes after, and Place reads it.
class Traced : public Pruned
{
public:
  Traced( Pruned const& pruned, Interference const& interference ) : Pruned( pruned ), mInterference( interference ) {}

  [[nodiscard]] Interference const& interference() const
  {
    return mInterference;
  }

private:
  Interference const& mInterference;
};

/// Traced, and what the type check and Size produced. What Place is handed.
///
/// The two arrive together because they are what stands between Trace and
/// Place, and neither reads the other: the type check resolves References and
/// records where a Movable Section is held still, Size resolves widths, and a
/// Step that needs one of them has always run after both.
class Sized : public Traced
{
public:
  Sized( Traced const& traced, Freezes const& freezes, Sizes const& sizes )
      : Traced( traced ), mFreezes( freezes ), mSizes( sizes )
  {
  }

  [[nodiscard]] Freezes const& freezes() const
  {
    return mFreezes;
  }

  [[nodiscard]] Sizes const& sizes() const
  {
    return mSizes;
  }

private:
  Freezes const& mFreezes;
  Sizes const& mSizes;
};

/// Sized, and where the solver put every Section. What Patch is handed.
class Placed : public Sized
{
public:
  Placed( Sized const& sized, Layout const& layout ) : Sized( sized ), mLayout( layout ) {}

  [[nodiscard]] Layout const& layout() const
  {
    return mLayout;
  }

private:
  Layout const& mLayout;
};

/// Everything a build produces, which is what a Container is written from.
///
/// Storage joins the build here and not at Place, and that is the whole of why
/// a stage carries a result only once it is finished: before Patch, Storage
/// answers which Sections have a Payload and not where one waits, so a stage
/// holding it would offer half an answer to anything that asked. Until then it
/// is threaded as itself, by the Steps that fill it. The Bytes are the same
/// case one Step later — the tables are written into them after Patch has run.
class Patched : public Placed
{
public:
  Patched( Placed const& placed, Storage const& storage, Bytes const& bytes )
      : Placed( placed ), mStorage( storage ), mBytes( bytes )
  {
  }

  [[nodiscard]] Storage const& storage() const
  {
    return mStorage;
  }

  [[nodiscard]] Bytes const& bytes() const
  {
    return mBytes;
  }

private:
  Storage const& mStorage;
  Bytes const& mBytes;
};

} // namespace nga::model
