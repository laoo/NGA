// A port of the ZX0 reference compressor, https://github.com/einar-saukas/ZX0,
// files optimize.c, compress.c and memory.c at ecde3a2ae05061fe06469ed46df81a33b7de7d86:
//
//   (c) Copyright 2021 by Einar Saukas. All rights reserved.
//
//   Redistribution and use in source and binary forms, with or without
//   modification, are permitted provided that the following conditions are met:
//       * Redistributions of source code must retain the above copyright
//         notice, this list of conditions and the following disclaimer.
//       * Redistributions in binary form must reproduce the above copyright
//         notice, this list of conditions and the following disclaimer in the
//         documentation and/or other materials provided with the distribution.
//       * The name of its author may not be used to endorse or promote products
//         derived from this software without specific prior written permission.
//
//   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
//   ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
//   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
//   DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
//   DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
//   (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
//   LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
//   ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
//   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
//   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// What changed in the port: no global state, no progress on standard output,
// no exit on failure, blocks in a vector addressed by index instead of pointers
// from malloc, and the forward, non-classic mode alone — the one the 6502 half
// decodes. The parse and the bit layout are the reference's.

#include "nga/model/Zx0.hpp"

#include <algorithm>
#include <cstddef>

namespace nga::model
{

namespace
{

/// The last offset a stream starts with, and the farthest back a match may
/// reach — the format's own limits, not the Target's.
constexpr int INITIAL_OFFSET = 1;
constexpr int MAX_OFFSET = 32640;

constexpr int NONE = -1;

/// One block of the parse: a run of literals when `offset` is zero, a match
/// otherwise, ending at `index`, chained to the block before it. `bits` is
/// the cost of the parse up to here.
struct Block
{
  int chain = NONE;
  int ghost = NONE;
  int bits = 0;
  int index = 0;
  int offset = 0;
  int references = 0;
};

/// Blocks with reference counts and a free list, as the reference keeps
/// them: a parse touches the input times the window many blocks and keeps
/// alive only the chains still reachable, so without reclaiming the dead ones
/// a Bank-sized input would not fit in memory.
class Blocks
{
public:
  int allocate( int bits, int index, int offset, int chain )
  {
    int id;
    if ( mGhost != NONE )
    {
      id = mGhost;
      mGhost = slot( id ).ghost;
      int const old = slot( id ).chain;
      if ( old != NONE && --slot( old ).references == 0 )
      {
        slot( old ).ghost = mGhost;
        mGhost = old;
      }
    }
    else
    {
      id = static_cast<int>( mBlocks.size() );
      mBlocks.emplace_back();
    }

    Block& block = slot( id );
    block.bits = bits;
    block.index = index;
    block.offset = offset;
    if ( chain != NONE )
    {
      ++slot( chain ).references;
    }
    block.chain = chain;
    block.references = 0;
    return id;
  }

  void assign( int& holder, int chain )
  {
    ++slot( chain ).references;
    if ( holder != NONE && --slot( holder ).references == 0 )
    {
      slot( holder ).ghost = mGhost;
      mGhost = holder;
    }
    holder = chain;
  }

  [[nodiscard]] Block const& at( int id ) const
  {
    return mBlocks[static_cast<std::size_t>( id )];
  }

private:
  [[nodiscard]] Block& slot( int id )
  {
    return mBlocks[static_cast<std::size_t>( id )];
  }

