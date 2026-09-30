#include "nga/model/Lnx.hpp"

#include "nga/model/Emit.hpp"
#include "nga/model/Segments.hpp"
#include "nga/model/Storage.hpp"
#include "nga/model/Transition.hpp"

#include <spdlog/fmt/fmt.h>

#include <algorithm>
#include <array>
#include <string>

namespace nga::model
{

namespace
{

/// The four geometries, in the order a finding lists them. A page is as wide as
/// the board is wired for and there are always 256 of them, the shift register
/// being eight bits.
constexpr std::array<LynxBoard, 4> BOARDS{
  LynxBoard{ .name = "64k", .pageSize = 256 },
  LynxBoard{ .name = "128k", .pageSize = 512 },
  LynxBoard{ .name = "256k", .pageSize = 1024 },
  LynxBoard{ .name = "512k", .pageSize = 2048 },
};

/// What the encryption needs of a big number library, and no more of it: the
/// modulus is 51 bytes and both exponents are constants of the format, so one
/// fixed width is the whole requirement. A number is held least significant byte
/// first, which is the order the cartridge carries it in. Exponentiation is by
/// squaring over a modular multiply that doubles and adds, so nothing here
/// divides — and long division, the one part of this arithmetic with corner
/// cases worth fearing, never appears. Every step acts on a value below twice
/// the modulus, which the one subtraction it can need brings back below it.
using Number = std::array<std::uint8_t, LYNX_CHUNK>;

/// A number as a key is written, most significant digit first. Constant
/// evaluated, so a literal of the wrong length does not compile.
constexpr Number fromHex( std::string_view digits )
{
  Number result{};
  for ( std::size_t at = 0; at < digits.size(); ++at )
  {
    char const digit = digits[at];
    std::uint8_t value = 0;
    if ( digit >= '0' && digit <= '9' )
    {
      value = static_cast<std::uint8_t>( digit - '0' );
    }
    else if ( digit >= 'a' && digit <= 'f' )
    {
      value = static_cast<std::uint8_t>( digit - 'a' + 10 );
    }
    std::size_t const position = LYNX_CHUNK - 1 - ( at / 2 );
    result[position] = static_cast<std::uint8_t>( ( result[position] << 4 ) | value );
  }
  return result;
}

/// The key the boot ROM carries the modulus of and checks a block with the
/// public exponent 3. What encrypts one is the private exponent, which has been
/// public knowledge for as long as anyone has written for this machine.
constexpr Number MODULUS =
    fromHex( "35b5a3942806d8a22695d771b23cfd561c4a19b6a3b02600365a306e3c4d63381bd41c136489364cf2ba2a58f4fee1fdac7e79" );
constexpr Number PUBLIC_EXPONENT = fromHex( std::string_view{ std::string( 101, '0' ) + "3" } );
constexpr Number PRIVATE_EXPONENT =
    fromHex( "23ce6d0d7004906c19b93a4bcc28a8e412dc11246d2019557987ab5ca818a3d3c8e3276d4270cb8021d6bda4296d47b1e5e2a3" );

/// `a += b`, and whether it carried out of the top.
bool addTo( Number& a, Number const& b )
{
  unsigned carry = 0;
  for ( std::size_t at = 0; at < LYNX_CHUNK; ++at )
  {
    unsigned const sum = unsigned{ a[at] } + b[at] + carry;
    a[at] = static_cast<std::uint8_t>( sum );
    carry = sum >> 8U;
  }
  return carry != 0;
}

/// `a -= b`, and whether it borrowed past the top.
bool subtractFrom( Number& a, Number const& b )
{
  unsigned borrow = 0;
  for ( std::size_t at = 0; at < LYNX_CHUNK; ++at )
  {
    unsigned const difference = unsigned{ a[at] } - b[at] - borrow;
    a[at] = static_cast<std::uint8_t>( difference );
    borrow = ( difference >> 8U ) & 1U;
  }
  return borrow != 0;
}

/// `a <<= 1`, and the bit that left the top.
bool doubleIt( Number& a )
{
  unsigned carry = 0;
  for ( std::size_t at = 0; at < LYNX_CHUNK; ++at )
  {
    unsigned const shifted = ( unsigned{ a[at] } << 1U ) | carry;
    a[at] = static_cast<std::uint8_t>( shifted );
    carry = shifted >> 8U;
  }
  return carry != 0;
}

bool isBelow( Number const& a, Number const& b )
{
  for ( std::size_t at = LYNX_CHUNK; at-- > 0; )
  {
    if ( a[at] != b[at] )
    {
      return a[at] < b[at];
    }
  }
  return false;
}

bool bitOf( Number const& a, std::size_t bit )
{
  return ( ( a[bit / 8] >> ( bit % 8 ) ) & 1U ) != 0;
}

/// `( a * b ) mod MODULUS`, for `a` and `b` below it: the bits of `b` from the
/// top, doubling what is held and adding `a` where the bit is set.
Number multiplyMod( Number const& a, Number const& b )
{
  Number result{};
  for ( std::size_t bit = std::size_t{ LYNX_CHUNK } * 8; bit-- > 0; )
  {
    bool const over = doubleIt( result );
    if ( over || !isBelow( result, MODULUS ) )
    {
      subtractFrom( result, MODULUS );
    }
    if ( bitOf( b, bit ) )
    {
      bool const carried = addTo( result, a );
      if ( carried || !isBelow( result, MODULUS ) )
      {
        subtractFrom( result, MODULUS );
      }
    }
  }
  return result;
}

/// `( base ^ exponent ) mod MODULUS`, for `base` below it.
Number powerMod( Number const& base, Number const& exponent )
{
  Number result{};
  result[0] = 1;
  for ( std::size_t bit = std::size_t{ LYNX_CHUNK } * 8; bit-- > 0; )
  {
    result = multiplyMod( result, result );
    if ( bitOf( exponent, bit ) )
    {
      result = multiplyMod( result, base );
    }
  }
  return result;
}

} // namespace

std::optional<std::uint8_t> lynxRotationNamed( std::string_view word )
{
  // What the header's own byte means, and the words for it: the machine is held
  // in three ways and a program that draws for one of them says which.
  if ( word == "none" )
  {
    return std::uint8_t{ 0 };
  }
  if ( word == "left" )
  {
    return std::uint8_t{ 1 };
  }
  if ( word == "right" )
  {
    return std::uint8_t{ 2 };
  }
  return std::nullopt;
}

std::span<LynxBoard const> lynxBoards()
{
  return BOARDS;
}

std::optional<LynxBoard> lynxBoardNamed( std::string_view name )
{
  for ( LynxBoard const& board : BOARDS )
  {
    if ( board.name == name )
    {
      return board;
    }
  }
  return std::nullopt;
}

std::vector<std::uint8_t> decryptLoader( std::span<std::uint8_t const> blocks )
{
  if ( blocks.empty() || blocks.size() % LYNX_CHUNK != 0 )
  {
    return {};
  }

  std::vector<std::uint8_t> out;
  std::uint8_t accumulator = 0;
  for ( std::size_t at = 0; at < blocks.size(); at += LYNX_CHUNK )
  {
    Number block{};
    std::ranges::copy( blocks.subspan( at, LYNX_CHUNK ), block.begin() );
    Number const plain = powerMod( block, PUBLIC_EXPONENT );

    // The highest byte says the block decoded to what it was made from, and the
    // ROM stops where it does not.
    if ( plain[LYNX_CHUNK - 1] != 0x15 )
    {
      return {};
    }
    for ( std::size_t byte = 0; byte < LYNX_BLOCK; ++byte )
    {
      accumulator = static_cast<std::uint8_t>( accumulator + plain[byte] );
      out.push_back( accumulator );
    }
  }
  if ( accumulator != 0 )
  {
    return {};
  }
  return out;
}

std::vector<std::uint8_t> encryptLoader( std::span<std::uint8_t const> plain )
{
  if ( plain.empty() || plain.size() > std::size_t{ LYNX_BLOCK } * LYNX_MOST_BLOCKS )
  {
    return {};
  }

  // The ROM adds each decrypted byte to the one before it, so the bytes it
  // reads are differences — and the accumulator it ends with has to be zero,
  // which the padding to fifty gives where the loader is shorter than that and
  // a loader of exactly fifty bytes has to end in one.
  std::vector<std::uint8_t> out{ 0 };
  std::uint8_t accumulator = 0;
  for ( std::size_t at = 0; at < plain.size(); at += LYNX_BLOCK )
  {
    Number block{};
    block[LYNX_CHUNK - 1] = 0x15;
    std::size_t const taken = std::min<std::size_t>( plain.size() - at, LYNX_BLOCK );
    // Over the whole fifty and not only what is there: a block short of them is
    // padded with zeroes, and a zero is a difference of nothing, which is what
    // brings the accumulator back to where the ROM wants it. The bytes those
    // zeroes decode to land past the loader, where the loader's own first act is
    // to write the rest of itself.
    for ( std::size_t byte = 0; byte < LYNX_BLOCK; ++byte )
    {
      std::uint8_t const value = byte < taken ? plain[at + byte] : std::uint8_t{ 0 };
      block[byte] = static_cast<std::uint8_t>( value - accumulator );
      accumulator = value;
    }

    Number const encrypted = powerMod( block, PRIVATE_EXPONENT );
    out.insert( out.end(), encrypted.begin(), encrypted.end() );

    // The count byte counts down, which is how the ROM knows how many to read:
    // `$FF` for one block and `$FB` for five, and it refuses anything below.
    out[0] = static_cast<std::uint8_t>( out[0] - 1 );
  }
  if ( accumulator != 0 )
  {
    return {};
  }
  return out;
}

namespace
{

/// The bootstrap, as the assembler reads it. What the boot ROM does before this
/// runs, and why it is in two parts, is docs/spec/lnx.md.
constexpr std::string_view LOADER_SOURCE =
    R"asm(; What the cartridge's first page holds in front of everything else: the fifty
; bytes the boot ROM decrypts and enters, and the loader they read in behind
; themselves. See docs/spec/lnx.md.
;
; The ROM leaves the cartridge's counter standing just past the encrypted block
; it read, so `lda RCART` here carries straight on into the loader — which is the
; whole reason the bootstrap is in two parts. Fifty bytes is what the ROM will
; decrypt, and a loader of a few hundred is what reading the image needs.

; The two addresses this needs are written out rather than named: a variant names
; its registers as it pleases and this Module is the tool's own on every one of
; them. $FCB2 is Suzy's cartridge port through the CART0 strobe, $FFF9 the map
; control register.
CHUNKS  = {}                             ; the board's page, in 256-byte chunks

.export ngaLynxLoad, ngaLynxPage, ngaLynxRun

.section absolute at $0200, root
ngaLynxStart
        lda #$08                        ; the vectors are the program's from here
        sta $FFF9                       ; on; the ROM and the hardware stay put
        ldy #0
shift
        lda $FCB2
        sta ngaLynxLoad,y
        iny
        cpy #ngaLynxLoad.runtimeSectionSize
        bne shift
        jmp ngaLynxLoad
.ends

; What the ROM will read, and what an index of one byte reaches.
.assert ngaLynxStart.runtimeSectionSize <= 50
.assert ngaLynxLoad.runtimeSectionSize <= 255

; The loader's own bytes, which hold nothing at load and so are copied by
; nobody: a Section of reservations in the zero page, where a pointer has to be.
.section zeropage, root
lynxDst    .res 2                       ; where the segment being read lands
lynxLeft   .res 2                       ; bytes of it still to come
lynxLow    .res 1                       ; bytes into the 256 being read
lynxChunks .res 1                       ; 256-byte chunks left in this page
lynxPage   .res 1                       ; the page the stream stands in
.ends

; One Section and not a Proc, because the fifty bytes above copy it by its size.
; Two of its operands are the Container's to fill — the page the load image
; begins at and the address the program starts at — and they are operands rather
; than data so that nothing here jumps through a pointer the tool cannot follow.
.section absolute, root
ngaLynxLoad
ngaLynxPage
        lda #0                          ; the page the load image begins at
        sta lynxPage
        jsr page                        ; the image begins where a page does
segment
        jsr byte
        sta lynxDst
        jsr byte
        sta lynxDst+1
        and lynxDst                     ; $FFFF closes the stream, and no
        cmp #$FF                        ; segment starts there
        beq run
        jsr byte                        ; the end address, inclusive
        sta lynxLeft
        jsr byte
        sta lynxLeft+1
        sec                             ; the count is end - start + 1
        lda lynxLeft
        sbc lynxDst
        sta lynxLeft
        lda lynxLeft+1
        sbc lynxDst+1
        sta lynxLeft+1
        inc lynxLeft
        bne bytes
        inc lynxLeft+1
bytes
        lda lynxLeft
        ora lynxLeft+1
        beq segment
        jsr byte
        ldy #0
        sta (lynxDst),y
        inc lynxDst
        bne counted
        inc lynxDst+1
counted
        lda lynxLeft
        bne low
        dec lynxLeft+1
low
        dec lynxLeft
        jmp bytes
run
ngaLynxRun
        jmp $0000                       ; where the program starts

; A = the next byte of the image. The counter only counts, and it counts inside
; its own page, so the page after this one is shifted in as the last byte of this
; one is handed back — the routine at $FE00 is the ROM's, and it takes the page
; number in A.
byte
        lda $FCB2
        inc lynxLow
        bne here
        dec lynxChunks
        bne here
        pha
        inc lynxPage
        jsr page
        pla
here
        rts

; The page in lynxPage, from its first byte.
page
        lda #CHUNKS
        sta lynxChunks
        lda #0
        sta lynxLow
        lda lynxPage
        jmp $FE00                       ; the ROM shifts the eight bits in
.ends
)asm";

} // namespace

