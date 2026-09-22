#include "nga/model/Size.hpp"

#include "nga/model/Dispatch.hpp"
#include "nga/model/Transition.hpp"

#include "nga/model/Evaluate.hpp"
#include "nga/model/TypeCheck.hpp"
#include "nga/syntax/Literal.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <utility>
#include <variant>

namespace nga::model
{

namespace
{

constexpr std::int64_t ZERO_PAGE_LIMIT = 0x100;
constexpr std::int64_t ADDRESS_LIMIT = 0x10000;
constexpr std::int64_t BRANCH_NEAREST = -128;
constexpr std::int64_t BRANCH_FARTHEST = 127;

/// One Section's sizes as they stand while its unit is being settled.
struct Measured
{
  std::vector<std::uint32_t> sizes;
  std::vector<std::optional<AddressingMode>> modes;
  std::vector<bool> emits;
  std::vector<InnerSizes> inner;
};

/// A Jcc still at two bytes: which member of its unit it stands in, at which
/// Chunk — or inner Chunk of a macro use — and what it branches to.
struct PendingJcc
{
  std::size_t member = 0;
  ChunkIndex chunk;
  std::optional<std::uint32_t> inner;
  syntax::Expression const* operand = nullptr;

  /// What it becomes where it does not reach: the opposite branch over a
  /// `jmp` for a Jcc, and the `jmp` alone for a certain jump — see
  /// docs/decisions/0181-a-jump-whose-flag-is-known.md.
  AddressingMode longer = AddressingMode::BRANCH_OVER_JUMP;
};

/// A unit's sizes as they stand in one round, as a source an operand is
/// evaluated against: a member's Chunks at the offsets those sizes give, and
/// the member itself at the offset it starts at along the chain, so that a
/// position a branch names evaluates to where it stands from the chain's start.
/// Nothing outside the unit is answered, since a branch reaches nothing there.
class Draft final : public ValueSource
{
public:
  Draft( std::span<SectionRef const> unit, std::span<Measured const> measured ) : mUnit( unit ), mMeasured( measured )
  {
    std::int64_t start = 0;
    mOffsets.reserve( measured.size() );
    mStarts.reserve( measured.size() );
    for ( Measured const& one : measured )
    {
      mStarts.push_back( start );
      std::vector<std::uint32_t> offsets;
      offsets.reserve( one.sizes.size() + 1 );
      offsets.push_back( 0 );
      for ( std::uint32_t const size : one.sizes )
      {
        offsets.push_back( offsets.back() + size );
      }
      start += offsets.back();
      mOffsets.push_back( std::move( offsets ) );
    }
  }

  [[nodiscard]] std::optional<std::int64_t> sizeOfSection( SectionRef where ) override
  {
    std::optional<std::size_t> const member = memberOf( where );
    return member.has_value() ? std::optional<std::int64_t>{ mOffsets[*member].back() } : std::nullopt;
  }

  [[nodiscard]] std::optional<std::int64_t>
  offsetOf( SectionRef where, ChunkIndex chunk, std::optional<std::uint32_t> inner ) override
  {
    std::optional<std::size_t> const member = memberOf( where );
    if ( !member.has_value() || chunk.value >= mOffsets[*member].size() )
    {
      return std::nullopt;
    }
    std::int64_t offset = mOffsets[*member][chunk.value];
    if ( inner.has_value() )
    {
      std::vector<InnerSizes> const& inside = mMeasured[*member].inner;
      if ( chunk.value >= inside.size() || *inner >= inside[chunk.value].offsets.size() )
      {
        return std::nullopt;
      }
      offset += inside[chunk.value].offsets[*inner];
    }
    return offset;
  }

  [[nodiscard]] std::optional<std::int64_t> addressOf( SectionRef where ) override
  {
    std::optional<std::size_t> const member = memberOf( where );
    return member.has_value() ? std::optional<std::int64_t>{ mStarts[*member] } : std::nullopt;
  }

private:
  [[nodiscard]] std::optional<std::size_t> memberOf( SectionRef where ) const
  {
    for ( std::size_t member = 0; member < mUnit.size(); ++member )
    {
      if ( mUnit[member].module == where.module && mUnit[member].section == where.section )
      {
        return member;
      }
    }
    return std::nullopt;
  }

