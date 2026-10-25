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

void dedupe( std::vector<std::uint32_t>& edges )
{
  std::ranges::sort( edges );
  auto const [first, last] = std::ranges::unique( edges );
  edges.erase( first, last );
}

namespace
{

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
      directSymbols.emplace_back( target );
      directSites.push_back( site );
    }
  }

  void localReference( LabelPosition target ) override
  {
    direct.push_back( Position{ .where = SectionRef{ .module = mFrom, .section = target.section },
                                .chunk = target.chunk,
                                .inner = target.inner } );
    directSymbols.emplace_back( std::nullopt );
    directSites.push_back( site );
  }

  std::vector<Position> direct;
  std::vector<Position> throughSlot;

  /// Beside each of `direct`: the Symbol it was reached by, where a name and
  /// not a local label reached it, and the expression it stands in, which
  /// whoever walks sets before each walk.
  std::vector<std::optional<SymbolRef>> directSymbols;
  std::vector<syntax::Expression const*> directSites;
  syntax::Expression const* site = nullptr;

private:
  GlobalSymbols const* mSymbols;
  ModuleIndex mFrom;
};

/// Reads the graph off the program: which Sections are code, every Chunk's
/// References as edges and Points, and then each Point's Successors.
class Reader
{
public:
  Reader( Pruned const& build, diag::DiagnosticSink& sink ) : mBuild( &build ), mSink( &sink ), mGraph( build ) {}

  Graph read()
  {
    // The routine's tail jumps through the entry the Frame hands it: the
    // `.transition` statements are its takings, and it their follower, by
    // construction and whether or not one of them is reachable. The cold start
    // is one more taking of the same jump.
    if ( std::optional<SymbolRef> const routine = mBuild->symbols().find( TRANSITION_ENTER_NAME ); routine.has_value() )
    {
      if ( std::optional<Position> const position = positionOf( mBuild->symbols(), *routine ); position.has_value() )
      {
        nodes()[mGraph.indexOf( position->where )].ownsCode = true;
      }
    }
    // Whether a Section is code is decided before any edge is read, since an
    // escape into it is read from another Section, which may come first.
    for ( Node& node : nodes() )
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
    for ( Node& node : nodes() )
    {
      if ( mBuild->reachable().includes( node.where ) )
      {
        readEdgesOf( node );
      }
    }

    // A Section that jumps through a pointer and owns no address of code:
    // nothing said where the jump goes, and a jump to nowhere is the one
    // thing the model must not believe — see docs/decisions/0061-root-at-the-taking.md.
    for ( Node const& node : nodes() )
    {
      if ( node.indirectJumpAt.has_value() && !node.ownsCode )
      {
        mSink->add( diag::diagnostic( diag::DiagnosticId::OWN_JUMP_FOLLOWS_NOTHING )
                        .at( node.indirectJumpAt->begin, node.indirectJumpAt->length )
                        .arg( "section", mGraph.nameOf( node ) ) );
      }
    }
    for ( Node& node : nodes() )
    {
      dedupe( node.calls );
      dedupe( node.jumps );
      dedupe( node.enters );
      dedupe( node.owners );
      dedupe( node.ownedJumps );
      dedupe( node.ownedTemporaries );
    }

    resolveSuccessors();
    return std::move( mGraph );
  }

private:
  std::vector<Node>& nodes()
  {
    return mGraph.nodes();
  }

  /// One Chunk whose References are being read: where it stands, for where
  /// an escape is; how it takes what it names, and who follows under `.own`;
  /// and what the taking has said and done so far, so that each finding is
  /// said once and `.own` that changed nothing is told so.
  struct Reading
  {
    std::optional<diag::SourceSpan> span{};
    syntax::Taking taking = syntax::Taking::ESCAPE;
    std::vector<std::uint32_t> followers{};

    /// The followers this taking has already been reported for, so that a
    /// statement handing several addresses to one follower says it once.
    std::vector<std::uint32_t> residencyReported{};
    bool ownDidSomething = false;
    bool escapeReported = false;
  };

