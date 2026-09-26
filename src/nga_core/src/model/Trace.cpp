#include "nga/model/Trace.hpp"

#include "nga/model/Evaluate.hpp"
#include "nga/model/Isa.hpp"
#include "nga/model/Prune.hpp"
#include "nga/model/References.hpp"
#include "nga/model/Transition.hpp"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace nga::model
{

void Interference::allow( SectionRef a, SectionRef b )
{
  mAllowed.insert( keyOf( a, b ) );
}

bool Interference::mayShare( SectionRef a, SectionRef b ) const
{
  return mAllowed.contains( keyOf( a, b ) );
}

std::pair<std::uint64_t, std::uint64_t> Interference::keyOf( SectionRef a, SectionRef b )
{
  auto const flat = []( SectionRef where )
  { return ( static_cast<std::uint64_t>( where.module.value ) << 32U ) | where.section.value; };
  std::uint64_t const one = flat( a );
  std::uint64_t const two = flat( b );
  return one < two ? std::pair{ one, two } : std::pair{ two, one };
}

namespace
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
std::optional<Position> positionOf( GlobalSymbols const& symbols, SymbolRef where )
{
  Symbol const& symbol = symbols.at( where );
  if ( auto const* const label = std::get_if<LabelPosition>( &symbol.value ); label != nullptr )
  {
    return Position{ .where = SectionRef{ .module = where.module, .section = label->section },
                     .chunk = label->chunk,
                     .inner = label->inner };
  }
  return std::nullopt;
}

/// What one Chunk names, gathered before it is read as edges: the walk
/// answers per name, and an indirect jump through a Slot is decided from
/// whether a Slot was among the names.
class Names final : public ReferenceVisitor
{
public:
  explicit Names( GlobalSymbols const& symbols, ModuleIndex from ) : mSymbols( &symbols ), mFrom( from ) {}

  void reference( SymbolRef target ) override
  {
    if ( mSymbols->at( target ).kind == SymbolKind::SLOT )
    {
      for ( ResolvedImplementation const& implementation : mSymbols->implementationsOf( target ) )
      {
        if ( std::optional<Position> const position = positionOf( *mSymbols, implementation.target );
             position.has_value() )
        {
          throughSlot.push_back( *position );
        }
      }
      return;
    }
    if ( std::optional<Position> const position = positionOf( *mSymbols, target ); position.has_value() )
    {
      direct.push_back( *position );
    }
  }

  void localReference( LabelPosition target ) override
  {
    direct.push_back( Position{ .where = SectionRef{ .module = mFrom, .section = target.section },
                                .chunk = target.chunk,
                                .inner = target.inner } );
  }

  std::vector<Position> direct;
  std::vector<Position> throughSlot;

private:
  GlobalSymbols const* mSymbols;
  ModuleIndex mFrom;
};

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

/// A source that gives one Section an address and nothing else: what an
/// operand is evaluated against, twice, to find how far into a Temporary it
/// reaches. See `Tracer::offsetWithin`.
class Anchored final : public ValueSource
{
public:
  Anchored( SectionRef where, std::int64_t address ) : mWhere( where ), mAddress( address ) {}

  [[nodiscard]] std::optional<std::int64_t> addressOf( SectionRef where ) override
  {
    return where == mWhere ? std::optional{ mAddress } : std::nullopt;
  }

  [[nodiscard]] std::optional<std::int64_t>
  offsetOf( SectionRef where, ChunkIndex chunk, std::optional<std::uint32_t> inner ) override
  {
    // The Label at the start alone: a position further in needs the size
    // of what stands before it, which is Size's and not known here.
    return where == mWhere && chunk.value == 0 && !inner.has_value() ? std::optional{ std::int64_t{ 0 } }
                                                                     : std::nullopt;
  }

private:
  SectionRef mWhere;
  std::int64_t mAddress;
};

class Tracer
{
public:
  explicit Tracer( Pruned const& build ) : mBuild( &build )
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

  Interference run( diag::DiagnosticSink& sink )
  {
    mSink = &sink;
    // The routine's tail jumps through the entry the Frame hands it: the
    // `.transition` statements are its takings, and it their follower, by
    // construction and whether or not one of them is reachable. The cold start
    // is one more taking of the same jump.
    if ( std::optional<SymbolRef> const routine = mBuild->symbols().find( TRANSITION_ENTER_NAME ); routine.has_value() )
    {
      if ( std::optional<Position> const position = positionOf( mBuild->symbols(), *routine ); position.has_value() )
      {
        mNodes[indexOf( position->where )].ownsCode = true;
      }
    }
    // Whether a Section is code is decided before any edge is read, since an
    // escape into it is read from another Section, which may come first.
    for ( Node& node : mNodes )
    {
      Section const& section = mBuild->symbols().moduleAt( node.where.module ).sectionAt( node.where.section );
      auto const isInstruction = []( Chunk const& chunk )
      { return std::holds_alternative<InstructionContent>( chunk.content ); };
      node.code = false;
      for ( std::uint32_t index = 0; index < section.chunks().size(); ++index )
      {
        node.code = node.code || isInstruction( section.chunks()[index] ) ||
                    std::ranges::any_of( section.innerChunksOf( ChunkIndex{ index } ), isInstruction );
      }
    }
    for ( Node& node : mNodes )
    {
      if ( mBuild->reachable().includes( node.where ) )
      {
        readEdgesOf( node );
      }
    }

    // A Section that jumps through a pointer and owns no address of code:
    // nothing said where the jump goes, and a jump to nowhere is the one
    // thing the model must not believe — see docs/decisions/0061-root-at-the-taking.md.
    for ( Node const& node : mNodes )
    {
      if ( node.indirectJumpAt.has_value() && !node.ownsCode )
      {
        sink.add( diag::diagnostic( diag::DiagnosticId::OWN_JUMP_FOLLOWS_NOTHING )
                      .at( node.indirectJumpAt->begin, node.indirectJumpAt->length )
                      .arg( "section", nameOf( node ) ) );
      }
    }
    for ( Node& node : mNodes )
    {
      dedupe( node.calls );
      dedupe( node.jumps );
      dedupe( node.enters );
      dedupe( node.owners );
      dedupe( node.ownedJumps );
      dedupe( node.ownedTemporaries );
    }

    resolveSuccessors();
    assignBits();
    solveLiveness();

    // A `root` Section is active in every state — the hardware enters it
    // whenever it likes — and so is everything it reaches.
    std::vector<bool> always( mNodes.size(), false );
    for ( std::uint32_t index = 0; index < mNodes.size(); ++index )
    {
      Node const& node = mNodes[index];
      if ( mBuild->reachable().includes( node.where ) &&
           mBuild->symbols().moduleAt( node.where.module ).sectionAt( node.where.section ).isRoot() )
      {
        always[index] = true;
        reachFrom( mNodes[index].calls, always );
        reachFrom( mNodes[index].jumps, always );
      }
    }

    // Per Temporary, from the Points it is live at: the Sections it is live
    // in, the Sections that may be below one of those Points — everything
    // a Call from it reaches — and which other Temporaries share a Point
    // with it.
    std::size_t const count = mTemporaries.size();
    std::vector<std::vector<bool>> inSections( count, std::vector<bool>( mNodes.size(), false ) );
    std::vector<std::vector<std::uint32_t>> callSeeds( count );
    std::vector<bool> together( count * count, false );
    std::vector<std::uint32_t> present;
    for ( std::uint32_t index = 0; index < mNodes.size(); ++index )
    {
      for ( Point const& point : mNodes[index].points )
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
      Node const& node = mNodes[mTemporaries[temporary]];
      diag::SourceSpan const span = spanOf( node );
      if ( node.temporaryEscaped )
      {
        // At the taking, since that is what to change; a Temporary has no
        // address to hand out — see docs/decisions/0058-temporary-is-an-attribute.md.
        diag::SourceSpan const at = node.temporaryEscapedAt.value_or( span );
        sink.add( diag::diagnostic( diag::DiagnosticId::TEMPORARY_ESCAPED )
                      .at( at.begin, at.length )
                      .arg( "section", nameOf( node ) ) );
        continue;
      }
      bool underRoot = false;
      for ( std::uint32_t index = 0; index < mNodes.size(); ++index )
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
        std::vector<bool> reached( mNodes.size(), false );
        reachFrom( mNodes[owner].calls, reached, false );
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
                      .arg( "section", nameOf( node ) )
                      .arg( "owner", nameOf( mNodes[*recursive] ) ) );
        continue;
      }

      Candidate candidate{ .temporary = temporary, .below = std::vector<bool>( mNodes.size(), false ) };
      reachFrom( callSeeds[temporary], candidate.below );
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
          interference.allow( mNodes[mTemporaries[one.temporary]].where, mNodes[mTemporaries[other.temporary]].where );
        }
      }
    }
    return interference;
  }