std::string lynxLoaderSource( std::uint32_t pageSize )
{
  return fmt::format( LOADER_SOURCE, pageSize / 256 );
}

void addLynxLoader( Project& project, diag::SourceManager& sources )
{
  std::uint32_t const pageSize = lynxBoardNamed( project.cartridge.value_or( "" ) ).value_or( BOARDS[2] ).pageSize;
  diag::FileId const file = sources.addFile( "<nga>/lynx.asm", lynxLoaderSource( pageSize ) );
  ModuleIndex const index{ static_cast<std::uint32_t>( project.modules.size() ) };
  project.modules.push_back( ProjectModule{
      .name = std::string{ LYNX_LOADER_MODULE }, .file = file, .residency = {}, .generated = Generated::NONE } );
  for ( Phase& phase : project.phases.phases )
  {
    phase.needs.push_back( index );
  }
  deriveResidency( project );
}

namespace
{

/// Where the loader's fifty bytes are decrypted to and entered, which is the
/// boot ROM's and not ours to choose.
constexpr std::uint32_t LOADER_AT = 0x0200;

/// The page storage begins at: the one after the loader's, which is a constant
/// the Container and the driver both hold, since neither can tell the other —
/// the same arrangement the diskette has with sector four.
constexpr std::uint32_t FIRST_STORAGE_PAGE = 1;

} // namespace

