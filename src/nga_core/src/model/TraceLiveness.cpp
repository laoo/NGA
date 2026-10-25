#include "nga/model/TraceGraph.hpp"

#include "nga/model/Evaluate.hpp"
#include "nga/model/Isa.hpp"
#include "nga/model/Prune.hpp"
#include "nga/model/References.hpp"
#include "nga/model/Transition.hpp"
#include "nga/model/TypeCheck.hpp"

#include <algorithm>
#include <cstddef>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace nga::model::flow
{

namespace
{

/// The Liveness of every byte of every Temporary, per Point, and from it
/// which Temporaries are never live at once.
class Liveness
{
public:
  explicit Liveness( Graph& graph ) : mBuild( &graph.build() ), mGraph( &graph ) {}

  Interference interfere( std::vector<bool> const& always, diag::DiagnosticSink& sink )
  {
    assignBits();
    solveLiveness();

    // Per Temporary, from the Points it is live at: the Sections it is live
    // in, the Sections that may be below one of those Points — everything
    // a Call from it reaches — and which other Temporaries share a Point
    // with it.
    std::size_t const count = mTemporaries.size();
    std::vector<std::vector<bool>> inSections( count, std::vector<bool>( nodes().size(), false ) );
    std::vector<std::vector<std::uint32_t>> callSeeds( count );
    std::vector<bool> together( count * count, false );
    std::vector<std::uint32_t> present;
    for ( std::uint32_t index = 0; index < nodes().size(); ++index )
    {
      for ( Point const& point : nodes()[index].points )
      {
        present.clear();
        for ( std::uint32_t temporary = 0; temporary < count; ++temporary )
        {
          if ( isLiveAt( temporary, point ) )
          {
            present.push_back( temporary );
          }
        }
        for ( std::uint32_t const temporary : present )
        {
          inSections[temporary][index] = true;
          callSeeds[temporary].insert( callSeeds[temporary].end(), point.callTargets.begin(), point.callTargets.end() );
          for ( std::uint32_t const other : present )
          {
            together[( temporary * count ) + other] = true;
          }
        }
      }
    }

    // Per Temporary: whether it may share at all, and every Section that is
    // active while it is live — the ones it is live in, and everything a
    // path from a Point it is live at beginning with a Call reaches.
    struct Candidate
    {
      std::uint32_t temporary = 0;
      std::vector<bool> below;
    };

    std::vector<Candidate> candidates;
    for ( std::uint32_t temporary = 0; temporary < count; ++temporary )
    {
      Node const& node = nodes()[mTemporaries[temporary]];
      diag::SourceSpan const span = mGraph->spanOf( node );
      if ( node.temporaryEscaped )
      {
        // At the taking, since that is what to change; a Temporary has no
        // address to hand out — see docs/decisions/0058-temporary-is-an-attribute.md.
        diag::SourceSpan const at = node.temporaryEscapedAt.value_or( span );
        sink.add( diag::diagnostic( diag::DiagnosticId::TEMPORARY_ESCAPED )
                      .at( at.begin, at.length )
                      .arg( "section", mGraph->nameOf( node ) ) );
        continue;
      }
      bool underRoot = false;
      for ( std::uint32_t index = 0; index < nodes().size(); ++index )
      {
        underRoot = underRoot || ( inSections[temporary][index] && always[index] );
      }
      if ( underRoot )
      {
        continue;
      }

      // An Owner a Call from itself comes back to holds two Activations at
      // once, and the second destroys the first's value. Only a path whose
      // first edge is a Call the source spells says so; one that begins with
      // a Jump under a call into the Section itself may be a state machine,
      // and merely keeps the Temporary from sharing.
      std::optional<std::uint32_t> recursive;
      for ( std::uint32_t const owner : node.owners )
      {
        std::vector<bool> reached( nodes().size(), false );
        mGraph->reachFrom( nodes()[owner].calls, reached, false );
        if ( reached[owner] )
        {
          recursive = owner;
          break;
        }
      }
      if ( recursive.has_value() )
      {
        sink.add( diag::diagnostic( diag::DiagnosticId::TEMPORARY_UNDER_RECURSION )
                      .at( span.begin, span.length )
                      .arg( "section", mGraph->nameOf( node ) )
                      .arg( "owner", mGraph->nameOf( nodes()[*recursive] ) ) );
        continue;
      }

      Candidate candidate{ .temporary = temporary, .below = std::vector<bool>( nodes().size(), false ) };
      mGraph->reachFrom( callSeeds[temporary], candidate.below );
      candidates.push_back( std::move( candidate ) );
    }

    Interference interference;
    for ( std::size_t a = 0; a < candidates.size(); ++a )
    {
      for ( std::size_t b = a + 1; b < candidates.size(); ++b )
      {
        // Whether nothing `deeper` is live in lies below a Point `top` is
        // live at.
        auto const apart = [&inSections]( Candidate const& top, Candidate const& deeper )
        {
          for ( std::size_t index = 0; index < top.below.size(); ++index )
          {
            if ( top.below[index] && inSections[deeper.temporary][index] )
            {
              return false;
            }
          }
          return true;
        };
        Candidate const& one = candidates[a];
        Candidate const& other = candidates[b];
        if ( !together[( one.temporary * count ) + other.temporary] && apart( one, other ) && apart( other, one ) )
        {
          interference.allow( nodes()[mTemporaries[one.temporary]].where,
                              nodes()[mTemporaries[other.temporary]].where );
        }
      }
    }
    return interference;
  }

private:
  /// One bit per byte of a Temporary the program names, and one more per
  /// Temporary for a byte it does not say which of: `buf,x` reads that one,
  /// and a write to it kills nothing. A Temporary's bits are contiguous, so
  /// that whether any byte of it is live is one look.
  void assignBits()
  {
    for ( std::uint32_t index = 0; index < nodes().size(); ++index )
    {
      if ( nodes()[index].temporary && mBuild->reachable().includes( nodes()[index].where ) )
      {
        mTemporaryIndex[index] = static_cast<std::uint32_t>( mTemporaries.size() );
        mTemporaries.push_back( index );
      }
    }
    std::vector<std::vector<std::int64_t>> offsets( mTemporaries.size() );
    for ( Node const& node : nodes() )
    {
      for ( Point const& point : node.points )
      {
        for ( Access const& access : point.accesses )
        {
          if ( access.offset.has_value() )
          {
            offsets[mTemporaryIndex.at( access.temporary )].push_back( *access.offset );
          }
        }
      }
    }
    mFirstBit.assign( mTemporaries.size(), 0 );
    mBitCount.assign( mTemporaries.size(), 0 );
    mOffsets.assign( mTemporaries.size(), {} );
    std::size_t total = 0;
    for ( std::uint32_t temporary = 0; temporary < mTemporaries.size(); ++temporary )
    {
      std::vector<std::int64_t>& known = offsets[temporary];
      std::ranges::sort( known );
      auto const [first, last] = std::ranges::unique( known );
      known.erase( first, last );
      mOffsets[temporary] = known;
      mFirstBit[temporary] = total;
      mBitCount[temporary] = known.size() + 1;
      total += mBitCount[temporary];
    }
    mTotalBits = total;

    for ( Node& node : nodes() )
    {
      for ( Point& point : node.points )
      {
        point.uses = BitSet{ total };
        point.defs = BitSet{ total };
        point.touchedBits = BitSet{ total };
        point.liveIn = BitSet{ total };
        point.liveOut = BitSet{ total };
        for ( Access const& access : point.accesses )
        {
          std::uint32_t const temporary = mTemporaryIndex.at( access.temporary );
          point.touched.push_back( temporary );
          if ( access.offset.has_value() )
          {
            std::size_t const bit = bitOf( temporary, *access.offset );
            if ( access.reads )
            {
              point.uses.set( bit );
            }
            if ( access.writes )
            {
              point.defs.set( bit );
            }
            point.touchedBits.set( bit );
            continue;
          }
          // No byte in particular: a read needs every byte, a write may
          // have hit any and so kills none.
          for ( std::size_t bit = mFirstBit[temporary]; bit < mFirstBit[temporary] + mBitCount[temporary]; ++bit )
          {
            if ( access.reads )
            {
              point.uses.set( bit );
            }
            point.touchedBits.set( bit );
          }
        }
        // A Temporary reached through an address this Section holds is
        // touched wherever the pointer may be used, which is anywhere.
        for ( std::uint32_t const owned : node.ownedTemporaries )
        {
          std::uint32_t const temporary = mTemporaryIndex.at( owned );
          point.touched.push_back( temporary );
          for ( std::size_t bit = mFirstBit[temporary]; bit < mFirstBit[temporary] + mBitCount[temporary]; ++bit )
          {
            point.uses.set( bit );
            point.touchedBits.set( bit );
          }
        }
        dedupe( point.touched );
      }
      // A Section whose flow is not known touches, at every Point, what it
      // touches at any.
      if ( node.ownsSelf )
      {
        std::vector<std::uint32_t> all;
        for ( Point const& point : node.points )
        {
          all.insert( all.end(), point.touched.begin(), point.touched.end() );
        }
        dedupe( all );
        for ( Point& point : node.points )
        {
          point.touched = all;
        }
      }
    }
  }

  [[nodiscard]] std::size_t bitOf( std::uint32_t temporary, std::int64_t offset ) const
  {
    std::vector<std::int64_t> const& known = mOffsets[temporary];
    auto const found = std::ranges::lower_bound( known, offset );
    return mFirstBit[temporary] + static_cast<std::size_t>( found - known.begin() );
  }

  /// What is live into a position of a Section: nothing for the position
  /// after the last Chunk, and nothing for a Section with no Points.
  [[nodiscard]] BitSet const& liveInAt( Landing landing ) const
  {
    Node const& node = nodes()[landing.node];
    if ( landing.point >= node.points.size() )
    {
      return mEmpty;
    }
    return node.points[landing.point].liveIn;
  }

  /// Liveness backwards over every Section's Points, to a fixed point over
  /// the whole program: a byte is live into a Point when the Point reads
  /// it, when what the Point transfers to needs it on entry, or when it is
  /// live out of the Point and the Point does not write it whole. What a
  /// Call needs on entry is what its target has live into its first Point,
  /// which is why the Sections are solved together and until nothing moves.
  ///
  /// A Section that owns an address of a position in itself jumps somewhere
  /// among its own Points that they do not say, so its flow is not known:
  /// everything it reads, and everything anything it goes to needs, is live
  /// at every Point of it, and a write kills nothing.
  void solveLiveness()
  {
    mEmpty = BitSet{ mTotalBits };
    bool changed = true;
    while ( changed )
    {
      changed = false;
      for ( Node& node : nodes() )
      {
        if ( node.points.empty() )
        {
          continue;
        }
        if ( node.ownsSelf )
        {
          BitSet everywhere{ mTotalBits };
          for ( Point const& point : node.points )
          {
            everywhere.unite( point.uses );
            for ( Landing const& landing : point.transfers )
            {
              everywhere.unite( liveInAt( landing ) );
            }
          }
          for ( std::vector<std::uint32_t> const* edges : { &node.calls, &node.jumps } )
          {
            for ( std::uint32_t const target : *edges )
            {
              everywhere.unite( liveInAt( Landing{ .node = target, .point = 0 } ) );
            }
          }
          for ( Point& point : node.points )
          {
            if ( point.liveIn != everywhere )
            {
              point.liveIn = everywhere;
              point.liveOut = everywhere;
              changed = true;
            }
          }
          continue;
        }
        bool local = true;
        while ( local )
        {
          local = false;
          for ( std::size_t at = node.points.size(); at > 0; --at )
          {
            Point& point = node.points[at - 1];
            BitSet out{ mTotalBits };
            for ( std::uint32_t const successor : point.successors )
            {
              out.unite( node.points[successor].liveIn );
            }
            BitSet in = out;
            in.subtract( point.defs );
            in.unite( point.uses );
            for ( Landing const& landing : point.transfers )
            {
              in.unite( liveInAt( landing ) );
            }
            if ( in != point.liveIn || out != point.liveOut )
            {
              point.liveIn = std::move( in );
              point.liveOut = std::move( out );
              local = true;
              changed = true;
            }
          }
        }
      }
    }
  }

  /// Whether a Temporary is live at a Point: some byte of it live into or
  /// out of it, or touched by it — a write to a dead byte still lands.
  [[nodiscard]] bool isLiveAt( std::uint32_t temporary, Point const& point ) const
  {
    return point.liveIn.anyIn( mFirstBit[temporary], mBitCount[temporary] ) ||
           point.liveOut.anyIn( mFirstBit[temporary], mBitCount[temporary] ) ||
           std::ranges::find( point.touched, temporary ) != point.touched.end();
  }

  [[nodiscard]] std::vector<Node>& nodes() const
  {
    return mGraph->nodes();
  }

  Pruned const* mBuild;
  Graph* mGraph;

  /// The reachable Temporaries, by node index, and their bits.
  std::vector<std::uint32_t> mTemporaries;
  std::unordered_map<std::uint32_t, std::uint32_t> mTemporaryIndex;
  std::vector<std::size_t> mFirstBit;
  std::vector<std::size_t> mBitCount;
  std::vector<std::vector<std::int64_t>> mOffsets;
  std::size_t mTotalBits = 0;
  BitSet mEmpty;
};

} // namespace

Interference interferenceOf( Graph& graph, std::vector<bool> const& always, diag::DiagnosticSink& sink )
{
  return Liveness{ graph }.interfere( always, sink );
}

} // namespace nga::model::flow
