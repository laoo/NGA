#include "nga/model/Patch.hpp"

#include "nga/model/Dispatch.hpp"
#include "nga/model/Prune.hpp"
#include "nga/model/Transition.hpp"

#include "nga/model/Evaluate.hpp"
#include "nga/model/Isa.hpp"
#include "nga/syntax/Literal.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace nga::model
{

namespace
{

/// Writes one Section's bytes.
class Patcher
{
public:
  Patcher( Placed const& build, Storage const* storage, diag::DiagnosticSink& sink )
      : mBuild( &build ), mStorage( storage ), mSink( &sink ), mResolved( build.sizes(), build.layout() ),
        mEntries( build, sink )
  {
  }

  std::vector<std::uint8_t> bytesOf( SectionRef where );

  /// Every Frame, written into Storage where the first half of PlaceStorage
  /// put it, once the second half has said where every Payload waits.
  void storeFrames( Storage& storage );

private:
  void patchChunk( SectionRef where, ChunkIndex index, std::vector<std::uint8_t>& into );

  /// One instruction or one data statement, at `at` bytes into the Section:
  /// a Chunk of the Section, or an inner Chunk of a macro use's expansion,
  /// which reaches its expressions through the same arena.
  void patchInstruction( SectionRef where,
                         Chunk const& chunk,
                         std::uint32_t at,
                         std::optional<AddressingMode> mode,
                         std::vector<std::uint8_t>& into );
  void patchData( SectionRef where, Chunk const& chunk, std::uint32_t at, std::vector<std::uint8_t>& into );

  /// The bytes a string literal produces, through its character set when it
  /// names one. Nothing when the set does not exist or does not map a
  /// character, both of which the type check has already reported.
  [[nodiscard]] std::optional<std::vector<std::uint8_t>>
  bytesOf( ModuleIndex home, syntax::Expression const& item, syntax::Quoted const& quoted, std::string_view text );

  /// Writes `width` little-endian bytes, reporting a value that does not fit.
  /// A negative value is accepted as its two's complement, which is what makes
  /// `.byte -1` and `lda #-1` mean what they look like.
  void patchDispatch( SectionRef where, Chunk const& chunk, std::uint32_t at, std::vector<std::uint8_t>& into );

  void write( std::vector<std::uint8_t>& into,
              std::uint32_t at,
              std::uint32_t width,
              std::int64_t value,
              diag::SourceSpan span );

  [[nodiscard]] std::optional<std::int64_t> valueOf( ModuleIndex home, syntax::Expression const& node )
  {
    return evaluate( mBuild->sources(), mBuild->symbols(), &mBuild->charsets(), home, node, mResolved );
  }

  void report( diag::Diagnostic value ) const
  {
    mSink->add( std::move( value ) );
  }

  Placed const* mBuild;

  /// Null in the first phase, which writes no table and so asks nothing of it.
  Storage const* mStorage;
  diag::DiagnosticSink* mSink;
  Resolved mResolved;
  EntryAddresses mEntries;
};

void Patcher::write(
    std::vector<std::uint8_t>& into, std::uint32_t at, std::uint32_t width, std::int64_t value, diag::SourceSpan span )
{
  std::int64_t const limit = std::int64_t{ 1 } << ( width * 8U );
  if ( value >= limit || value < -( limit / 2 ) )
  {
    report( diag::diagnostic( diag::DiagnosticId::VALUE_DOES_NOT_FIT )
                .at( span.begin, span.length )
                .arg( "value", value )
                .arg( "width", static_cast<std::int64_t>( width ) ) );
    return;
  }

  auto const bits = static_cast<std::uint64_t>( value );
  for ( std::uint32_t byte = 0; byte < width; ++byte )
  {
    into[at + byte] = static_cast<std::uint8_t>( ( bits >> ( byte * 8U ) ) & 0xFFU );
  }
}

void Patcher::patchInstruction( SectionRef where,
                                Chunk const& chunk,
                                std::uint32_t at,
                                std::optional<AddressingMode> mode,
                                std::vector<std::uint8_t>& into )
{
  Section const& section = mBuild->symbols().moduleAt( where.module ).sectionAt( where.section );
  if ( !mode.has_value() )
  {
    // Size could not settle a width and said so. There is nothing to write.
    return;
  }

  auto const& instruction = std::get<InstructionContent>( chunk.content );
  std::optional<std::uint8_t> const opcode = opcodeOf( mBuild->sources().textOf( instruction.mnemonic.span() ), *mode );
  if ( !opcode.has_value() )
  {
    return;
  }

  into[at] = *opcode;

  std::span<syntax::ExpressionPtr const> const items = section.itemsOf( chunk );
  if ( items.empty() )
  {
    return;
  }

  std::optional<std::int64_t> const value = valueOf( where.module, *items.front() );
  if ( !value.has_value() )
  {
    // Whatever stopped this has been reported: an undefined name, a character
    // set that does not exist, a Section that was never placed.
    return;
  }

  if ( *mode == AddressingMode::RELATIVE )
  {
    // A branch's operand is a distance from the instruction after it, which is
    // the one place where an address and a size meet.
    std::int64_t const from = mBuild->layout().addressOf( where ) + at + 2;
    std::int64_t const distance = *value - from;
    if ( distance < -128 || distance > 127 )
    {
      // Only a Bcc lands here: Size lengthens a Jcc whose distance does not
      // fit, so the Jcc offered is the one of the same letters.
      std::string_view const mnemonic = mBuild->sources().textOf( instruction.mnemonic.span() );
      report( diag::diagnostic( diag::DiagnosticId::BRANCH_OUT_OF_RANGE )
                  .at( chunk.span.begin, chunk.span.length )
                  .arg( "mnemonic", std::string{ mnemonic } )
                  .arg( "distance", distance )
                  .arg( "jcc", std::string{ jccFor( mnemonic ).value_or( mnemonic ) } ) );
      return;
    }
    into[at + 1] = static_cast<std::uint8_t>( distance & 0xFF );
    return;
  }

  if ( *mode == AddressingMode::BRANCH_OVER_JUMP )
  {
    // A Jcc Size found out of a branch's reach. Its first byte, written above,
    // is the opposite branch, which skips the `jmp` that goes where the Jcc
    // does.
    std::uint32_t const skip = sizeOf( AddressingMode::ABSOLUTE );
    into[at + 1] = static_cast<std::uint8_t>( skip );
    into[at + 2] = opcodeOf( "jmp", AddressingMode::ABSOLUTE ).value_or( 0 );
    write( into, at + 3, skip - 1, *value, items.front()->span );
    return;
  }

  write( into, at + 1, sizeOf( *mode ) - 1, *value, items.front()->span );
}

std::optional<std::vector<std::uint8_t>> Patcher::bytesOf( ModuleIndex home,
                                                           syntax::Expression const& item,
                                                           syntax::Quoted const& quoted,
                                                           std::string_view text )
{
  if ( quoted.charset.empty() )
  {
    return syntax::plainBytesOf( text );
  }

  // Through the character set the literal's own Module and Namespace see.
  std::optional<SymbolRef> const named = mBuild->symbols().resolveText( home, item, quoted.charset );
  if ( !named.has_value() )
  {
    return std::nullopt;
  }
  std::optional<CharsetRef> const where = charsetOf( mBuild->symbols(), *named );
  if ( !where.has_value() )
  {
    return std::nullopt;
  }

  Charset const& table = mBuild->charsets().at( *where );
  std::vector<std::uint8_t> bytes;
  for ( char32_t const point : syntax::codePointsOf( quoted.body ) )
  {
    std::optional<std::uint8_t> const byte = table.byteFor( point );
    if ( !byte.has_value() )
    {
      return std::nullopt;
    }
    bytes.push_back( *byte );
  }
  return bytes;
}

void Patcher::patchData( SectionRef where, Chunk const& chunk, std::uint32_t at, std::vector<std::uint8_t>& into )
{
  Section const& section = mBuild->symbols().moduleAt( where.module ).sectionAt( where.section );
  auto const& data = std::get<DataContent>( chunk.content );

  std::uint32_t const width = data.width == syntax::DataWidth::BYTE ? 1 : 2;

  for ( syntax::ExpressionPtr const& item : section.itemsOf( chunk ) )
  {
    if ( containsError( *item ) )
    {
      continue;
    }

    if ( item->kind == syntax::ExpressionKind::STRING )
    {
      std::string_view const text = mBuild->sources().textOf( item->token.span() );
      syntax::Quoted const quoted = syntax::quotedOf( text );
      std::optional<std::vector<std::uint8_t>> const bytes = bytesOf( where.module, *item, quoted, text );
      if ( !bytes.has_value() )
      {
        // Every reason this can fail has already been reported, and one byte
        // per code point holds whether or not the table did — which is what
        // keeps the sizes this walk was given true.
        at += syntax::characterCountOf( quoted.body );
        continue;
      }
      for ( std::uint8_t const byte : *bytes )
      {
        into[at] = byte;
        ++at;
      }
      continue;
    }

    if ( std::optional<std::int64_t> const value = valueOf( where.module, *item ); value.has_value() )
    {
      write( into, at, width, *value, item->span );
    }
    at += width;
  }
}

/// A Dispatch: the instructions that jump, and then the table they jump
/// through — one of addresses where the processor has `jmp (abs,x)`, two
/// halves of each target less one where the `rts` trick is what it has. The
/// tables stand inside the Chunk, so their addresses are its own plus an
/// offset and no name is needed for them.
void Patcher::patchDispatch( SectionRef where, Chunk const& chunk, std::uint32_t at, std::vector<std::uint8_t>& into )
{
  Section const& section = mBuild->symbols().moduleAt( where.module ).sectionAt( where.section );
  std::span<syntax::ExpressionPtr const> const targets = section.itemsOf( chunk );
  DispatchForm const form = dispatchFormOf( mBuild->project().target.cpu, targets.size() );
  std::int64_t const table = std::int64_t{ mBuild->layout().addressOf( where ) } + at + tableOffsetOf( form );
  auto const count = static_cast<std::uint32_t>( targets.size() );

  auto const opcode = []( std::string_view mnemonic, AddressingMode mode )
  { return opcodeOf( mnemonic, mode ).value_or( 0 ); };

  std::uint32_t entries = at + tableOffsetOf( form );
  if ( form == DispatchForm::INDEXED )
  {
    into[at] = opcode( "asl", AddressingMode::IMPLIED );
    into[at + 1] = opcode( "tax", AddressingMode::IMPLIED );
    into[at + 2] = opcode( "jmp", AddressingMode::ABSOLUTE_INDEXED_INDIRECT );
    write( into, at + 3, 2, table, chunk.span );
  }
  else
  {
    // The high half first, since `rts` goes to the address on the stack plus
    // one and the high byte is pushed first.
    into[at] = opcode( "tax", AddressingMode::IMPLIED );
    into[at + 1] = opcode( "lda", AddressingMode::ABSOLUTE_X );
    write( into, at + 2, 2, table, chunk.span );
    into[at + 4] = opcode( "pha", AddressingMode::IMPLIED );
    into[at + 5] = opcode( "lda", AddressingMode::ABSOLUTE_X );
    write( into, at + 6, 2, table + count, chunk.span );
    into[at + 8] = opcode( "pha", AddressingMode::IMPLIED );
    into[at + 9] = opcode( "rts", AddressingMode::IMPLIED );
  }

  for ( std::uint32_t index = 0; index < count; ++index )
  {
    std::optional<std::int64_t> const value = valueOf( where.module, *targets[index] );
    if ( !value.has_value() )
    {
      // Reported where it failed: a name that resolves to nothing, a Section
      // that was never placed.
      continue;
    }
    if ( form == DispatchForm::INDEXED )
    {
      write( into, entries + ( 2 * index ), 2, *value, targets[index]->span );
      continue;
    }
    std::int64_t const less = *value - 1;
    into[entries + index] = static_cast<std::uint8_t>( ( less >> 8 ) & 0xFF );
    into[entries + count + index] = static_cast<std::uint8_t>( less & 0xFF );
  }
}

void Patcher::patchChunk( SectionRef where, ChunkIndex index, std::vector<std::uint8_t>& into )
{
  Section const& section = mBuild->symbols().moduleAt( where.module ).sectionAt( where.section );
  Chunk const& chunk = section.chunks()[index.value];
  Sizes const& sizes = mBuild->sizes();

  if ( std::holds_alternative<InstructionContent>( chunk.content ) )
  {
    patchInstruction( where, chunk, sizes.offsetOf( where, index ), sizes.modeOf( where, index ), into );
    return;
  }
  if ( std::holds_alternative<DataContent>( chunk.content ) )
  {
    patchData( where, chunk, sizes.offsetOf( where, index ), into );
    return;
  }
  if ( std::holds_alternative<DispatchContent>( chunk.content ) )
  {
    patchDispatch( where, chunk, sizes.offsetOf( where, index ), into );
    return;
  }
  if ( std::holds_alternative<MacroUseContent>( chunk.content ) )
  {
    // The use's bytes are its expansion's, inner Chunk by inner Chunk, at
    // the offsets Size gave them.
    std::span<Chunk const> const inner = section.innerChunksOf( index );
    for ( std::uint32_t one = 0; one < inner.size(); ++one )
    {
      std::uint32_t const at = sizes.innerOffsetOf( where, index, one );
      if ( std::holds_alternative<InstructionContent>( inner[one].content ) )
      {
        patchInstruction( where, inner[one], at, sizes.innerModeOf( where, index, one ), into );
      }
      else if ( std::holds_alternative<DataContent>( inner[one].content ) )
      {
        patchData( where, inner[one], at, into );
      }
    }
    return;
  }

  // The Target's own kinds, written in the layout docs/spec/transition.md
  // states — see docs/decisions/0019-transition-mechanism.md.
  std::span<std::uint8_t> const own = std::span{ into }.subspan( mBuild->sizes().offsetOf( where, index ),
                                                                 mBuild->sizes().sizeOfChunk( where, index ) );
  if ( auto const* const transition = std::get_if<TransitionContent>( &chunk.content ); transition != nullptr )
  {
    writeTransition( *transition, where.module, *mBuild, *mStorage, own );
    return;
  }
  if ( std::holds_alternative<TransitionCellContent>( chunk.content ) )
  {
    writeCell( *mBuild, own );
    return;
  }
  if ( auto const* const cell = std::get_if<SlotCellContent>( &chunk.content ); cell != nullptr )
  {
    writeSlotCell( *cell, *mBuild, own );
    return;
  }
  // A reservation occupies addresses and emits nothing, so its bytes are left
  // as they were made: zero — which is what travels where one lies inside the
  // initialised extent, and nothing where one lies outside it.
}

std::vector<std::uint8_t> Patcher::bytesOf( SectionRef where )
{
  if ( !mBuild->sizes().isKnown( where ) )
  {
    return {};
  }

  std::vector<std::uint8_t> bytes( mBuild->sizes().sizeOfSection( where ), 0 );
  Section const& section = mBuild->symbols().moduleAt( where.module ).sectionAt( where.section );

  // Addresses as this Module sees them: a Movable Section it refers to holds
  // one across this Module's Residency, by the rule of 0030, so the bytes
  // are right in every Phase the Module is in.
  mResolved.viewFrom( mBuild->symbols().moduleAt( where.module ).residency() );
  for ( std::uint32_t index = 0; index < section.chunks().size(); ++index )
  {
    patchChunk( where, ChunkIndex{ index }, bytes );
  }
  return bytes;
}

} // namespace

Bytes::Bytes( std::span<Module const> modules )
{
  mByModule.resize( modules.size() );
  for ( std::size_t module = 0; module < modules.size(); ++module )
  {
    mByModule[module].resize( modules[module].sections().size() );
  }
}

void Bytes::store( SectionRef where, std::vector<std::uint8_t> bytes )
{
  mByModule[where.module.value][where.section.value] = std::move( bytes );
}

std::span<std::uint8_t const> Bytes::of( SectionRef where ) const
{
  return mByModule[where.module.value][where.section.value];
}

std::span<std::uint8_t const> Bytes::ofChunk( SectionRef where, ChunkIndex chunk, Sizes const& sizes ) const
{
  return of( where ).subspan( sizes.offsetOf( where, chunk ), sizes.sizeOfChunk( where, chunk ) );
}

Bytes patch( Placed const& build, Storage const& storage, diag::DiagnosticSink& sink )
{
  Bytes bytes{ build.modules() };
  Patcher patcher{ build, &storage, sink };

  for ( std::uint32_t module = 0; module < build.modules().size(); ++module )
  {
    Module const& one = build.modules()[module];
    for ( std::uint32_t index = 0; index < one.sections().size(); ++index )
    {
      SectionRef const where{ .module = ModuleIndex{ module }, .section = SectionIndex{ index } };
      if ( !build.reachable().includes( where ) )
      {
        continue;
      }
      bytes.store( where, patcher.bytesOf( where ) );
    }
  }

  return bytes;
}

void Patcher::storeFrames( Storage& storage )
{
  for ( std::uint32_t index = 0; index < storage.frameCount(); ++index )
  {
    FrameIndex const frame{ index };
    if ( storage.isFrameDeclared( frame ) )
    {
      storage.storeFrame( frame, frameBytesOf( frame, *mBuild, storage, mEntries ) );
    }
  }
}

void patchFrames( Placed const& build, Storage& storage, diag::DiagnosticSink& sink )
{
  Patcher patcher{ build, &storage, sink };
  patcher.storeFrames( storage );
}

} // namespace nga::model