LnxFile emitLnx( Patched const& build, diag::DiagnosticSink& sink )
{
  Project const& project = build.project();
  GlobalSymbols const& symbols = build.symbols();
  Sizes const& sizes = build.sizes();
  Layout const& layout = build.layout();
  Target const& target = build.target();

  std::optional<LynxBoard> const board = lynxBoardNamed( project.cartridge.value_or( "" ) );
  if ( !board.has_value() )
  {
    // The Project file refuses a name no board has, so this is the Container
    // being asked for without one at all.
    return {};
  }
  std::uint32_t const page = board->pageSize;
  std::uint32_t const whole = page * LYNX_PAGES;

  // The loader's own Sections go into page zero and not into the load image:
  // the ROM reads the first fifty bytes and those read the rest, so nothing
  // loads them.
  std::optional<ModuleIndex> loader;
  for ( std::uint32_t at = 0; at < project.modules.size(); ++at )
  {
    if ( project.modules[at].name == LYNX_LOADER_MODULE )
    {
      loader = ModuleIndex{ at };
    }
  }
  if ( !loader.has_value() )
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::LNX_WITHOUT_LOADER ).sortedBy( "loader" ) );
    return {};
  }

  std::vector<std::uint8_t> encrypted;
  std::vector<std::uint8_t> body;
  std::uint32_t bodyAt = 0;
  std::vector<Resident> image;
  for ( Resident const& one : residentSections( build, {} ) )
  {
    if ( one.where.module.value != loader->value )
    {
      image.push_back( one );
      continue;
    }
    if ( one.address == LOADER_AT )
    {
      encrypted = encryptLoader( one.content );
      if ( encrypted.empty() )
      {
        sink.add( diag::diagnostic( diag::DiagnosticId::LNX_LOADER_TOO_LARGE )
                      .arg( "available", LYNX_BLOCK )
                      .arg( "required", static_cast<std::uint32_t>( one.content.size() ) )
                      .sortedBy( "loader" ) );
        return {};
      }
      continue;
    }
    body.assign( one.content.begin(), one.content.end() );
    bodyAt = one.address;
  }
  if ( encrypted.empty() || body.empty() )
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::LNX_WITHOUT_LOADER ).sortedBy( "loader" ) );
    return {};
  }

  // The load image: the Sections the entry Phase needs, as the segments a `.xex`
  // carries, and `$FFFF` to close the stream, which no segment starts at.
  SegmentWriter out;
  for ( Resident const& one : image )
  {
    out.segment( one.address, one.content );
  }
  std::vector<std::uint8_t> stream = std::move( out ).take();
  stream.push_back( 0xFF );
  stream.push_back( 0xFF );

  // Storage begins at a fixed page because the driver reaches a unit by number
  // and nothing tells it where storage is; the image begins on the first page
  // after the storage the program came to **use**, so that the loader has a page
  // number to shift in and no offset to read its way up to.
  std::uint32_t used = 0;
  for ( Stored const& one : storedImages( build ) )
  {
    used = std::max( used, target.positionOf( one.at ) + static_cast<std::uint32_t>( one.form.size() ) );
  }
  std::uint32_t const storagePages = ( used + page - 1 ) / page;
  std::uint32_t const imagePage = FIRST_STORAGE_PAGE + storagePages;

  std::size_t const bootstrap = encrypted.size() + body.size();
  if ( bootstrap > page )
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::LNX_BOOTSTRAP_TOO_LARGE )
                  .arg( "available", page - static_cast<std::uint32_t>( encrypted.size() ) )
                  .arg( "required", static_cast<std::uint32_t>( body.size() ) )
                  .sortedBy( "loader" ) );
    return {};
  }

  std::uint32_t const required = ( imagePage * page ) + static_cast<std::uint32_t>( stream.size() );
  if ( imagePage >= LYNX_PAGES || required > whole )
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::LNX_DOES_NOT_FIT )
                  .arg( "name", std::string{ board->name } )
                  .arg( "available", whole )
                  .arg( "required", required )
                  .sortedBy( "size" ) );
    return {};
  }

  diag::SeverityPolicy quietPolicy;
  diag::DiagnosticSink quiet{ quietPolicy };
  bool const alreadyReported = isEnteredByAnEdge( project.phases, project.phases.entry );
  std::optional<std::uint32_t> const run =
      entryAddressOf( build, project.phases.entry, alreadyReported ? quiet : sink );
  if ( !run.has_value() )
  {
    return {};
  }

  // Two operands of the loader are the Container's to fill, because neither is
  // known until every image has been placed: the page the load image begins at,
  // and the address the program starts at. Each Label stands on the instruction
  // whose operand it is, so the byte to write is the one after it.
  auto const fill = [&]( std::string_view name, std::span<std::uint8_t const> value )
  {
    std::optional<std::uint32_t> const at = addressOfExported( symbols, name, sizes, layout );
    if ( !at.has_value() || *at < bodyAt || *at + value.size() >= bodyAt + body.size() )
    {
      return false;
    }
    std::ranges::copy( value, body.begin() + static_cast<std::ptrdiff_t>( *at - bodyAt ) + 1 );
    return true;
  };
  std::array<std::uint8_t, 1> const pageByte{ static_cast<std::uint8_t>( imagePage ) };
  std::array<std::uint8_t, 2> const runBytes{ static_cast<std::uint8_t>( *run & 0xFF ),
                                              static_cast<std::uint8_t>( ( *run >> 8 ) & 0xFF ) };
  if ( !fill( LYNX_PAGE_NAME, pageByte ) || !fill( LYNX_RUN_NAME, runBytes ) )
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::LNX_WITHOUT_LOADER ).sortedBy( "loader" ) );
    return {};
  }

  // Bytes no part of the program accounts for are `$FF`, which is what an erased
  // device holds: an image burnt onto one then differs from it only where the
  // program is.
  // The whole board and not the pages a program happens to fill: a reader takes
  // the geometry from the header's page size, or from the file's own length
  // divided by 256 where there is no header, and either way it wants 256 pages
  // to be there. One that is given fewer cannot say which of the two numbers is
  // wrong, and the one this tool has met refuses to map the cartridge at all.
  std::vector<std::uint8_t> file( whole, 0xFF );
  std::ranges::copy( encrypted, file.begin() );
  std::ranges::copy( body, file.begin() + static_cast<std::ptrdiff_t>( encrypted.size() ) );
  for ( Stored const& one : storedImages( build ) )
  {
    std::size_t const at = ( FIRST_STORAGE_PAGE * page ) + target.positionOf( one.at );
    std::ranges::copy( one.form, file.begin() + static_cast<std::ptrdiff_t>( at ) );
  }
  std::ranges::copy( stream, file.begin() + ( static_cast<std::ptrdiff_t>( imagePage ) * page ) );

  if ( project.container == Container::LYX )
  {
    // No header at all, and so the whole of the board: a reader with none
    // divides the file's length by 256 to learn the page size, so anything
    // shorter would describe a different geometry — see docs/spec/lnx.md.
    return LnxFile{ .bytes = std::move( file ) };
  }

  std::vector<std::uint8_t> header( LNX_HEADER_SIZE, 0 );
  header[0] = 'L';
  header[1] = 'Y';
  header[2] = 'N';
  header[3] = 'X';
  header[4] = static_cast<std::uint8_t>( page & 0xFF );
  header[5] = static_cast<std::uint8_t>( ( page >> 8 ) & 0xFF );
  header[8] = 1;
  std::ranges::copy( project.cartridgeName, header.begin() + 10 );
  std::ranges::copy( project.cartridgeMaker, header.begin() + 10 + LNX_NAME_SIZE );
  header[10 + LNX_NAME_SIZE + LNX_MAKER_SIZE] = project.cartridgeRotation;
  header.insert( header.end(), file.begin(), file.end() );
  return LnxFile{ .bytes = std::move( header ) };
}

} // namespace nga::model