  std::span<SectionRef const> mUnit;
  std::span<Measured const> mMeasured;
  std::vector<std::vector<std::uint32_t>> mOffsets;
  std::vector<std::int64_t> mStarts;
};

/// Makes a Jcc the opposite branch over a `jmp`, and moves what follows it
/// within a macro use's expansion by as much as it grew.
void lengthen( Measured& measured, PendingJcc const& jcc )
{
  std::uint32_t const growth = sizeOf( jcc.longer ) - sizeOf( AddressingMode::RELATIVE );
  measured.sizes[jcc.chunk.value] += growth;
  if ( !jcc.inner.has_value() )
  {
    measured.modes[jcc.chunk.value] = jcc.longer;
    return;
  }
  InnerSizes& inside = measured.inner[jcc.chunk.value];
  inside.modes[*jcc.inner] = jcc.longer;
  for ( std::size_t at = *jcc.inner + 1; at < inside.offsets.size(); ++at )
  {
    inside.offsets[at] += growth;
  }
}

/// Resolves sizes, answering size questions about the program as it goes.
///
/// A reservation may be written in terms of other sizes, so this is a walk of
/// a dependency graph rather than a loop: asking for a Section that is already
/// being computed is the cycle, and it is reported once.
class Sizer final : public ValueSource
{
public:
  Sizer( Merged const& build, diag::DiagnosticSink& sink )
      : mBuild( &build ), mSink( &sink ), mSizes( build.modules() ), mInProgress( build.modules().size() ),
        mPrevious( build.modules().size() )
  {
    for ( std::uint32_t module = 0; module < build.modules().size(); ++module )
    {
      auto const& sections = build.symbols().moduleAt( ModuleIndex{ module } ).sections();
      mInProgress[module].assign( sections.size(), false );
      mPrevious[module].resize( sections.size() );
      for ( std::uint32_t section = 0; section < sections.size(); ++section )
      {
        if ( std::optional<SectionIndex> const next = sections[section].next();
             next.has_value() && next->value < sections.size() )
        {
          mPrevious[module][next->value] = SectionIndex{ section };
        }
      }
    }
  }

  void computeEverything();

  Sizes take() &&
  {
    return std::move( mSizes );
  }

  [[nodiscard]] std::optional<std::int64_t> sizeOfSection( SectionRef where ) override
  {
    return ensure( where ) ? std::optional<std::int64_t>{ mSizes.sizeOfSection( where ) } : std::nullopt;
  }

  [[nodiscard]] std::optional<std::int64_t>
  offsetOf( SectionRef where, ChunkIndex chunk, std::optional<std::uint32_t> inner ) override
  {
    if ( !ensure( where ) )
    {
      return std::nullopt;
    }
    return inner.has_value() ? mSizes.innerOffsetOf( where, chunk, *inner ) : mSizes.offsetOf( where, chunk );
  }

private:
  /// True once this Section's sizes are known. False when they cannot be,
  /// which is either a cycle — reported here — or a consequence of something
  /// already reported.
  bool ensure( SectionRef where );

  /// The Sections whose sizes are settled together: a Proc and every Proc
  /// chained to it by `then`, in the order of the chain, since a Jcc reaches
  /// along the whole chain in either direction and the distances of one
  /// chain depend on each other. Any other Section is a unit alone.
  [[nodiscard]] std::vector<SectionRef> unitOf( SectionRef where ) const;

  /// One Section's Chunks sized as they are before any Jcc lengthens, with
  /// every Jcc among them added to `jccs`.
  Measured measure( SectionRef where, std::size_t member, std::vector<PendingJcc>& jccs );

  /// Lengthens every Jcc of the unit whose distance a branch does not reach,
  /// until every one it leaves at two bytes does.
  void relax( std::span<SectionRef const> unit, std::vector<Measured>& measured, std::vector<PendingJcc> jccs );

  /// The operand of a Chunk that is a Jcc or a certain jump sized as a
  /// branch, or null.
  [[nodiscard]] syntax::Expression const*
  jccOperand( SectionRef where, Chunk const& chunk, std::optional<AddressingMode> mode ) const;

  /// What that Chunk becomes where its branch does not reach.
  [[nodiscard]] AddressingMode longerModeOf( Chunk const& chunk ) const;

