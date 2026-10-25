#pragma once

#include "nga/diag/DiagnosticSink.hpp"
#include "nga/model/Prune.hpp"
#include "nga/model/Trace.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

// The Trace Step's graph, which one pass reads off the program and the others
// walk: a Node per Section, a Point per Chunk of it, and the Calls, Jumps and
// Successors between them. Each pass over it — what runs off the end of a
// Section, the Liveness of every Temporary and the Interference from it, and
// what a Holder holds — is a file of its own, and Trace.cpp is the order they
// run in.

namespace nga::model::flow
{

/// A position a Reference names: a Chunk of a Section, or an inner Chunk of
/// a macro use where a local label of the body stands.
struct Position
{
  SectionRef where;
  ChunkIndex chunk;
  std::optional<std::uint32_t> inner;
};

/// The position a Symbol's memory starts at, as Prune answers it.
[[nodiscard]] std::optional<Position> positionOf( GlobalSymbols const& symbols, SymbolRef where );

/// A set of bits, sized once: what the liveness of every byte of every
/// Temporary is held in, one bit per byte the program names.
class BitSet
{
public:
  BitSet() = default;

  explicit BitSet( std::size_t bits ) : mWords( ( bits + 63 ) / 64, 0 ) {}

  void set( std::size_t bit )
  {
    mWords[bit / 64] |= std::uint64_t{ 1 } << ( bit % 64 );
  }

  [[nodiscard]] bool test( std::size_t bit ) const
  {
    return ( ( mWords[bit / 64] >> ( bit % 64 ) ) & 1U ) != 0;
  }

  /// Whether any bit of `[first, first + count)` is set.
  [[nodiscard]] bool anyIn( std::size_t first, std::size_t count ) const
  {
    for ( std::size_t bit = first; bit < first + count; ++bit )
    {
      if ( test( bit ) )
      {
        return true;
      }
    }
    return false;
  }

  /// Adds every bit of `other`; whether anything changed.
  bool unite( BitSet const& other )
  {
    bool changed = false;
    for ( std::size_t word = 0; word < mWords.size(); ++word )
    {
      std::uint64_t const next = mWords[word] | other.mWords[word];
      changed = changed || next != mWords[word];
      mWords[word] = next;
    }
    return changed;
  }

  void subtract( BitSet const& other )
  {
    for ( std::size_t word = 0; word < mWords.size(); ++word )
    {
      mWords[word] &= ~other.mWords[word];
    }
  }

  friend bool operator==( BitSet const&, BitSet const& ) = default;

private:
  std::vector<std::uint64_t> mWords;
};

/// One byte of a Temporary a Chunk reads or writes: which, when the operand
/// says — `ptr + 1` — and nothing when it does not, as `buf,x` does not.
struct Access
{
  std::uint32_t temporary = 0;
  std::optional<std::int64_t> offset;
  bool reads = false;
  bool writes = false;
};

/// Where a transfer lands: a Section, and the index of a Point in it, which
/// is the count when the position is the one after its last Chunk.
struct Landing
{
  std::uint32_t node = 0;
  std::uint32_t point = 0;
};

/// One Chunk as the flow of its Section sees it — an instruction, a data
/// statement, or one inner Chunk of a macro use — in the order they stand.
/// Its Successors are the Points of its own Section that may run next; a
/// transfer is a position in another Section that runs next, or, under a
/// Call, below this one. See the glossary.
struct Point
{
  std::vector<Access> accesses;
  bool fallsThrough = true;

  /// The statement this Point is of — a macro use's, for its inner Chunks —
  /// whether it is an instruction, and whether control goes past the end of
  /// the Section from here with no `then` to say where.
  diag::SourceSpan statement;
  bool instruction = false;
  bool runsOff = false;

  /// `brk` or `stp`: what follows is not reached by running on, whatever
  /// the processor would do — a trap is where a program says it stops.
  bool traps = false;

  std::vector<Position> targets;
  bool calls = false;

