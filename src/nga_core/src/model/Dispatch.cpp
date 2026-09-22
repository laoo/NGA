#include "nga/model/Dispatch.hpp"

namespace nga::model
{
namespace
{

/// `asl`, `tax` and `jmp (abs,x)`; and `tax`, `lda abs,x`, `pha`, `lda abs,x`,
/// `pha`, `rts`. Here rather than at the writer because Size needs them
/// before a byte is written, as a Transition's do.
constexpr std::uint32_t INDEXED_HEADER_SIZE = 5;
constexpr std::uint32_t RETURNED_HEADER_SIZE = 10;

} // namespace

DispatchForm dispatchFormOf( Cpu cpu, std::size_t targets )
{
  return cpu == Cpu::WDC65SC02 && targets <= MOST_INDEXED_TARGETS ? DispatchForm::INDEXED : DispatchForm::RETURNED;
}

std::uint32_t sizeOfDispatch( DispatchForm form, std::size_t targets )
{
  return tableOffsetOf( form ) + ( 2 * static_cast<std::uint32_t>( targets ) );
}

std::uint32_t tableOffsetOf( DispatchForm form )
{
  return form == DispatchForm::INDEXED ? INDEXED_HEADER_SIZE : RETURNED_HEADER_SIZE;
}

} // namespace nga::model