  std::vector<Block> mBlocks;
  int mGhost = NONE;
};

int offsetCeiling( int index, int offsetLimit )
{
  return std::clamp( index, INITIAL_OFFSET, offsetLimit );
}

int eliasGammaBits( int value )
{
  int bits = 1;
  while ( ( value >>= 1 ) != 0 )
  {
    bits += 2;
  }
  return bits;
}

/// The reference's `optimize`: the cheapest parse of the input, as the block
/// that ends it, chained back to a fake block before the first byte.
int optimize( Blocks& blocks, std::span<std::uint8_t const> input, int offsetLimit )
{
  int const size = static_cast<int>( input.size() );
  int maxOffset = offsetCeiling( size - 1, offsetLimit );

  std::vector<int> lastLiteral( static_cast<std::size_t>( maxOffset ) + 1, NONE );
  std::vector<int> lastMatch( static_cast<std::size_t>( maxOffset ) + 1, NONE );
  std::vector<int> optimal( static_cast<std::size_t>( size ), NONE );
  std::vector<int> matchLength( static_cast<std::size_t>( maxOffset ) + 1, 0 );
  std::vector<int> bestLength( static_cast<std::size_t>( size ), 0 );
  if ( size > 2 )
  {
    bestLength[2] = 2;
  }

  blocks.assign( lastMatch[INITIAL_OFFSET], blocks.allocate( -1, -1, INITIAL_OFFSET, NONE ) );

  for ( int index = 0; index < size; ++index )
  {
    int bestLengthSize = 2;
    maxOffset = offsetCeiling( index, offsetLimit );
    for ( int offset = 1; offset <= maxOffset; ++offset )
    {
      auto const o = static_cast<std::size_t>( offset );
      if ( index != 0 && index >= offset &&
           input[static_cast<std::size_t>( index )] == input[static_cast<std::size_t>( index - offset )] )
      {
        // Copy from the last offset.
        if ( lastLiteral[o] != NONE )
        {
          int const length = index - blocks.at( lastLiteral[o] ).index;
          int const bits = blocks.at( lastLiteral[o] ).bits + 1 + eliasGammaBits( length );
          blocks.assign( lastMatch[o], blocks.allocate( bits, index, offset, lastLiteral[o] ) );
          auto const i = static_cast<std::size_t>( index );
          if ( optimal[i] == NONE || blocks.at( optimal[i] ).bits > bits )
          {
            blocks.assign( optimal[i], lastMatch[o] );
          }
        }
        // Copy from a new offset.
        if ( ++matchLength[o] > 1 )
        {
          if ( bestLengthSize < matchLength[o] )
          {
            auto const bl = static_cast<std::size_t>( bestLength[static_cast<std::size_t>( bestLengthSize )] );
            int bits = blocks.at( optimal[static_cast<std::size_t>( index ) - bl] ).bits +
                       eliasGammaBits( static_cast<int>( bl ) - 1 );
            do
            {
              ++bestLengthSize;
              auto const bs = static_cast<std::size_t>( bestLengthSize );
              int const bits2 = blocks.at( optimal[static_cast<std::size_t>( index ) - bs] ).bits +
                                eliasGammaBits( bestLengthSize - 1 );
              if ( bits2 <= bits )
              {
                bestLength[bs] = bestLengthSize;
                bits = bits2;
              }
              else
              {
                bestLength[bs] = bestLength[bs - 1];
              }
            } while ( bestLengthSize < matchLength[o] );
          }
          int const length = bestLength[static_cast<std::size_t>( matchLength[o] )];
          int const bits = blocks.at( optimal[static_cast<std::size_t>( index - length )] ).bits + 8 +
                           eliasGammaBits( ( ( offset - 1 ) / 128 ) + 1 ) + eliasGammaBits( length - 1 );
          if ( lastMatch[o] == NONE || blocks.at( lastMatch[o] ).index != index ||
               blocks.at( lastMatch[o] ).bits > bits )
          {
            blocks.assign(
                lastMatch[o],
                blocks.allocate( bits, index, offset, optimal[static_cast<std::size_t>( index - length )] ) );
            auto const i = static_cast<std::size_t>( index );
            if ( optimal[i] == NONE || blocks.at( optimal[i] ).bits > bits )
            {
              blocks.assign( optimal[i], lastMatch[o] );
            }
          }
        }
      }
      else
      {
        // Copy literals.
        matchLength[o] = 0;
        if ( lastMatch[o] != NONE )
        {
          int const length = index - blocks.at( lastMatch[o] ).index;
          int const bits = blocks.at( lastMatch[o] ).bits + 1 + eliasGammaBits( length ) + ( length * 8 );
          blocks.assign( lastLiteral[o], blocks.allocate( bits, index, 0, lastMatch[o] ) );
          auto const i = static_cast<std::size_t>( index );
          if ( optimal[i] == NONE || blocks.at( optimal[i] ).bits > bits )
          {
            blocks.assign( optimal[i], lastLiteral[o] );
          }
        }
      }
    }
  }

  return optimal[static_cast<std::size_t>( size ) - 1];
}

/// The stream as the reference writes it: bytes in order, and bits packed
/// into a byte reserved where the first of each eight was written. The one
/// bit after an offset's low byte goes into that byte's bit 0 instead —
/// `backtrack` — which is also how the first block's indicator is dropped,
/// since the parse always begins with literals.
class Writer
{
public:
  void byte( std::uint8_t value )
  {
    mOut.push_back( value );
  }