  /// A jump whose target no name says — through a pointer, or to a literal
  /// address — and a return: where a Section follows an address it owns,
  /// since nothing else of it goes anywhere its Points do not name.
  bool loose = false;
  bool returns = false;

  std::vector<std::uint32_t> successors;
  std::vector<Landing> transfers;
  std::vector<std::uint32_t> callTargets;

  /// What this Point hands to a Holder, as Holder and Node; the Holder it
  /// empties; and the Phase a `.transition` here enters — see
  /// docs/decisions/0247-the-hardware-holds-a-section-for-as-long-as-it-is-given-it.md.
  std::vector<std::pair<std::uint32_t, std::uint32_t>> handsOver;
  std::optional<std::uint32_t> releases;
  std::optional<PhaseIndex> enters;

  /// The bytes this Point reads, the bytes it writes whole, and every byte
  /// it touches either way; and which Temporaries those bytes are of.
  BitSet uses;
  BitSet defs;
  BitSet touchedBits;
  std::vector<std::uint32_t> touched;

  /// What is live into and out of this Point: a byte whose value some path
  /// from here reads before it writes it.
  BitSet liveIn;
  BitSet liveOut;
};

/// One Section as the graph sees it.
struct Node
{
  SectionRef where;
  std::vector<std::uint32_t> calls;
  std::vector<std::uint32_t> jumps;

  /// Every Section a Chunk here names, which a Holder holding this one holds
  /// with it.
  std::vector<std::uint32_t> names;

  /// Where a `.transition` here enters: the entry of the Phase entered. Kept
  /// apart from `jumps` because it ends every activation on the stack — the
  /// routine jumps to that entry and no frame is ever resumed — so a walk
  /// asking what may be running at once stops here, while one asking what a
  /// run can reach does not. See
  /// docs/decisions/0102-a-transition-ends-the-activation.md.
  std::vector<std::uint32_t> enters;
  bool callsItself = false;
  bool code = false;

  /// Whether some address of code is this Section's to follow — taken under
  /// `.own` here, or given by `.own NAME` elsewhere — and where its `jmp (…)`
  /// through no Slot is, if it has one: a Section that jumps through a
  /// pointer and owns nothing has a jump nobody said the target of.
  bool ownsCode = false;
  std::optional<diag::SourceSpan> indirectJumpAt;

  /// The code this Section follows an owned address into, by Section: what
  /// its jumps the Points do not name the target of go to. One into a
  /// position of this Section itself leaves the flow inside it unknown.
  std::vector<std::uint32_t> ownedJumps;
  bool ownsSelf = false;
  bool temporary = false;
  bool temporaryEscaped = false;

  /// Where the address was first taken, for the finding.
  std::optional<diag::SourceSpan> temporaryEscapedAt;
  std::vector<std::uint32_t> owners;

  /// The Section's Chunks as Points, in order, with the first Point of
  /// every Chunk — a macro use's is its first inner one — and one more
  /// index for the position after the last.
  std::vector<Point> points;
  std::vector<std::uint32_t> firstOf;

  /// Temporaries this Section reaches through an address taken under
  /// `.own`: read and written at no Point the tool can see, so live at all
  /// of them.
  std::vector<std::uint32_t> ownedTemporaries;
};

/// The edges of a Node, sorted and each once.
void dedupe( std::vector<std::uint32_t>& edges );

/// Every reachable Section as a Node, and its Chunks as Points.
class Graph
{
public:
  explicit Graph( Pruned const& build ) : mBuild( &build )
  {
    std::span<Module const> const modules = build.modules();
    mFirstOf.reserve( modules.size() + 1 );
    std::uint32_t count = 0;
    for ( Module const& module : modules )
    {
      mFirstOf.push_back( count );
      count += static_cast<std::uint32_t>( module.sections().size() );
    }
    mFirstOf.push_back( count );
    mNodes.reserve( count );
    for ( std::uint32_t module = 0; module < modules.size(); ++module )
    {
      for ( std::uint32_t index = 0; index < modules[module].sections().size(); ++index )
      {
        Node node;
        node.where = SectionRef{ .module = ModuleIndex{ module }, .section = SectionIndex{ index } };
        node.temporary = modules[module].sections()[index].isTemporary();
        mNodes.push_back( std::move( node ) );
      }
    }
  }