  std::uint32_t sizeOfChunk( SectionRef where, Chunk const& chunk, std::optional<AddressingMode>& mode );
  std::uint32_t sizeOfInstruction( SectionRef where, Chunk const& chunk, std::optional<AddressingMode>& mode );
  std::uint32_t sizeOfData( SectionRef where, Chunk const& chunk );
  std::uint32_t sizeOfReservation( SectionRef where, Chunk const& chunk );

  /// The PlacementClass an operand carries, or nothing when it has none to
  /// carry — which is what the diagnostic is about.
  std::optional<PlacementClass>
  placementOf( SectionRef where, syntax::Expression const& operand, syntax::OperandShape shape );

  void report( diag::Diagnostic value ) const
  {
    mSink->add( std::move( value ) );
  }

  Merged const* mBuild;
  diag::DiagnosticSink* mSink;
  Sizes mSizes;
  std::vector<std::vector<bool>> mInProgress;

  /// Per Module and Section, the Proc chained to it by `then`, if one is.
  std::vector<std::vector<std::optional<SectionIndex>>> mPrevious;
};

std::optional<PlacementClass>
Sizer::placementOf( SectionRef where, syntax::Expression const& operand, syntax::OperandShape shape )
{
  // Only these three shapes have two widths to choose between. An immediate or
  // an indirect operand is one byte or two whatever it names, so nothing about
  // it has to be known here.
  bool const widthDepends = shape == syntax::OperandShape::DIRECT || shape == syntax::OperandShape::DIRECT_X ||
                            shape == syntax::OperandShape::DIRECT_Y;
  if ( !widthDepends )
  {
    return PlacementClass::ABSOLUTE;
  }

  // Types have already been checked, so this asks rather than complains.
  diag::SeverityPolicy policy;
  diag::DiagnosticSink quiet{ policy };
  Type const type = typeOf( *mBuild, where.module, operand, quiet );

  if ( type.is( syntax::ExpressionType::ADDRESS ) )
  {
    // Declared where the Symbol was defined, which may be a Module this one
    // never saw. Nothing here reads an address back.
    return type.placement;
  }
  if ( !type.is( syntax::ExpressionType::INTEGER ) )
  {
    return std::nullopt;
  }

  // An Integer operand has no PlacementClass to inherit, so its value decides
  // the width — and the value has to be one the author declared rather than
  // one a Step produced.
  std::optional<std::int64_t> const value =
      declaredValueOf( mBuild->sources(), mBuild->symbols(), &mBuild->charsets(), where.module, operand );
  if ( !value.has_value() )
  {
    report(
        diag::diagnostic( diag::DiagnosticId::PLACEMENT_CLASS_UNKNOWN ).at( operand.span.begin, operand.span.length ) );
    return std::nullopt;
  }
  if ( *value < 0 || *value >= ADDRESS_LIMIT )
  {
    report( diag::diagnostic( diag::DiagnosticId::ADDRESS_OUT_OF_RANGE )
                .at( operand.span.begin, operand.span.length )
                .arg( "value", *value ) );
    return std::nullopt;
  }
  return *value < ZERO_PAGE_LIMIT ? PlacementClass::ZEROPAGE : PlacementClass::ABSOLUTE;
}

std::uint32_t Sizer::sizeOfInstruction( SectionRef where, Chunk const& chunk, std::optional<AddressingMode>& mode )
{
  auto const& instruction = std::get<InstructionContent>( chunk.content );
  std::string_view const mnemonic = mBuild->sources().textOf( instruction.mnemonic.span() );

  if ( !isMnemonic( mnemonic ) )
  {
    // Cannot happen: Expand turns every mnemonic the ISA does not have into
    // a macro use, and reports the ones that name no macro.
    return 0;
  }

  std::span<syntax::ExpressionPtr const> const items =
      mBuild->symbols().moduleAt( where.module ).sectionAt( where.section ).itemsOf( chunk );
  PlacementClass placement = PlacementClass::ABSOLUTE;
  if ( !items.empty() )
  {
    if ( containsError( *items.front() ) )
    {
      return 0;
    }
    std::optional<PlacementClass> const found = placementOf( where, *items.front(), instruction.shape );
    if ( !found.has_value() )
    {
      return 0;
    }
    placement = *found;
  }

  mode = modeFor( mnemonic, instruction.shape, placement );
  if ( !mode.has_value() )
  {
    report( diag::diagnostic( diag::DiagnosticId::NO_SUCH_MODE )
                .at( chunk.span.begin, chunk.span.length )
                .arg( "mnemonic", std::string{ mnemonic } )
                .arg( "mode", std::string{ nameOf( instruction.shape ) } ) );
    return 0;
  }

  // The table holds both processors, so that a program writing `stz` on a
  // 6502 is told which processor it wants rather than that no such mnemonic
  // exists. The size stands either way: what the instruction occupies is not
  // in question, only whether the machine can run it — see
  // docs/decisions/0182-the-target-names-its-processor.md.
  Cpu const cpu = mBuild->project().target.cpu;
  if ( cpu != Cpu::WDC65SC02 && needs65sc02( mnemonic, *mode ) )
  {
    report( diag::diagnostic( diag::DiagnosticId::INSTRUCTION_NEEDS_CPU )
                .at( chunk.span.begin, chunk.span.length )
                .arg( "mnemonic", std::string{ mnemonic } )
                .arg( "needs", std::string{ nameOf( Cpu::WDC65SC02 ) } )
                .arg( "cpu", std::string{ nameOf( cpu ) } ) );
  }
  return sizeOf( *mode );
}

std::uint32_t Sizer::sizeOfData( SectionRef where, Chunk const& chunk )
{
  auto const& data = std::get<DataContent>( chunk.content );
  Section const& section = mBuild->symbols().moduleAt( where.module ).sectionAt( where.section );

  std::uint32_t total = 0;
  for ( syntax::ExpressionPtr const& item : section.itemsOf( chunk ) )
  {
    if ( containsError( *item ) )
    {
      continue;
    }
    if ( item->kind == syntax::ExpressionKind::STRING )
    {
      // One byte per character, whatever the Charset turns out to map it to.
      // That guarantee is what lets this be known before the table is.
      total += syntax::characterCountOf( syntax::quotedOf( mBuild->sources().textOf( item->token.span() ) ).body );
      continue;
    }
    total += data.width == syntax::DataWidth::BYTE ? 1 : 2;
  }
  return total;
}

std::uint32_t Sizer::sizeOfReservation( SectionRef where, Chunk const& chunk )
{
  Section const& section = mBuild->symbols().moduleAt( where.module ).sectionAt( where.section );
  auto const items = section.itemsOf( chunk );
  if ( items.empty() || containsError( *items.front() ) )
  {
    return 0;
  }

  // A reservation may be written in terms of other sizes, which is why this is
  // evaluated against the Sizer rather than against nothing.
  std::optional<std::int64_t> const count =
      evaluate( mBuild->sources(), mBuild->symbols(), &mBuild->charsets(), where.module, *items.front(), *this );
  if ( !count.has_value() )
  {
    // Types have already been checked, so an expression that types cleanly and
    // still has no value is one whose size depends on itself. One that does
    // not type has been reported, and says nothing more here.
    diag::SeverityPolicy policy;
    diag::DiagnosticSink quiet{ policy };
    if ( typeOf( *mBuild, where.module, *items.front(), quiet ).is( syntax::ExpressionType::INTEGER ) )
    {
      report( diag::diagnostic( diag::DiagnosticId::SIZE_CYCLE ).at( chunk.span.begin, chunk.span.length ) );
    }
    return 0;
  }
  if ( *count < 0 || *count > ADDRESS_LIMIT )
  {
    report( diag::diagnostic( diag::DiagnosticId::RESERVATION_NOT_A_COUNT )
                .at( chunk.span.begin, chunk.span.length )
                .arg( "size", *count ) );
    return 0;
  }
  return static_cast<std::uint32_t>( *count );
}

std::uint32_t Sizer::sizeOfChunk( SectionRef where, Chunk const& chunk, std::optional<AddressingMode>& mode )
{
  if ( std::holds_alternative<InstructionContent>( chunk.content ) )
  {
    return sizeOfInstruction( where, chunk, mode );
  }
  if ( std::holds_alternative<DataContent>( chunk.content ) )
  {
    return sizeOfData( where, chunk );
  }
  if ( std::holds_alternative<TransitionContent>( chunk.content ) )
  {
    // An entry per Phase the Section is present in, which is its Module's
    // Residency and known before any size is.
    return sizeOfTransition( mBuild->symbols().moduleAt( where.module ).residency() );
  }
  if ( std::holds_alternative<TransitionCellContent>( chunk.content ) )
  {
    return TRANSITION_CELL_SIZE;
  }
  if ( std::holds_alternative<DispatchContent>( chunk.content ) )
  {
    // The form is the processor's and the count the author's, so the size is
    // known here; neither is an address and neither is a size.
    std::size_t const targets =
        mBuild->symbols().moduleAt( where.module ).sectionAt( where.section ).itemsOf( chunk ).size();
    return sizeOfDispatch( dispatchFormOf( mBuild->project().target.cpu, targets ), targets );
  }
  if ( auto const* const cell = std::get_if<SlotCellContent>( &chunk.content ); cell != nullptr )
  {
    return cell->binding == Binding::VECTOR ? VECTOR_CELL_SIZE : POINTER_CELL_SIZE;
  }
  if ( std::holds_alternative<MacroUseContent>( chunk.content ) )
  {
    // Sized by what it expanded to, inner Chunk by inner Chunk, in `measure`;
    // an inner Chunk is a use only as the marker a body's `.with` leaves
    // either side of its statement, which is nothing, since a nested use
    // expands in place.
    return 0;
  }
  return sizeOfReservation( where, chunk );
}

AddressingMode Sizer::longerModeOf( Chunk const& chunk ) const
{
  auto const* const instruction = std::get_if<InstructionContent>( &chunk.content );
  return instruction == nullptr ? AddressingMode::BRANCH_OVER_JUMP
                                : lengthenedModeOf( mBuild->sources().textOf( instruction->mnemonic.span() ) );
}

syntax::Expression const*
Sizer::jccOperand( SectionRef where, Chunk const& chunk, std::optional<AddressingMode> mode ) const
{
  auto const* const instruction = std::get_if<InstructionContent>( &chunk.content );
  std::string_view const mnemonic =
      instruction == nullptr ? std::string_view{} : mBuild->sources().textOf( instruction->mnemonic.span() );
  if ( instruction == nullptr || mode != AddressingMode::RELATIVE ||
       ( !isJcc( mnemonic ) && !isCertainJump( mnemonic ) ) )
  {
    return nullptr;
  }
  std::span<syntax::ExpressionPtr const> const items =
      mBuild->symbols().moduleAt( where.module ).sectionAt( where.section ).itemsOf( chunk );
  return items.empty() ? nullptr : &*items.front();
}

std::vector<SectionRef> Sizer::unitOf( SectionRef where ) const
{
  std::vector<std::optional<SectionIndex>> const& previous = mPrevious[where.module.value];
  Module const& module = mBuild->symbols().moduleAt( where.module );

  // A chain that comes back to where it started has no first Section
  // (NGA2264), so each walk stops at a Section it has already been at.
  SectionIndex first = where.section;
  for ( std::size_t steps = 0; steps < previous.size(); ++steps )
  {
    std::optional<SectionIndex> const before = previous[first.value];
    if ( !before.has_value() || *before == where.section )
    {
      break;
    }
    first = *before;
  }

  std::vector<SectionRef> unit;
  std::optional<SectionIndex> at = first;
  while ( at.has_value() && unit.size() < previous.size() )
  {
    unit.push_back( SectionRef{ .module = where.module, .section = *at } );
    at = module.sectionAt( *at ).next();
    if ( at == first )
    {
      break;
    }
  }
  return unit;
}

Measured Sizer::measure( SectionRef where, std::size_t member, std::vector<PendingJcc>& jccs )
{
  Section const& section = mBuild->symbols().moduleAt( where.module ).sectionAt( where.section );

  Measured measured;
  measured.sizes.reserve( section.chunks().size() );
  measured.modes.reserve( section.chunks().size() );
  measured.emits.reserve( section.chunks().size() );
  measured.inner.reserve( section.chunks().size() );

  for ( std::uint32_t index = 0; index < section.chunks().size(); ++index )
  {
    Chunk const& chunk = section.chunks()[index];
    std::optional<AddressingMode> mode;
    InnerSizes inside;
    if ( std::holds_alternative<MacroUseContent>( chunk.content ) )
    {
      // A use is as large as what it expanded to, and a use that expanded to
      // nothing is nothing, and emits nothing.
      std::span<Chunk const> const innerChunks = section.innerChunksOf( ChunkIndex{ index } );
      std::uint32_t running = 0;
      inside.offsets.push_back( running );
      for ( std::uint32_t one = 0; one < innerChunks.size(); ++one )
      {
        std::optional<AddressingMode> innerMode;
        running += sizeOfChunk( where, innerChunks[one], innerMode );
        inside.offsets.push_back( running );
        inside.modes.push_back( innerMode );
        if ( syntax::Expression const* const operand = jccOperand( where, innerChunks[one], innerMode );
             operand != nullptr )
        {
          jccs.push_back( PendingJcc{ .member = member,
                                      .chunk = ChunkIndex{ index },
                                      .inner = one,
                                      .operand = operand,
                                      .longer = longerModeOf( innerChunks[one] ) } );
        }
      }
      measured.sizes.push_back( running );
      measured.emits.push_back( !inside.modes.empty() );
    }
    else
    {
      measured.sizes.push_back( sizeOfChunk( where, chunk, mode ) );
      measured.emits.push_back( !std::holds_alternative<ReserveContent>( chunk.content ) );
      if ( syntax::Expression const* const operand = jccOperand( where, chunk, mode ); operand != nullptr )
      {
        jccs.push_back( PendingJcc{ .member = member,
                                    .chunk = ChunkIndex{ index },
                                    .inner = std::nullopt,
                                    .operand = operand,
                                    .longer = longerModeOf( chunk ) } );
      }
    }
    measured.modes.push_back( mode );
    measured.inner.push_back( std::move( inside ) );
  }
  return measured;
}

void Sizer::relax( std::span<SectionRef const> unit, std::vector<Measured>& measured, std::vector<PendingJcc> jccs )
{
  // Every Jcc starts at two bytes and only lengthens, so the rounds end. No
  // Section aligns anything inside itself, so lengthening one never shortens
  // another's distance: a Jcc out of reach in one round is out of reach in
  // every consistent choice, which is what makes the result the smallest —
  // see docs/decisions/0077-a-jcc-is-two-bytes-or-five.md.
  while ( !jccs.empty() )
  {
    std::vector<PendingJcc> longer;
    std::vector<PendingJcc> still;
    {
      Draft draft{ unit, measured };
      for ( PendingJcc const& jcc : jccs )
      {
        SectionRef const where = unit[jcc.member];
        std::optional<std::int64_t> const target =
            evaluate( mBuild->sources(), mBuild->symbols(), &mBuild->charsets(), where.module, *jcc.operand, draft );
        std::optional<std::int64_t> const start = draft.addressOf( where );
        std::optional<std::int64_t> const offset = draft.offsetOf( where, jcc.chunk, jcc.inner );
        if ( !target.has_value() || !start.has_value() || !offset.has_value() )
        {
          // What it names lies outside its unit, or does not evaluate at all,
          // and the type check has said so. It stays a branch, which Patch
          // finds the same about; no distance of it lengthens anything.
          continue;
        }
        std::int64_t const distance = *target - ( *start + *offset + sizeOf( AddressingMode::RELATIVE ) );
        ( distance < BRANCH_NEAREST || distance > BRANCH_FARTHEST ? longer : still ).push_back( jcc );
      }
    }
    if ( longer.empty() )
    {
      return;
    }
    for ( PendingJcc const& jcc : longer )
    {
      lengthen( measured[jcc.member], jcc );
    }
    jccs = std::move( still );
  }
}

bool Sizer::ensure( SectionRef where )
{
  if ( mSizes.isKnown( where ) )
  {
    return true;
  }
  if ( mInProgress[where.module.value][where.section.value] )
  {
    return false;
  }

  std::vector<SectionRef> const unit = unitOf( where );
  for ( SectionRef const member : unit )
  {
    mInProgress[member.module.value][member.section.value] = true;
  }

  std::vector<Measured> measured;
  measured.reserve( unit.size() );
  std::vector<PendingJcc> jccs;
  for ( std::size_t member = 0; member < unit.size(); ++member )
  {
    measured.push_back( measure( unit[member], member, jccs ) );
  }
  relax( unit, measured, std::move( jccs ) );

  for ( std::size_t member = 0; member < unit.size(); ++member )
  {
    SectionRef const one = unit[member];
    mInProgress[one.module.value][one.section.value] = false;
    mSizes.store( one,
                  std::move( measured[member].sizes ),
                  std::move( measured[member].modes ),
                  measured[member].emits,
                  std::move( measured[member].inner ) );
  }
  return true;
}

void Sizer::computeEverything()
{
  for ( std::uint32_t module = 0; module < mBuild->symbols().modules().size(); ++module )
  {
    Module const& one = mBuild->symbols().modules()[module];
    for ( std::uint32_t section = 0; section < one.sections().size(); ++section )
    {
      ensure( SectionRef{ .module = ModuleIndex{ module }, .section = SectionIndex{ section } } );
    }
  }
}

} // namespace

Sizes::Sizes( std::span<Module const> modules )
{
  mByModule.resize( modules.size() );
  for ( std::size_t module = 0; module < modules.size(); ++module )
  {
    mByModule[module].resize( modules[module].sections().size() );
  }
}

void Sizes::store( SectionRef where,
                   std::vector<std::uint32_t> sizes,
                   std::vector<std::optional<AddressingMode>> modes,
                   std::vector<bool> const& emits,
                   std::vector<InnerSizes> inner )
{
  OneSection& one = mByModule[where.module.value][where.section.value];
  one.sizes = std::move( sizes );
  one.modes = std::move( modes );
  one.inner = std::move( inner );

  one.offsets.clear();
  one.offsets.reserve( one.sizes.size() + 1 );
  std::uint32_t running = 0;
  one.offsets.push_back( running );
  for ( std::uint32_t const size : one.sizes )
  {
    running += size;
    one.offsets.push_back( running );
  }

  // From the first emitting Chunk to the end of the last; a reservation in
  // between is inside, and one at either end is not. Empty when nothing emits.
  one.initialised = InitialisedExtent{};
  bool first = true;
  for ( std::size_t index = 0; index < one.sizes.size(); ++index )
  {
    if ( !emits[index] )
    {
      continue;
    }
    if ( first )
    {
      one.initialised.begin = one.offsets[index];
      first = false;
    }
    one.initialised.end = one.offsets[index + 1];
  }
  one.known = true;
}

Sizes::OneSection const& Sizes::at( SectionRef where ) const
{
  return mByModule[where.module.value][where.section.value];
}

bool Sizes::isKnown( SectionRef where ) const
{
  return at( where ).known;
}

std::uint32_t Sizes::sizeOfChunk( SectionRef where, ChunkIndex chunk ) const
{
  return at( where ).sizes[chunk.value];
}

std::uint32_t Sizes::offsetOf( SectionRef where, ChunkIndex chunk ) const
{
  return at( where ).offsets[chunk.value];
}

std::uint32_t Sizes::sizeOfSection( SectionRef where ) const
{
  return at( where ).offsets.back();
}

InitialisedExtent Sizes::initialisedExtentOf( SectionRef where ) const
{
  return at( where ).initialised;
}

std::optional<AddressingMode> Sizes::modeOf( SectionRef where, ChunkIndex chunk ) const
{
  return at( where ).modes[chunk.value];
}

std::uint32_t Sizes::innerOffsetOf( SectionRef where, ChunkIndex chunk, std::uint32_t inner ) const
{
  OneSection const& one = at( where );
  return one.offsets[chunk.value] + one.inner[chunk.value].offsets[inner];
}

std::uint32_t Sizes::innerSizeOf( SectionRef where, ChunkIndex chunk, std::uint32_t inner ) const
{
  InnerSizes const& inside = at( where ).inner[chunk.value];
  return inside.offsets[inner + 1] - inside.offsets[inner];
}

std::optional<AddressingMode> Sizes::innerModeOf( SectionRef where, ChunkIndex chunk, std::uint32_t inner ) const
{
  return at( where ).inner[chunk.value].modes[inner];
}

Sizes computeSizes( Merged const& build, diag::DiagnosticSink& sink )
{
  Sizer sizer{ build, sink };
  sizer.computeEverything();
  return std::move( sizer ).take();
}

} // namespace nga::model
