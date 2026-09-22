#pragma once

#include "nga/model/Chunk.hpp"
#include "nga/model/PlacementClass.hpp"
#include "nga/syntax/Builder.hpp"
#include "nga/syntax/Expression.hpp"
#include "nga/syntax/Token.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace nga::model
{

/// What a Section was declared as. A Proc is a Section of its own — code
/// with one entry, the Label at its start — and a Temporary a zero-page
/// variable. Every Section is anonymous and named by its first Label. See
/// the glossary.
enum class SectionKind : std::uint8_t
{
  PLAIN,
  PROC,
  TEMPORARY,
};

/// An optionally named, placeable container of Chunks, produced by one Module.
///
/// The item arena is the reason Chunks are cheap: every expression a Chunk owns
/// lives here instead, Chunks are appended in order so their items are a
/// contiguous range, and appending is the only way to add one — which is what
/// keeps the ranges and the Chunks from ever disagreeing.
/// One branch of a conditional: what decides it, and the Chunks it holds.
///
/// A branch of an `.if` has a `condition`, null for `.else`, which is taken
/// when no branch before it was; a `.case` of a `.match` has a `pattern`,
/// and which of the two is meant is the Conditional's to say. The range is
/// half-open over the Chunks of whatever holds the branch — a Section, or a
/// macro's body while it is still a template.
struct Branch
{
  syntax::ExpressionPtr condition;
  syntax::Pattern pattern;
  ChunkIndex first;
  ChunkIndex last;
  diag::SourceSpan span;
};

/// A name a `.case` bound: which conditional of the body, which of its
/// branches, and which name of the pattern.
struct CaseBinding
{
  std::size_t conditional = 0;
  std::size_t branch = 0;
  std::uint32_t slot = 0;
};

/// `.if` through `.endif`, or `.match` through `.endmatch`: the branches in
/// the order they were written, of which at most one is kept. A range of
/// Chunks with a decider — a declared value, or the length of the pack
/// `subject` names — which Expand answers before it instantiates what the
/// range holds. See docs/decisions/0048-conditional-assembly.md, 0049 and
/// 0050.
struct Conditional
{
  std::optional<syntax::Token> subject;

  /// Which `.case` bound the pack `subject` names; absent when it is the
  /// macro's own pack, and when it names no pack, which has been reported.
  std::optional<CaseBinding> subjectCase;
  std::vector<Branch> branches;

  [[nodiscard]] bool isMatch() const
  {
    return subject.has_value();
  }
};

/// One byte of a Proc a `.declare` named: the Temporary the declaration
/// stood before, and the type it was declared to hold, where one was
/// written — see docs/decisions/0081-a-procs-signature-is-declared.md.
///
/// Or bytes a register carries: `place` says where each byte is, from the
/// low one, `a`, `x` or `y` for a register and `m` for a byte of the
/// reservation below, which holds exactly those and is there only where
/// there is one — see docs/decisions/0145-an-argument-in-a-register.md.
/// Empty where every byte is in the reservation, as it always was.
struct Declared
{
  std::optional<SectionIndex> temporary;
  std::optional<DeclaredType> type;
  diag::SourceSpan span{};
  std::string place{};

  /// How many bytes it is where `place` says, and how many of them the
  /// reservation holds.
  [[nodiscard]] std::uint32_t placedBytes() const
  {
    return static_cast<std::uint32_t>( place.size() );
  }

  [[nodiscard]] std::uint32_t reservedBytes() const
  {
    return static_cast<std::uint32_t>( std::ranges::count( place, 'm' ) );
  }
};

/// A Proc's **Signature**: its arguments in the order they were declared,
/// and its result, if it declares one. Declared rather than read off the
/// text, so that a line added at the top of a Proc cannot turn a parameter
/// into scratch. Empty for a Proc that declares nothing — one that takes
/// nothing and returns nothing — and for every Section that is not a Proc.
struct Signature
{
  std::vector<Declared> arguments;
  std::optional<Declared> result;

  [[nodiscard]] bool isEmpty() const
  {
    return arguments.empty() && !result.has_value();
  }
};

class Section
{
public:
  Section( PlacementClass placement,
           syntax::ExpressionPtr pinnedAddress,
           syntax::ExpressionPtr alignment,
           syntax::ExpressionPtr boundary,
           bool movable,
           bool root,
           SectionKind kind,
           diag::SourceSpan span )
      : mMovable( movable ), mRoot( root ), mKind( kind ), mPlacement( placement ),
        mPinnedAddress( std::move( pinnedAddress ) ), mAlignment( std::move( alignment ) ),
        mBoundary( std::move( boundary ) ), mSpan( span )
  {
  }

  /// Move-only, for the reason a Module is: it owns expressions.
  Section( Section const& ) = delete;
  Section& operator=( Section const& ) = delete;
  Section( Section&& ) = default;
  Section& operator=( Section&& ) = default;
  ~Section() = default;

  [[nodiscard]] PlacementClass placement() const
  {
    return mPlacement;
  }

  /// Whether the Section may stand at a different address in each Phase. A
  /// direct Reference holds it to one address across the referrer's
  /// Residency; see docs/decisions/0030-movable-sections.md.
  [[nodiscard]] bool isMovable() const
  {
    return mMovable;
  }

  /// Whether the Section is a Root: the hardware reaches it with no Reference
  /// from any Chunk, so Prune keeps it whatever names it. Declared, never
  /// derived — see docs/decisions/0033-prune.md.
  [[nodiscard]] bool isRoot() const
  {
    return mRoot;
  }

  /// Whether the Section is a Temporary: its value is not needed while no
  /// Section that names it is running or waiting for a call to return, so
  /// it may share addresses with another Temporary never active at the same
  /// time. Declared by `.ztemp`, never inferred — see the glossary.
  [[nodiscard]] bool isTemporary() const
  {
    return mKind == SectionKind::TEMPORARY;
  }

  /// Whether the Section is a Proc: code entered only through the Label at
  /// its start, declared by `.proc`. The scope of local labels, and the node
  /// Trace sees a call graph of.
  [[nodiscard]] bool isProc() const
  {
    return mKind == SectionKind::PROC;
  }

  /// What this Proc's `.declare`s said its arguments and its result are.
  /// Only a Proc carries one, and one that declares nothing carries an
  /// empty one.
  [[nodiscard]] Signature const& signature() const
  {
    return mSignature;
  }

  void declareArgument( Declared const& what )
  {
    mSignature.arguments.push_back( what );
  }

  void declareResult( Declared const& what )
  {
    mSignature.result = what;
  }

  /// Null unless the Section was pinned with `at`.
  [[nodiscard]] syntax::Expression const* pinnedAddress() const
  {
    return mPinnedAddress.get();
  }

  /// Null unless the Section was given `align`: it starts at a multiple of
  /// this.
  [[nodiscard]] syntax::Expression const* alignment() const
  {
    return mAlignment.get();
  }

  /// Null unless the Section was given `within`: it does not cross a multiple
  /// of this.
  [[nodiscard]] syntax::Expression const* boundary() const
  {
    return mBoundary.get();
  }

  [[nodiscard]] diag::SourceSpan span() const
  {
    return mSpan;
  }

  /// What `in PANE` wrote, if anything: the name, resolved at the end of
  /// Assemble against the Project's Panes into `pane`.
  [[nodiscard]] std::optional<syntax::Token> const& paneName() const
  {
    return mPaneName;
  }

  void setPaneName( syntax::Token name )
  {
    mPaneName = name;
  }

  /// The Pane the Section is in, once resolved; absent for a Section in
  /// `fixed` or in its Window's base state — see docs/decisions/0054-panes.md.
  [[nodiscard]] std::optional<PaneIndex> pane() const
  {
    return mPane;
  }

  void bindPane( PaneIndex pane )
  {
    mPane = pane;
  }

  /// What `under PANE` wrote on a Proc, if anything: the Pane the Proc runs
  /// with shown although it is not in it, resolved at the end of Assemble
  /// into `under` — see docs/decisions/0098-a-proc-declares-what-is-shown.md.
  [[nodiscard]] std::optional<syntax::Token> const& underName() const
  {
    return mUnderName;
  }

  void setUnderName( syntax::Token name )
  {
    mUnderName = name;
  }

  [[nodiscard]] std::optional<PaneIndex> under() const
  {
    return mUnder;
  }

  /// What `as NAME` wrote on a Proc, if anything: the function type whose
  /// Temporaries hold this Proc's arguments — see
  /// docs/decisions/0065-handlers.md.
  [[nodiscard]] std::optional<syntax::Token> const& asName() const
  {
    return mAsName;
  }

  void setAsName( syntax::Token name )
  {
    mAsName = name;
  }

  void bindUnder( PaneIndex pane )
  {
    mUnder = pane;
  }

  /// What `.endp then NAME` wrote, for a Proc: the name control falls
  /// through into. Resolved at the end of Assemble into `next`.
  [[nodiscard]] std::optional<syntax::Token> const& then() const
  {
    return mThen;
  }

  void setThen( syntax::Token name, std::string_view scope )
  {
    mThen = name;
    mThenScope = scope;
  }

  /// The Namespace the `then` stood in, where its name is looked for first.
  [[nodiscard]] std::string_view thenScope() const
  {
    return mThenScope;
  }

  /// The Namespace the Section was declared in — `one.` — where a name a
  /// macro use in it is looked for first; empty at the top level. A Section
  /// lies in one Namespace, since a block closes before the Namespace does.
  [[nodiscard]] std::string_view scope() const
  {
    return mScope;
  }

  void setScope( std::string_view scope )
  {
    mScope = scope;
  }

  /// The Proc this one falls through into, which the solver places
  /// immediately after it: `start(next) = start(this) + size(this)` in every
  /// Phase. Nothing unless `then` was written and named a Proc of this
  /// Module.
  [[nodiscard]] std::optional<SectionIndex> next() const
  {
    return mNext;
  }

  void setNext( SectionIndex index )
  {
    mNext = index;
  }

  /// Which format this Section's Payload waits as — an index into the
  /// formats the tool encodes. Zero, `copy`, unless the Project named
  /// another, which is recorded at the end of Assemble. A Section with no
  /// Payload carries it and nothing reads it. The number a Frame carries is
  /// the decoder's, and is looked up from this when the form is made.
  [[nodiscard]] std::uint8_t format() const
  {
    return mFormat;
  }

  void setFormat( std::uint8_t index )
  {
    mFormat = index;
  }

  [[nodiscard]] std::vector<Chunk> const& chunks() const
  {
    return mChunks;
  }

  /// For the end of Assemble alone, which resolves a Transition's name and
  /// fills a table's edges once the whole Project is in hand. No later Step
  /// changes a Chunk.
  [[nodiscard]] Chunk& chunkAt( ChunkIndex index )
  {
    return mChunks[index.value];
  }

  /// Whether any Chunk emits bytes. A Section of nothing but reservations
  /// occupies addresses, puts nothing in an image, and needs no Payload.
  [[nodiscard]] bool emitsBytes() const;

  /// What a Label defined here names: the Chunk that comes next. Two Labels in
  /// a row therefore name the same index, because a definition names the next
  /// Chunk rather than opening one.
  [[nodiscard]] ChunkIndex nextChunkIndex() const
  {
    return ChunkIndex{ static_cast<std::uint32_t>( mChunks.size() ) };
  }

  [[nodiscard]] std::span<syntax::ExpressionPtr const> itemsOf( Chunk const& chunk ) const
  {
    return std::span{ mItems }.subspan( chunk.firstItem, chunk.itemCount );
  }

  ChunkIndex appendChunk( ChunkContent content, diag::SourceSpan span, std::vector<syntax::ExpressionPtr> items );

  /// Adds items to the Chunk added last and widens its span to take in what
  /// they were written with: how a statement written over several lines — a
  /// `.dispatch` and its rows — stays one Chunk. The items of the last Chunk
  /// are the last in the arena, which is what makes this a range that grows.
  void extendLastChunk( std::vector<syntax::ExpressionPtr> items, diag::SourceSpan span );

  /// Marks a Chunk as written under `.own` or `.root`, with the followers
  /// `.own` named.
  void markTaking( ChunkIndex chunk, Taking taking, std::vector<syntax::Token> followers );

  /// What a `.root` taking does to the Section it names — see
  /// docs/decisions/0061-root-at-the-taking.md.
  void setRoot()
  {
    mRoot = true;
  }

  /// Puts expressions into the arena without a Chunk of their own: what the
  /// inner Chunks of an expansion are built over. The first index taken.
  std::uint32_t appendItems( std::vector<syntax::ExpressionPtr> items );

  /// The inside of a macro use, once Expand has given it one: the body's
  /// instructions and data, instantiated, in order, and reaching their
  /// expressions through this Section's arena as any Chunk does. Empty for
  /// every other Chunk, and for a use that expanded to nothing.
  [[nodiscard]] std::span<Chunk const> innerChunksOf( ChunkIndex index ) const;

  /// For Expand alone: gives a macro use its inside.
  void expand( ChunkIndex use, std::vector<Chunk> inner );

  /// The conditionals whose branches lie among this Section's Chunks, in the
  /// order they opened, so an enclosing one is decided before the one inside
  /// it.
  [[nodiscard]] std::span<Conditional const> conditionals() const
  {
    return mConditionals;
  }

  /// Opens one, and says which it is; the branches are filled as they close.
  std::size_t openConditional();

  [[nodiscard]] Conditional& conditionalAt( std::size_t index )
  {
    return mConditionals[index];
  }

  /// Leaves a Chunk occupying nothing and emitting nothing, which is what a
  /// branch not taken becomes: no items to walk, no bytes, no size, and no
  /// index moved, so nothing that names a position has to be told. See
  /// docs/decisions/0048-conditional-assembly.md.
  void blank( ChunkIndex index );

private:
  bool mMovable = false;
  bool mRoot = false;
  SectionKind mKind = SectionKind::PLAIN;
  std::uint8_t mFormat = 0;
  PlacementClass mPlacement = PlacementClass::ABSOLUTE;
  syntax::ExpressionPtr mPinnedAddress;
  syntax::ExpressionPtr mAlignment;
  syntax::ExpressionPtr mBoundary;
  diag::SourceSpan mSpan;
  std::optional<syntax::Token> mThen;
  std::string_view mThenScope;
  std::string_view mScope;
  std::optional<SectionIndex> mNext;
  std::optional<syntax::Token> mPaneName;
  std::optional<PaneIndex> mPane;
  std::optional<syntax::Token> mUnderName;
  std::optional<PaneIndex> mUnder;
  std::optional<syntax::Token> mAsName;

  /// Filled for a Proc alone, by the `.declare`s inside it.
  Signature mSignature;

  std::vector<Chunk> mChunks;
  std::vector<syntax::ExpressionPtr> mItems;

  std::vector<Conditional> mConditionals;

  /// Per macro use, by its index: the inner Chunks. Sparse, since most
  /// Chunks are not uses, and never walked to produce output.
  std::unordered_map<std::uint32_t, std::vector<Chunk>> mExpansions;
};

} // namespace nga::model