  void edge( Node& from, SectionRef to, ReferenceKind kind, Reading& reading )
  {
    std::uint32_t const target = mGraph.indexOf( to );
    Node& into = nodes()[target];
    bool const self = target == mGraph.indexOf( from.where );

    // Under `.own` an address taken is followed by the taker and by nobody
    // else: a jump to code, from here alone, and a use of a Temporary, by
    // this Owner — see docs/decisions/0060-own.md. Data is neither, and
    // the statement is told so.
    // Under `.root` the hardware follows it: the Section is a Root already,
    // and no jump of the program's reaches it through this taking.
    if ( kind == ReferenceKind::ESCAPE && reading.taking == syntax::Taking::ROOT )
    {
      return;
    }
    if ( kind == ReferenceKind::ESCAPE && reading.taking == syntax::Taking::OWN )
    {
      // The followers: this Section, or the ones `.own` named.
      std::vector<std::uint32_t> followers = reading.followers;
      if ( followers.empty() )
      {
        followers.push_back( mGraph.indexOf( from.where ) );
      }
      for ( std::uint32_t const follower : followers )
      {
        if ( into.temporary )
        {
          into.owners.push_back( follower );
          nodes()[follower].ownedTemporaries.push_back( target );
          reading.ownDidSomething = true;
        }
        if ( into.code )
        {
          // Under bare `.own` the follower is the taker, and the taking is
          // then a Reference of its own that 0028 already holds to the
          // target's Residency; saying it twice would be noise.
          if ( follower != mGraph.indexOf( from.where ) )
          {
            checkFollowerResidency( follower, target, reading );
          }
          if ( follower != target )
          {
            nodes()[follower].jumps.push_back( target );
            nodes()[follower].ownedJumps.push_back( target );
          }
          else
          {
            nodes()[follower].ownsSelf = true;
          }
          nodes()[follower].ownsCode = true;
          reading.ownDidSomething = true;
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
        into.owners.push_back( mGraph.indexOf( from.where ) );
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
          into.temporaryEscapedAt = reading.span;
        }
      }
      if ( into.code && !reading.escapeReported && reading.span.has_value() )
      {
        // An address of code with nothing said about who follows it: there
        // is no set of "everywhere" left for it to reach, since every other
        // taking says, so it is refused rather than let loose — see
        // docs/decisions/0061-root-at-the-taking.md.
        mSink->add( diag::diagnostic( diag::DiagnosticId::CODE_ADDRESS_ESCAPES )
                        .at( reading.span->begin, reading.span->length )
                        .arg( "section", mGraph.nameOf( into ) ) );
        reading.escapeReported = true;
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
  void checkFollowerResidency( std::uint32_t follower, std::uint32_t target, Reading& reading )
  {
    if ( std::ranges::find( reading.residencyReported, follower ) != reading.residencyReported.end() )
    {
      return;
    }
    GlobalSymbols const& symbols = mBuild->symbols();
    Residency const& following = symbols.moduleAt( nodes()[follower].where.module ).residency();
    Residency const& followed = symbols.moduleAt( nodes()[target].where.module ).residency();
    for ( std::uint32_t index = 0; index < following.phaseCount(); ++index )
    {
      PhaseIndex const phase{ index };
      if ( !following.includes( phase ) || followed.includes( phase ) )
      {
        continue;
      }
      reading.residencyReported.push_back( follower );
      diag::SourceSpan const at = reading.span.value_or( mGraph.spanOf( nodes()[target] ) );
      mSink->add( diag::diagnostic( diag::DiagnosticId::OWN_FOLLOWER_OUTLIVES_TARGET )
                      .at( at.begin, at.length )
                      .arg( "section", mGraph.nameOf( nodes()[target] ) )
                      .arg( "follower", mGraph.nameOf( nodes()[follower] ) )
                      .arg( "phase", mBuild->phases().phases[index].displayName() ) );
      return;
    }
  }

  /// How far into a Temporary an operand reaches, when the operand says:
  /// `ptr + 1` is one byte in, `ptr + OFFSET` with OFFSET declared is
  /// whatever it is, and `buf,x` says nothing — nor does an offset outside
  /// the address space.
  [[nodiscard]] std::optional<std::int64_t>
  offsetWithin( ModuleIndex home, SectionRef temporary, syntax::Expression const& operand ) const
  {
    std::optional<std::int64_t> const offset = offsetPast(
        mBuild->sources(), mBuild->symbols(), &mBuild->charsets(), home, operand, temporary, ChunkIndex{ 0 } );
    if ( !offset.has_value() || *offset < 0 || std::cmp_greater_equal( *offset, ADDRESS_SPACE_END ) )
    {
      return std::nullopt;
    }
    return offset;
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
    std::uint32_t const index = mGraph.indexOf( target.where );
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
      if ( !nodes()[index].temporary )
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
      Reading unread;
      edge( node, SectionRef{ .module = node.where.module, .section = *next }, ReferenceKind::JUMP, unread );
    }

    diag::SourceSpan statement;
    auto const readChunk = [&]( Chunk const& chunk )
    {
      Point point;
      point.statement = statement;
      Reading reading{ .span = chunk.span, .taking = chunk.taking };
      // `.root NAME` names a Holder, and is read below with what it names.
      for ( syntax::Token const& name :
            chunk.taking == syntax::Taking::ROOT ? std::vector<syntax::Token>{} : chunk.followers )
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
        reading.followers.push_back( mGraph.indexOf( position->where ) );
      }
      ReferenceKind const kind = referenceKindOf( sources, chunk );
      auto const* const instruction = std::get_if<InstructionContent>( &chunk.content );
      bool const indirectJump = instruction != nullptr &&
                                ( instruction->shape == syntax::OperandShape::INDIRECT ||
                                  instruction->shape == syntax::OperandShape::INDEXED_INDIRECT ) &&
                                sources.textOf( instruction->mnemonic.span() ) == "jmp";

      Names names{ symbols, node.where.module };
      for ( syntax::ExpressionPtr const& item : section.itemsOf( chunk ) )
      {
        names.site = item.get();
        walkReferences( symbols, sources, node.where.module, *item, names );
      }
      for ( std::size_t at = 0; at < names.direct.size(); ++at )
      {
        Position const& target = names.direct[at];
        // An address of data — a byte the program writes in place, or a
        // table that stands among code — taken with nothing said is read or
        // written through, and nobody jumps there — see
        // docs/decisions/0238-the-address-of-a-byte-the-program-may-write-is-data.md
        // and docs/decisions/0240-an-address-of-data-in-a-section-of-code-is-data.md.
        ReferenceKind reached = kind;
        if ( kind == ReferenceKind::ESCAPE && chunk.taking == syntax::Taking::ESCAPE &&
             names.directSymbols[at].has_value() &&
             takenAsData( *mBuild, node.where.module, *names.directSymbols[at], *names.directSites[at] ) )
        {
          reached = ReferenceKind::WRITE;
        }
        edge( node, target.where, reached, reading );
        notePoint( node, point, section, chunk, target, reached, true );
      }
      for ( Position const& target : names.direct )
      {
        node.names.push_back( mGraph.indexOf( target.where ) );
      }
      // Handed to a Holder, or a Holder emptied — see
      // docs/decisions/0247-the-hardware-holds-a-section-for-as-long-as-it-is-given-it.md.
      auto const holderOf = [&]( syntax::Token const& name ) -> std::optional<std::uint32_t>
      {
        std::string_view const text = sources.textOf( name.span() );
        std::optional<std::uint32_t> const holder = mBuild->target().holderNamed( text );
        if ( !holder.has_value() )
        {
          mSink->add( diag::diagnostic( diag::DiagnosticId::HOLDER_UNKNOWN )
                          .at( name.location, name.length )
                          .arg( "name", std::string{ text } ) );
        }
        return holder;
      };
      if ( chunk.taking == syntax::Taking::ROOT && !chunk.followers.empty() )
      {
        if ( std::optional<std::uint32_t> const holder = holderOf( chunk.followers.front() ); holder.has_value() )
        {
          for ( Position const& target : names.direct )
          {
            point.handsOver.emplace_back( *holder, mGraph.indexOf( target.where ) );
          }
        }
      }
      if ( auto const* const release = std::get_if<ReleaseContent>( &chunk.content ); release != nullptr )
      {
        point.releases = holderOf( release->holder );
      }

      // A Slot is declared indirection: `jmp (slot)` goes to every
      // Implementation, and any other use touches each as the instruction
      // would touch what it named directly.
      for ( Position const& target : names.throughSlot )
      {
        ReferenceKind const through = indirectJump ? ReferenceKind::JUMP : kind;
        edge( node, target.where, through, reading );
        notePoint( node, point, section, chunk, target, through, false );
      }
      // A jump through a pointer follows what this Section owns, which are
      // edges already; one in a Section that owns nothing is reported once
      // every taking has been read.
      if ( indirectJump && names.throughSlot.empty() && !node.indirectJumpAt.has_value() )
      {
        node.indirectJumpAt = chunk.span;
      }
      if ( chunk.taking == syntax::Taking::OWN && !reading.ownDidSomething )
      {
        mSink->add(
            diag::diagnostic( diag::DiagnosticId::OWN_DOES_NOTHING ).at( chunk.span.begin, chunk.span.length ) );
      }

      if ( instruction != nullptr )
      {
        point.instruction = true;
        std::string_view const mnemonic = sources.textOf( instruction->mnemonic.span() );
        point.traps = mnemonic == "brk" || mnemonic == "stp";
        bool const unnamed = names.direct.empty() && names.throughSlot.empty();
        switch ( flowOf( sources.textOf( instruction->mnemonic.span() ) ) )
        {
        case ControlFlow::JUMP:
          point.fallsThrough = false;
          // What `jmp (ptr)` names is the pointer, which it reads, and not
          // where it goes: it follows the addresses its Section owns, as a
          // jump that names nothing does. Through a Slot the targets are
          // named — see docs/decisions/0295-a-jump-through-a-pointer-goes-where-its-section-follows.md.
          point.loose = unnamed || ( indirectJump && names.throughSlot.empty() );
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
        // An opcode `.alt` lets the program write here is one more thing
        // the Point may do: go on, or return. Nothing of the shapes it is
        // allowed on jumps — see
        // docs/decisions/0237-an-instruction-declares-the-opcodes-it-may-be-given.md.
        for ( syntax::Token const& alternative : chunk.alternatives )
        {
          std::string_view const text = sources.textOf( alternative.span() );
          if ( flowOf( text ) == ControlFlow::RETURN )
          {
            point.returns = true;
          }
          else
          {
            point.fallsThrough = true;
          }
          point.traps = point.traps && ( text == "brk" || text == "stp" );
        }
      }

      if ( std::holds_alternative<PhaseEntryContent>( chunk.content ) )
      {
        // A stub calls the Proc around `select` and jumps through the address
        // Patch gives it, as the routine does: nothing follows it, and the
        // entry is the statement's to name, which it did.
        point.fallsThrough = false;
        if ( std::optional<SymbolRef> const select = symbols.find( SELECT_ROMS_NAME ); select.has_value() )
        {
          if ( std::optional<Position> const position = positionOf( symbols, *select ); position.has_value() )
          {
            edge( node, position->where, ReferenceKind::CALL, reading );
          }
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
        // it enters, and nothing of this Section runs after it. Without
        // storage there is no routine, and it goes to the stub of the Phase
        // instead.
        point.fallsThrough = false;
        point.enters = transition->target;
        std::optional<SymbolRef> routine = symbols.find( TRANSITION_ROUTINE_NAME );
        if ( !routine.has_value() && transition->target.has_value() )
        {
          routine = symbols.find( entryStubName( *transition->target ) );
        }
        if ( routine.has_value() )
        {
          if ( std::optional<Position> const position = positionOf( symbols, *routine ); position.has_value() )
          {
            edge( node, position->where, ReferenceKind::JUMP, reading );
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
              std::uint32_t const target = mGraph.indexOf( position->where );
              if ( target != mGraph.indexOf( node.where ) )
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
      statement = chunk.span;
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

  /// Every Point's Successors and transfers, from the positions it goes to,
  /// once every Section has its Points: a position in this Section is a
  /// Successor, one in another the frame that runs next, and one under a
  /// Call — into any Section, this one included — the frame that runs
  /// below this one. What falls off the end goes where `then` said.
  void resolveSuccessors()
  {
    for ( std::uint32_t index = 0; index < nodes().size(); ++index )
    {
      Node& node = nodes()[index];
      Section const& section = mBuild->symbols().moduleAt( node.where.module ).sectionAt( node.where.section );
      std::optional<Landing> const after =
          section.next().has_value()
              ? std::optional{ Landing{
                    .node = mGraph.indexOf( SectionRef{ .module = node.where.module, .section = *section.next() } ),
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
          else
          {
            point.runsOff = true;
          }
        }
        for ( Position const& target : point.targets )
        {
          std::uint32_t const into = mGraph.indexOf( target.where );
          std::uint32_t const landing = Graph::pointIndexOf( nodes()[into], target );
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
          else if ( into == index )
          {
            point.runsOff = true;
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

  Pruned const* mBuild;
  diag::DiagnosticSink* mSink;
  Graph mGraph;
};

} // namespace

Graph readGraph( Pruned const& build, diag::DiagnosticSink& sink )
{
  return Reader{ build, sink }.read();
}

} // namespace nga::model::flow