  void bit( bool value )
  {
    if ( mBacktrack )
    {
      if ( value && !mOut.empty() )
      {
        mOut.back() |= 1;
      }
      mBacktrack = false;
      return;
    }
    if ( mMask == 0 )
    {
      mMask = 128;
      mHolder = mOut.size();
      mOut.push_back( 0 );
    }
    if ( value )
    {
      mOut[mHolder] |= static_cast<std::uint8_t>( mMask );
    }
    mMask >>= 1;
  }

  void backtrack()
  {
    mBacktrack = true;
  }

  void gamma( int value, bool inverted )
  {
    int i = 2;
    while ( i <= value )
    {
      i <<= 1;
    }
    i >>= 1;
    while ( ( i >>= 1 ) > 0 )
    {
      bit( false );
      bool const data = ( value & i ) != 0;
      bit( inverted ? !data : data );
    }
    bit( true );
  }

  std::vector<std::uint8_t> take() &&
  {
    return std::move( mOut );
  }

private:
  std::vector<std::uint8_t> mOut;
  std::size_t mHolder = 0;
  int mMask = 0;
  bool mBacktrack = true;
};

} // namespace

std::vector<std::uint8_t> encodeZx0( std::span<std::uint8_t const> from )
{
  if ( from.empty() )
  {
    return {};
  }

  Blocks blocks;
  int const last = optimize( blocks, from, MAX_OFFSET );

  // The chain runs from the last block back to the fake one; the stream is
  // written the other way round.
  std::vector<int> order;
  for ( int id = last; id != NONE; id = blocks.at( id ).chain )
  {
    order.push_back( id );
  }
  std::ranges::reverse( order );

  Writer out;
  int lastOffset = INITIAL_OFFSET;
  std::size_t at = 0;
  for ( std::size_t step = 1; step < order.size(); ++step )
  {
    Block const& previous = blocks.at( order[step - 1] );
    Block const& block = blocks.at( order[step] );
    int const length = block.index - previous.index;

    if ( block.offset == 0 )
    {
      out.bit( false );
      out.gamma( length, false );
      for ( int i = 0; i < length; ++i )
      {
        out.byte( from[at++] );
      }
    }
    else if ( block.offset == lastOffset )
    {
      out.bit( false );
      out.gamma( length, false );
      at += static_cast<std::size_t>( length );
    }
    else
    {
      out.bit( true );
      out.gamma( ( ( block.offset - 1 ) / 128 ) + 1, true );
      out.byte( static_cast<std::uint8_t>( ( 127 - ( ( block.offset - 1 ) % 128 ) ) << 1 ) );
      out.backtrack();
      out.gamma( length - 1, false );
      at += static_cast<std::size_t>( length );
      lastOffset = block.offset;
    }
  }

  // The end marker: a new offset whose MSB is 256.
  out.bit( true );
  out.gamma( 256, true );
  return std::move( out ).take();
}

} // namespace nga::model