private:
  [[nodiscard]] std::uint32_t indexOf( SectionRef where ) const
  {
    return mFirstOf[where.module.value] + where.section.value;
  }

  [[nodiscard]] std::string nameOf( Node const& node ) const
  {
    return mBuild->symbols().moduleAt( node.where.module ).displayNameOf( node.where.section, mBuild->sources() );
  }

  [[nodiscard]] diag::SourceSpan spanOf( Node const& node ) const
  {
    return mBuild->symbols().moduleAt( node.where.module ).sectionAt( node.where.section ).span();
  }

  static void dedupe( std::vector<std::uint32_t>& edges )
  {
    std::ranges::sort( edges );
    auto const [first, last] = std::ranges::unique( edges );
    edges.erase( first, last );
  }

  /// Everything reachable from `from` over Calls and Jumps alike, marked in
  /// `reached`; what is already marked is not walked again.
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

  void edge( Node& from, SectionRef to, ReferenceKind kind )
  {
    std::uint32_t const target = indexOf( to );
    Node& into = mNodes[target];
    bool const self = target == indexOf( from.where );

    // Under `.own` an address taken is followed by the taker and by nobody
    // else: a jump to code, from here alone, and a use of a Temporary, by
    // this Owner — see docs/decisions/0060-own.md. Data is neither, and
    // the statement is told so.
    // Under `.root` the hardware follows it: the Section is a Root already,
    // and no jump of the program's reaches it through this taking.
    if ( kind == ReferenceKind::ESCAPE && mReadingTaking == Taking::ROOT )
    {
      return;
    }
    if ( kind == ReferenceKind::ESCAPE && mReadingTaking == Taking::OWN )
    {
      // The followers: this Section, or the ones `.own` named.
      std::vector<std::uint32_t> followers = mFollowers;
      if ( followers.empty() )
      {
        followers.push_back( indexOf( from.where ) );
      }
      for ( std::uint32_t const follower : followers )
      {
        if ( into.temporary )
        {
          into.owners.push_back( follower );
          mNodes[follower].ownedTemporaries.push_back( target );
          mOwnDidSomething = true;
        }
        if ( into.code )
        {
          // Under bare `.own` the follower is the taker, and the taking is
          // then a Reference of its own that 0028 already holds to the
          // target's Residency; saying it twice would be noise.
          if ( follower != indexOf( from.where ) )
          {
            checkFollowerResidency( follower, target );
          }
          if ( follower != target )
          {
            mNodes[follower].jumps.push_back( target );
            mNodes[follower].ownedJumps.push_back( target );
          }
          else
          {
            mNodes[follower].ownsSelf = true;
          }
          mNodes[follower].ownsCode = true;
          mOwnDidSomething = true;
        }
      }
      return;
    }
    switch ( kind )
    {
    case ReferenceKind::CALL:
      if ( self )
      {
        from.callsItself = true;
      }
      else
      {
        from.calls.push_back( target );
      }
      return;
    case ReferenceKind::JUMP:
      if ( !self )
      {
        from.jumps.push_back( target );
      }
      return;
    case ReferenceKind::READ:
    case ReferenceKind::WRITE:
    case ReferenceKind::READ_WRITE:
      if ( into.temporary )
      {
        into.owners.push_back( indexOf( from.where ) );
      }
      // Code written at run time goes where the address it was given
      // leads, and that address was taken under `.own NAME` naming it —
      // or it was an error there.
      return;
    case ReferenceKind::ESCAPE:
      if ( into.temporary )
      {
        into.temporaryEscaped = true;
        if ( !into.temporaryEscapedAt.has_value() )
        {
          into.temporaryEscapedAt = mReading;
        }
      }
      if ( into.code && !mEscapeReported && mReading.has_value() )
      {
        // An address of code with nothing said about who follows it: there
        // is no set of "everywhere" left for it to reach, since every other
        // taking says, so it is refused rather than let loose — see
        // docs/decisions/0061-root-at-the-taking.md.
        mSink->add( diag::diagnostic( diag::DiagnosticId::CODE_ADDRESS_ESCAPES )
                        .at( mReading->begin, mReading->length )
                        .arg( "section", nameOf( into ) ) );
        mEscapeReported = true;
      }
      return;
    }
  }

  /// A follower is present only where what it follows is: the address it was
  /// handed leads into code, and in a Phase that holds the follower and not
  /// the code a jump through the pointer would land in nothing. 0028's rule,
  /// asked of the jump edge `.own` declares rather than of a Reference — see
  /// docs/decisions/0066-a-follower-is-present-where-its-target-is.md. Once
  /// per taking and follower, for the first Phase in declaration order.
  void checkFollowerResidency( std::uint32_t follower, std::uint32_t target )
  {
    if ( std::ranges::find( mResidencyReported, follower ) != mResidencyReported.end() )
    {
      return;
    }
    GlobalSymbols const& symbols = mBuild->symbols();
    Residency const& following = symbols.moduleAt( mNodes[follower].where.module ).residency();
    Residency const& followed = symbols.moduleAt( mNodes[target].where.module ).residency();
    for ( std::uint32_t index = 0; index < following.phaseCount(); ++index )
    {
      PhaseIndex const phase{ index };
      if ( !following.includes( phase ) || followed.includes( phase ) )
      {
        continue;
      }
      mResidencyReported.push_back( follower );
      diag::SourceSpan const at = mReading.value_or( spanOf( mNodes[target] ) );
      mSink->add( diag::diagnostic( diag::DiagnosticId::OWN_FOLLOWER_OUTLIVES_TARGET )
                      .at( at.begin, at.length )
                      .arg( "section", nameOf( mNodes[target] ) )
                      .arg( "follower", nameOf( mNodes[follower] ) )
                      .arg( "phase", mBuild->phases().phases[index].name.value_or( "(implicit)" ) ) );
      return;
    }
  }

  /// How far into a Temporary an operand reaches, when the operand says:
  /// `ptr + 1` is one byte in, `ptr + OFFSET` with OFFSET declared is
  /// whatever it is, and `buf,x` says nothing. Found by evaluating the
  /// operand against a source that gives the Temporary an address and
  /// nothing else, twice, so that a shape the address does not pass through
  /// whole — `ptr * 2`, `<ptr` — is told from an offset.
  [[nodiscard]] std::optional<std::int64_t>
  offsetWithin( ModuleIndex home, SectionRef temporary, syntax::Expression const& operand ) const
  {
    constexpr std::int64_t shift = 0x10000;
    Anchored low{ temporary, 0 };
    Anchored high{ temporary, shift };
    GlobalSymbols const& symbols = mBuild->symbols();
    std::optional<std::int64_t> const atLow =
        evaluate( mBuild->sources(), symbols, &mBuild->charsets(), home, operand, low );
    std::optional<std::int64_t> const atHigh =
        evaluate( mBuild->sources(), symbols, &mBuild->charsets(), home, operand, high );
    if ( !atLow.has_value() || !atHigh.has_value() || *atHigh - *atLow != shift || *atLow < 0 || *atLow >= shift )
    {
      return std::nullopt;
    }
    return atLow;
  }

  /// The bytes of a Temporary an instruction touches through an operand
  /// that reaches `offset` bytes into it: the one byte a direct operand
  /// names, the two of a pointer read through, and no byte in particular
  /// where an index the tool does not know is added.
  static std::vector<std::optional<std::int64_t>> bytesOf( syntax::OperandShape shape,
                                                           std::optional<std::int64_t> offset )
  {
    switch ( shape )
    {
    case syntax::OperandShape::DIRECT:
      return { offset };
    case syntax::OperandShape::INDIRECT:
    case syntax::OperandShape::INDIRECT_Y:
      if ( offset.has_value() )
      {
        return { offset, std::optional{ *offset + 1 } };
      }
      return { std::nullopt };
    case syntax::OperandShape::NONE:
    case syntax::OperandShape::IMMEDIATE:
    case syntax::OperandShape::DIRECT_X:
    case syntax::OperandShape::DIRECT_Y:
    case syntax::OperandShape::INDEXED_INDIRECT:
      break;
    }
    return { std::nullopt };
  }

  /// What one name a Chunk reaches means to the Chunk's Point: a Call or a
  /// Jump goes there, and a read or a write of a Temporary touches bytes of
  /// it — the ones the operand names when it was written directly, and no
  /// byte in particular when the name came through a Slot's Cell.
  void notePoint( Node const& node,
                  Point& point,
                  Section const& section,
                  Chunk const& chunk,
                  Position const& target,
                  ReferenceKind kind,
                  bool direct )
  {
    std::uint32_t const index = indexOf( target.where );
    switch ( kind )
    {
    case ReferenceKind::CALL:
      point.targets.push_back( target );
      point.calls = true;
      return;
    case ReferenceKind::JUMP:
      point.targets.push_back( target );
      return;
    case ReferenceKind::READ:
    case ReferenceKind::WRITE:
    case ReferenceKind::READ_WRITE:
    {
      if ( !mNodes[index].temporary )
      {
        return;
      }
      auto const* const instruction = std::get_if<InstructionContent>( &chunk.content );
      std::optional<std::int64_t> offset;
      syntax::OperandShape shape = syntax::OperandShape::DIRECT_X;
      if ( instruction != nullptr && direct && section.itemsOf( chunk ).size() == 1 )
      {
        shape = instruction->shape;
        offset = offsetWithin( node.where.module, target.where, *section.itemsOf( chunk ).front() );
      }
      for ( std::optional<std::int64_t> const byte : bytesOf( shape, offset ) )
      {
        point.accesses.push_back( Access{ .temporary = index,
                                          .offset = byte,
                                          .reads = kind != ReferenceKind::WRITE,
                                          .writes = kind != ReferenceKind::READ } );
      }
      return;
    }
    case ReferenceKind::ESCAPE:
      return;
    }
  }

  void readEdgesOf( Node& node )
  {
    GlobalSymbols const& symbols = mBuild->symbols();
    diag::SourceManager const& sources = mBuild->sources();
    Module const& module = symbols.moduleAt( node.where.module );
    Section const& section = module.sectionAt( node.where.section );

    // Falling through into the next Proc is a Jump the source does not
    // spell: this Activation ends where the next begins.
    if ( std::optional<SectionIndex> const next = section.next(); next.has_value() )
    {
      edge( node, SectionRef{ .module = node.where.module, .section = *next }, ReferenceKind::JUMP );
    }

    auto const readChunk = [&]( Chunk const& chunk )
    {
      Point point;
      mReading = chunk.span;
      mReadingTaking = chunk.taking;
      mOwnDidSomething = false;
      mEscapeReported = false;
      mFollowers.clear();
      mResidencyReported.clear();
      for ( syntax::Token const& name : chunk.followers )
      {
        std::string_view const text = sources.textOf( name.span() );
        std::optional<SymbolRef> const found = symbols.lookupScoped( node.where.module, section.scope(), text );
        std::optional<Position> const position = found.has_value() ? positionOf( symbols, *found ) : std::nullopt;
        if ( !position.has_value() )
        {
          mSink->add( diag::diagnostic( diag::DiagnosticId::OWN_FOLLOWER_UNKNOWN )
                          .at( name.location, name.length )
                          .arg( "name", std::string{ text } ) );
          continue;
        }
        mFollowers.push_back( indexOf( position->where ) );
      }
      ReferenceKind const kind = referenceKindOf( sources, chunk );
      auto const* const instruction = std::get_if<InstructionContent>( &chunk.content );
      bool const indirectJump = instruction != nullptr && instruction->shape == syntax::OperandShape::INDIRECT &&
                                sources.textOf( instruction->mnemonic.span() ) == "jmp";

      Names names{ symbols, node.where.module };
      for ( syntax::ExpressionPtr const& item : section.itemsOf( chunk ) )
      {
        walkReferences( symbols, sources, node.where.module, *item, names );
      }
      for ( Position const& target : names.direct )
      {
        edge( node, target.where, kind );
        notePoint( node, point, section, chunk, target, kind, true );
      }
      // A Slot is declared indirection: `jmp (slot)` goes to every
      // Implementation, and any other use touches each as the instruction
      // would touch what it named directly.
      for ( Position const& target : names.throughSlot )
      {
        ReferenceKind const through = indirectJump ? ReferenceKind::JUMP : kind;
        edge( node, target.where, through );
        notePoint( node, point, section, chunk, target, through, false );
      }
      // A jump through a pointer follows what this Section owns, which are
      // edges already; one in a Section that owns nothing is reported once
      // every taking has been read.
      if ( indirectJump && names.throughSlot.empty() && !node.indirectJumpAt.has_value() )
      {
        node.indirectJumpAt = chunk.span;
      }
      if ( chunk.taking == Taking::OWN && !mOwnDidSomething )
      {
        mSink->add(
            diag::diagnostic( diag::DiagnosticId::OWN_DOES_NOTHING ).at( chunk.span.begin, chunk.span.length ) );
      }

      if ( instruction != nullptr )
      {
        bool const unnamed = names.direct.empty() && names.throughSlot.empty();
        switch ( flowOf( sources.textOf( instruction->mnemonic.span() ) ) )
        {
        case ControlFlow::JUMP:
          point.fallsThrough = false;
          point.loose = unnamed;
          break;
        case ControlFlow::RETURN:
          point.fallsThrough = false;
          point.returns = true;
          break;
        case ControlFlow::BRANCH:
          point.loose = unnamed;
          break;
        case ControlFlow::NEXT:
        case ControlFlow::CALL:
          break;
        }
      }

      if ( std::holds_alternative<DispatchContent>( chunk.content ) )
      {
        // Control goes to one of the positions the statement names and to
        // nothing else, so its Successors are those and nothing follows it.
        // The targets are References of kind JUMP already, which is what
        // every other jump within a Section is — see
        // docs/decisions/0191-a-dispatch-goes-to-one-of-its-own-positions.md.
        point.fallsThrough = false;
      }

      if ( auto const* const transition = std::get_if<TransitionContent>( &chunk.content ); transition != nullptr )
      {
        // A `.transition` goes to the routine and to the entry of the Phase
        // it enters, and nothing of this Section runs after it.
        point.fallsThrough = false;
        if ( std::optional<SymbolRef> const routine = symbols.find( TRANSITION_ROUTINE_NAME ); routine.has_value() )
        {
          if ( std::optional<Position> const position = positionOf( symbols, *routine ); position.has_value() )
          {
            edge( node, position->where, ReferenceKind::JUMP );
            point.targets.push_back( *position );
          }
        }
        if ( transition->target.has_value() )
        {
          if ( std::optional<SymbolRef> const entry = entrySymbolOf( mBuild->phases(), *transition->target, symbols );
               entry.has_value() )
          {
            if ( std::optional<Position> const position = positionOf( symbols, *entry ); position.has_value() )
            {
              // The entry of the Phase entered, as what it is: the end of
              // every activation on the stack, not a jump this one makes.
              std::uint32_t const target = indexOf( position->where );
              if ( target != indexOf( node.where ) )
              {
                node.enters.push_back( target );
              }
              point.targets.push_back( *position );
            }
          }
        }
      }
      node.points.push_back( std::move( point ) );
    };

    // A macro use's edges are its expansion's; its own items are the
    // arguments, which stand cloned inside it.
    for ( std::uint32_t index = 0; index < section.chunks().size(); ++index )
    {
      node.firstOf.push_back( static_cast<std::uint32_t>( node.points.size() ) );
      Chunk const& chunk = section.chunks()[index];
      if ( std::holds_alternative<MacroUseContent>( chunk.content ) )
      {
        for ( Chunk const& inner : section.innerChunksOf( ChunkIndex{ index } ) )
        {
          readChunk( inner );
        }
        continue;
      }
      readChunk( chunk );
    }
    node.firstOf.push_back( static_cast<std::uint32_t>( node.points.size() ) );
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

  /// Every Point's Successors and transfers, from the positions it goes to,
  /// once every Section has its Points: a position in this Section is a
  /// Successor, one in another the frame that runs next, and one under a
  /// Call — into any Section, this one included — the frame that runs
  /// below this one. What falls off the end goes where `then` said.
  void resolveSuccessors()
  {
    for ( std::uint32_t index = 0; index < mNodes.size(); ++index )
    {
      Node& node = mNodes[index];
      Section const& section = mBuild->symbols().moduleAt( node.where.module ).sectionAt( node.where.section );
      std::optional<Landing> const after =
          section.next().has_value()
              ? std::optional{ Landing{
                    .node = indexOf( SectionRef{ .module = node.where.module, .section = *section.next() } ),
                    .point = 0 } }
              : std::nullopt;
      auto const count = static_cast<std::uint32_t>( node.points.size() );
      for ( std::uint32_t at = 0; at < count; ++at )
      {
        Point& point = node.points[at];
        if ( point.fallsThrough )
        {
          if ( at + 1 < count )
          {
            point.successors.push_back( at + 1 );
          }
          else if ( after.has_value() )
          {
            point.transfers.push_back( *after );
          }
        }
        for ( Position const& target : point.targets )
        {
          std::uint32_t const into = indexOf( target.where );
          std::uint32_t const landing = pointIndexOf( mNodes[into], target );
          if ( point.calls )
          {
            point.transfers.push_back( Landing{ .node = into, .point = landing } );
            point.callTargets.push_back( into );
          }
          else if ( into == index && landing < count )
          {
            point.successors.push_back( landing );
          }
          else if ( into == index && after.has_value() )
          {
            point.transfers.push_back( *after );
          }
          else if ( into != index )
          {
            point.transfers.push_back( Landing{ .node = into, .point = landing } );
          }
        }
        dedupe( point.successors );
        dedupe( point.callTargets );
      }

      // Where this Section follows the addresses it owns: at its jumps that
      // name no target, and, when it has none — `pha`, `pha`, `rts` — at
      // its returns.
      if ( !node.ownedJumps.empty() )
      {
        bool const anyLoose = std::ranges::any_of( node.points, []( Point const& point ) { return point.loose; } );
        for ( Point& point : node.points )
        {
          if ( anyLoose ? point.loose : point.returns )
          {
            for ( std::uint32_t const owned : node.ownedJumps )
            {
              point.transfers.push_back( Landing{ .node = owned, .point = 0 } );
            }
          }
        }
      }
    }
  }

  /// One bit per byte of a Temporary the program names, and one more per
  /// Temporary for a byte it does not say which of: `buf,x` reads that one,
  /// and a write to it kills nothing. A Temporary's bits are contiguous, so
  /// that whether any byte of it is live is one look.
  void assignBits()
  {
    for ( std::uint32_t index = 0; index < mNodes.size(); ++index )
    {
      if ( mNodes[index].temporary && mBuild->reachable().includes( mNodes[index].where ) )
      {
        mTemporaryIndex[index] = static_cast<std::uint32_t>( mTemporaries.size() );
        mTemporaries.push_back( index );
      }
    }
    std::vector<std::vector<std::int64_t>> offsets( mTemporaries.size() );
    for ( Node const& node : mNodes )
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

    for ( Node& node : mNodes )
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
    Node const& node = mNodes[landing.node];
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
      for ( Node& node : mNodes )
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

  Pruned const* mBuild;

  /// The Chunk whose References are being read, for where an escape is;
  /// whether it stands under `.own`, and whether that changed anything.
  std::optional<diag::SourceSpan> mReading;
  Taking mReadingTaking = Taking::ESCAPE;
  std::vector<std::uint32_t> mFollowers;

  /// The followers this taking has already been reported for, so that a
  /// statement handing several addresses to one follower says it once.
  std::vector<std::uint32_t> mResidencyReported;
  bool mOwnDidSomething = false;
  bool mEscapeReported = false;
  diag::DiagnosticSink* mSink = nullptr;
  std::vector<std::uint32_t> mFirstOf;
  std::vector<Node> mNodes;

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

Interference trace( Pruned const& build, diag::DiagnosticSink& sink )
{
  Tracer tracer{ build };
  return tracer.run( sink );
}

} // namespace nga::model