  [[nodiscard]] Pruned const& build() const
  {
    return *mBuild;
  }

  [[nodiscard]] std::vector<Node>& nodes()
  {
    return mNodes;
  }

  [[nodiscard]] std::vector<Node> const& nodes() const
  {
    return mNodes;
  }

  [[nodiscard]] std::uint32_t indexOf( SectionRef where ) const
  {
    return mFirstOf[where.module.value] + where.section.value;
  }

  [[nodiscard]] std::string nameOf( Node const& node ) const
  {
    return mBuild->symbols().moduleAt( node.where.module ).displayNameOf( node.where.section );
  }

  [[nodiscard]] diag::SourceSpan spanOf( Node const& node ) const
  {
    return mBuild->symbols().moduleAt( node.where.module ).sectionAt( node.where.section ).span();
  }

  /// Marks everything a run from `from` may reach. `acrossTransitions` says
  /// whether a `.transition` is followed: it is, for what a run reaches, and
  /// it is not for what may be running at once, since nothing beyond a
  /// transition resumes anything that was.
  void
  reachFrom( std::vector<std::uint32_t> const& from, std::vector<bool>& reached, bool acrossTransitions = true ) const
  {
    std::vector<std::uint32_t> pending;
    for ( std::uint32_t const target : from )
    {
      if ( !reached[target] )
      {
        reached[target] = true;
        pending.push_back( target );
      }
    }
    while ( !pending.empty() )
    {
      std::uint32_t const next = pending.back();
      pending.pop_back();
      std::vector<std::vector<std::uint32_t> const*> sets{ &mNodes[next].calls, &mNodes[next].jumps };
      if ( acrossTransitions )
      {
        sets.push_back( &mNodes[next].enters );
      }
      for ( std::vector<std::uint32_t> const* edges : sets )
      {
        for ( std::uint32_t const target : *edges )
        {
          if ( !reached[target] )
          {
            reached[target] = true;
            pending.push_back( target );
          }
        }
      }
    }
  }

  /// Which Point of a Section a position is: a Chunk's first, an inner
  /// Chunk's own, or the count for the position after the last.
  [[nodiscard]] static std::uint32_t pointIndexOf( Node const& node, Position const& position )
  {
    auto const count = static_cast<std::uint32_t>( node.points.size() );
    if ( node.firstOf.empty() || position.chunk.value + 1 >= node.firstOf.size() )
    {
      return count;
    }
    std::uint32_t const index = node.firstOf[position.chunk.value] + position.inner.value_or( 0 );
    return std::min( index, count );
  }

private:
  Pruned const* mBuild;
  std::vector<std::uint32_t> mFirstOf;
  std::vector<Node> mNodes;
};

/// The graph of the program, every reachable Section's References read as
/// edges and Points, and what reading them finds wrong said.
[[nodiscard]] Graph readGraph( Pruned const& build, diag::DiagnosticSink& sink );

/// Code that runs off the end of a Section with nothing after it, and a
/// final Phase's code that returns to nothing.
void reportRunningOff( Graph const& graph, diag::DiagnosticSink& sink );
void reportFinalReturns( Graph const& graph, diag::DiagnosticSink& sink );

/// The Liveness of every Temporary, and from it which pairs may share:
/// `always` is every Section active in every state, under a `root`.
[[nodiscard]] Interference interferenceOf( Graph& graph, std::vector<bool> const& always, diag::DiagnosticSink& sink );

/// What some Holder may hold, and in which Phases.
[[nodiscard]] Holds holdsOf( Graph const& graph, std::vector<bool> const& always );

} // namespace nga::model::flow
