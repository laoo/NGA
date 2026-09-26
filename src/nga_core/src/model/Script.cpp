#include "nga/diag/Diagnostic.hpp"
#include "nga/model/Generator.hpp"

#include "quickjs.h"

#include <spdlog/spdlog.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace nga::model
{

namespace
{

/// How much work a script is given before it is stopped. Counted in calls to
/// the engine's interrupt handler rather than in seconds, so that a slow machine
/// does not report an error a fast one does not — see
/// docs/decisions/0069-generators.md.
///
/// The engine calls the handler every few thousand operations, so the figure is
/// measured rather than guessed: building 64 KB of data in a loop over every
/// byte costs 20, and building 1 MB with two operations a byte and 256 Sections
/// costs 315. The allowance below is sixteen times the second of those, and a
/// script that will not finish is stopped in about a second. It is a tuning
/// constant and not a contract; `--verbose` reports what a script spent.
constexpr int WORK_ALLOWED = 5000;

/// What the engine may allocate. Generous for a script that builds a screen's
/// worth of data, and far below what a runaway one would want.
constexpr std::size_t MEMORY_ALLOWED = 128U << 20U;

/// A `JSValue` that frees itself, because the engine asserts at shutdown that
/// nothing was left behind — which makes a leak a failed test rather than a slow
/// truth.
class Value
{
public:
  Value() = default;

  Value( JSContext* ctx, JSValue value ) : mCtx( ctx ), mValue( value ) {}

  Value( Value const& ) = delete;
  Value& operator=( Value const& ) = delete;

  Value( Value&& other ) noexcept : mCtx( other.mCtx ), mValue( other.mValue )
  {
    other.mCtx = nullptr;
    other.mValue = JS_UNDEFINED;
  }

  Value& operator=( Value&& other ) noexcept
  {
    if ( this != &other )
    {
      free();
      mCtx = other.mCtx;
      mValue = other.mValue;
      other.mCtx = nullptr;
      other.mValue = JS_UNDEFINED;
    }
    return *this;
  }

  ~Value()
  {
    free();
  }

  [[nodiscard]] JSValue get() const
  {
    return mValue;
  }

  [[nodiscard]] bool isException() const
  {
    return JS_IsException( mValue );
  }

  /// Hands the value over to something that takes ownership of it — the engine
  /// consumes what it evaluates — so that this handle frees nothing.
  [[nodiscard]] JSValue release()
  {
    JSValue const value = mValue;
    mCtx = nullptr;
    mValue = JS_UNDEFINED;
    return value;
  }

private:
  void free()
  {
    if ( mCtx != nullptr )
    {
      JS_FreeValue( mCtx, mValue );
      mCtx = nullptr;
      mValue = JS_UNDEFINED;
    }
  }

  JSContext* mCtx = nullptr;
  JSValue mValue = JS_UNDEFINED;
};

/// What a script reported: its own words, and the line it said them at.
struct Report
{
  bool isError = false;
  std::string message{};
  std::string where{};
};

/// What `nga.read` reaches, and what it recorded: every path read is one of the
/// build's inputs, which is what makes a generator a pure function of its
/// arguments and those bytes.
struct Host
{
  GeneratorFiles const* files = nullptr;
  std::vector<std::string> read{};
  std::vector<Report> reports{};
  int work = 0;
};

int onInterrupt( JSRuntime* /*rt*/, void* opaque )
{
  auto* host = static_cast<Host*>( opaque );
  return ++host->work > WORK_ALLOWED ? 1 : 0;
}

JSValue readFile( JSContext* ctx, JSValueConst /*self*/, int argc, JSValueConst* argv )
{
  auto* host = static_cast<Host*>( JS_GetContextOpaque( ctx ) );
  if ( argc < 1 )
  {
    return JS_ThrowTypeError( ctx, "nga.read takes the path of a file" );
  }

  char const* text = JS_ToCString( ctx, argv[0] );
  if ( text == nullptr )
  {
    return JS_EXCEPTION;
  }
  std::string const path{ text };
  JS_FreeCString( ctx, text );

  std::string resolved;
  std::optional<std::string> const contents = host->files->read( path, resolved );
  if ( !contents.has_value() )
  {
    // Thrown rather than reported, so that the failure lands at the line of the
    // script that asked, and so that a script may answer for it itself.
    return JS_ThrowInternalError( ctx, "cannot read `%s`", resolved.c_str() );
  }

  // Every path read is one of the build's inputs, which is what makes a
  // generator a pure function of its arguments and those bytes. Nothing
  // consumes the set yet; `--verbose` is where it can be seen.
  host->read.push_back( resolved );
  spdlog::debug( "script read {}", resolved );
  return JS_NewUint8ArrayCopy( ctx, reinterpret_cast<std::uint8_t const*>( contents->data() ), contents->size() );
}

std::string stringOf( JSContext* ctx, JSValue value )
{
  char const* text = JS_ToCString( ctx, value );
  std::string result{ text == nullptr ? "" : text };
  JS_FreeCString( ctx, text );
  return result;
}

/// The first frame of the stack as it stands now — `tools/tiles.js:41` — which
/// is where a script said what it said. There is no API for the position of a
/// call, so an Error is made for the stack the engine fills in and thrown at
/// nobody.
std::string whereNow( JSContext* ctx )
{
  Value const here{ ctx, JS_NewError( ctx ) };
  Value const stack{ ctx, JS_GetPropertyStr( ctx, here.get(), "stack" ) };
  if ( JS_IsUndefined( stack.get() ) )
  {
    return {};
  }

  // `    at warn (native)\n    at default (tools/tiles.js:6:34)\n`: the first
  // frame is the function the script called, which is this one, so the first
  // frame that is not the engine's own is the line wanted.
  std::string const frames = stringOf( ctx, stack.get() );
  for ( std::size_t at = 0; at < frames.size(); )
  {
    std::size_t const end = frames.find( '\n', at );
    std::string_view const frame{ frames.data() + at, ( end == std::string::npos ? frames.size() : end ) - at };
    at = end == std::string::npos ? frames.size() : end + 1;

    std::size_t const open = frame.find( '(' );
    std::size_t const close = frame.find( ')', open );
    if ( open == std::string_view::npos || close == std::string_view::npos )
    {
      continue;
    }
    std::string_view const where = frame.substr( open + 1, close - open - 1 );
    if ( where != "native" )
    {
      return std::string{ where };
    }
  }
  return {};
}

JSValue reportError( JSContext* ctx, JSValueConst /*self*/, int argc, JSValueConst* argv )
{
  auto* host = static_cast<Host*>( JS_GetContextOpaque( ctx ) );
  host->reports.push_back( Report{
      .isError = true, .message = argc < 1 ? std::string{} : stringOf( ctx, argv[0] ), .where = whereNow( ctx ) } );
  return JS_UNDEFINED;
}

JSValue reportWarning( JSContext* ctx, JSValueConst /*self*/, int argc, JSValueConst* argv )
{
  auto* host = static_cast<Host*>( JS_GetContextOpaque( ctx ) );
  host->reports.push_back( Report{
      .isError = false, .message = argc < 1 ? std::string{} : stringOf( ctx, argv[0] ), .where = whereNow( ctx ) } );
  return JS_UNDEFINED;
}

/// The message a thrown value makes, with the first frame of its stack behind
/// it: the script's own line is what the author needs, and the call site is
/// where the finding stands.
std::string messageOf( JSContext* ctx )
{
  Value const thrown{ ctx, JS_GetException( ctx ) };
  std::string message = stringOf( ctx, thrown.get() );

  Value const stack{ ctx, JS_GetPropertyStr( ctx, thrown.get(), "stack" ) };
  if ( !JS_IsUndefined( stack.get() ) )
  {
    std::string const frames = stringOf( ctx, stack.get() );
    std::size_t const at = frames.find( "at " );
    if ( at != std::string::npos )
    {
      std::size_t const end = frames.find( '\n', at );
      message += ", " + frames.substr( at, end == std::string::npos ? end : end - at );
    }
  }
  return message;
}

/// The engine, shut in: the intrinsics a generator needs and not one more.
///
/// There is no clock, because `JS_AddIntrinsicDate` is not called and so `Date`
/// is not in the context at all; no randomness, because `Math.random` is deleted
/// from the one object that had it; and no filesystem, network, environment or
/// directory listing, because `quickjs-libc.c` is not compiled in and the only
/// way out is `nga.read`. See docs/spec/generators.md.
class Engine
{
public:
  explicit Engine( Host& host )
  {
    mRuntime = JS_NewRuntime();
    JS_SetMemoryLimit( mRuntime, MEMORY_ALLOWED );
    JS_SetInterruptHandler( mRuntime, onInterrupt, &host );

    mContext = JS_NewContextRaw( mRuntime );
    JS_AddIntrinsicBaseObjects( mContext );
    JS_AddIntrinsicEval( mContext );
    JS_AddIntrinsicRegExpCompiler( mContext );
    JS_AddIntrinsicRegExp( mContext );
    JS_AddIntrinsicJSON( mContext );
    JS_AddIntrinsicMapSet( mContext );
    JS_AddIntrinsicTypedArrays( mContext );

    // A module is evaluated through a promise whether or not it awaits, so the
    // intrinsic is the engine's own requirement and not a capability: there is
    // no timer and no I/O for a script to await.
    JS_AddIntrinsicPromise( mContext );

    JS_SetContextOpaque( mContext, &host );

    Value const global{ mContext, JS_GetGlobalObject( mContext ) };
    Value const math{ mContext, JS_GetPropertyStr( mContext, global.get(), "Math" ) };
    JS_DeleteProperty( mContext, math.get(), JS_NewAtom( mContext, "random" ), 0 );
  }

  Engine( Engine const& ) = delete;
  Engine( Engine&& ) = delete;
  Engine& operator=( Engine const& ) = delete;
  Engine& operator=( Engine&& ) = delete;

  ~Engine()
  {
    JS_FreeContext( mContext );
    JS_FreeRuntime( mRuntime );
  }

  [[nodiscard]] JSContext* context() const
  {
    return mContext;
  }

private:
  JSRuntime* mRuntime = nullptr;
  JSContext* mContext = nullptr;
};

/// Reads the value a script returned into the description the emitter takes.
///
/// Every shape it cannot use is named the way `checkModule` names one —
/// `sections[1].items[3]` — because a fault in a script has to be findable
/// without a debugger.
class Reader
{
public:
  explicit Reader( JSContext* ctx ) : mCtx( ctx ) {}

  [[nodiscard]] std::optional<GeneratedModule> read( JSValue returned, std::string const& source )
  {
    if ( !JS_IsObject( returned ) || JS_IsArray( returned ) )
    {
      return fail( "the value returned is not an object" );
    }

    GeneratedModule description;
    description.source = source;

    if ( std::optional<std::string> const space = stringAt( returned, "namespace", "namespace" ) )
    {
      description.namespaceName = space;
    }
    if ( !mFault.empty() )
    {
      return std::nullopt;
    }

    Value const sections{ mCtx, JS_GetPropertyStr( mCtx, returned, "sections" ) };
    if ( !JS_IsArray( sections.get() ) )
    {
      return fail( "sections: not an array" );
    }

    std::int64_t count = 0;
    Value const length{ mCtx, JS_GetPropertyStr( mCtx, sections.get(), "length" ) };
    JS_ToInt64( mCtx, &count, length.get() );

    for ( std::int64_t at = 0; at < count; ++at )
    {
      Value const section{ mCtx, JS_GetPropertyUint32( mCtx, sections.get(), static_cast<std::uint32_t>( at ) ) };
      std::string const where = "sections[" + std::to_string( at ) + "]";
      std::optional<GeneratedSection> read = readSection( section.get(), where );
      if ( !read.has_value() )
      {
        return std::nullopt;
      }
      description.sections.push_back( std::move( *read ) );
    }

    return description;
  }

  [[nodiscard]] std::string const& fault() const
  {
    return mFault;
  }

private:
  std::nullopt_t fail( std::string what )
  {
    if ( mFault.empty() )
    {
      mFault = std::move( what );
    }
    return std::nullopt;
  }

  [[nodiscard]] std::optional<GeneratedSection> readSection( JSValue section, std::string const& where )
  {
    if ( !JS_IsObject( section ) )
    {
      return fail( where + ": not an object" );
    }

    GeneratedSection read;
    std::optional<std::string> const name = stringAt( section, "name", where + ".name" );
    if ( !name.has_value() )
    {
      return fail( mFault.empty() ? where + ".name: a section is named" : mFault );
    }
    read.name = *name;

    if ( std::optional<std::string> const placement = stringAt( section, "placement", where + ".placement" ) )
    {
      if ( *placement == "zeropage" )
      {
        read.placement = PlacementClass::ZEROPAGE;
      }
      else if ( *placement != "absolute" )
      {
        return fail( where + ".placement: `" + *placement + "` is neither `absolute` nor `zeropage`" );
      }
    }

    read.pinnedAddress = numberAt( section, "at", where + ".at" );
    read.alignment = numberAt( section, "align", where + ".align" );
    read.boundary = numberAt( section, "within", where + ".within" );
    read.pane = stringAt( section, "in", where + ".in" );
    read.movable = flagAt( section, "movable", where + ".movable" );
    read.readOnly = flagAt( section, "readonly", where + ".readonly" );
    read.root = flagAt( section, "root", where + ".root" );
    read.temporary = flagAt( section, "temporary", where + ".temporary" );
    if ( !mFault.empty() )
    {
      return std::nullopt;
    }

    Value const items{ mCtx, JS_GetPropertyStr( mCtx, section, "items" ) };
    if ( JS_IsUndefined( items.get() ) )
    {
      return read;
    }
    if ( !JS_IsArray( items.get() ) )
    {
      return fail( where + ".items: not an array" );
    }

    std::int64_t count = 0;
    Value const length{ mCtx, JS_GetPropertyStr( mCtx, items.get(), "length" ) };
    JS_ToInt64( mCtx, &count, length.get() );

    for ( std::int64_t at = 0; at < count; ++at )
    {
      Value const item{ mCtx, JS_GetPropertyUint32( mCtx, items.get(), static_cast<std::uint32_t>( at ) ) };
      std::optional<GeneratedItem> one = readItem( item.get(), where + ".items[" + std::to_string( at ) + "]" );
      if ( !one.has_value() )
      {
        return std::nullopt;
      }
      read.items.push_back( std::move( *one ) );
    }
    return read;
  }

  /// An item is bytes, words, a reservation or a Constant, and any of them may
  /// carry the label emitted in front of it — see
  /// docs/spec/generators.md#what-is-returned. Bytes written bare are the
  /// common case and need no wrapper.
  [[nodiscard]] std::optional<GeneratedItem> readItem( JSValue item, std::string const& where )
  {
    if ( !JS_IsObject( item ) )
    {
      return fail( where + ": not a kind of item" );
    }

    if ( std::optional<std::vector<std::uint8_t>> bare = bytesOf( item, where ) )
    {
      return GeneratedItem{ .label = std::nullopt, .content = GeneratedBytes{ .bytes = std::move( *bare ) } };
    }
    if ( !mFault.empty() )
    {
      return std::nullopt;
    }

    GeneratedItem read;
    read.label = stringAt( item, "label", where + ".label" );
    if ( !mFault.empty() )
    {
      return std::nullopt;
    }

    if ( has( item, "bytes" ) )
    {
      Value const named{ mCtx, JS_GetPropertyStr( mCtx, item, "bytes" ) };
      std::optional<std::vector<std::uint8_t>> bytes = bytesOf( named.get(), where + ".bytes" );
      if ( !bytes.has_value() )
      {
        return fail( mFault.empty() ? where + ".bytes: not bytes" : mFault );
      }
      read.content = GeneratedBytes{ .bytes = std::move( *bytes ) };
      return read;
    }

    if ( has( item, "words" ) )
    {
      Value const named{ mCtx, JS_GetPropertyStr( mCtx, item, "words" ) };
      std::optional<std::vector<std::uint8_t>> bytes = wordsOf( named.get(), where + ".words" );
      if ( !bytes.has_value() )
      {
        return fail( mFault.empty() ? where + ".words: not words" : mFault );
      }
      read.content = GeneratedBytes{ .bytes = std::move( *bytes ) };
      return read;
    }

    if ( has( item, "reserve" ) )
    {
      std::optional<std::int64_t> const size = numberAt( item, "reserve", where + ".reserve" );
      if ( !size.has_value() )
      {
        return std::nullopt;
      }
      read.content = GeneratedReserve{ .size = *size };
      return read;
    }

    if ( has( item, "constant" ) )
    {
      std::optional<std::string> const name = stringAt( item, "constant", where + ".constant" );
      std::optional<std::int64_t> const value = numberAt( item, "value", where + ".value" );
      if ( !name.has_value() || !value.has_value() )
      {
        return fail( mFault.empty() ? where + ": a constant is a name and a value" : mFault );
      }
      read.content = GeneratedConstant{ .name = *name, .value = *value };
      return read;
    }

    if ( read.label.has_value() )
    {
      // A label and nothing else is a position, which is how the end of a
      // Section is named.
      return read;
    }

    return fail( where + ": not a kind of item" );
  }

  [[nodiscard]] bool has( JSValue object, char const* key ) const
  {
    Value const value{ mCtx, JS_GetPropertyStr( mCtx, object, key ) };
    return !JS_IsUndefined( value.get() ) && !JS_IsNull( value.get() );
  }

  /// Words as the target reads them: two bytes each, low byte first. The order
  /// is the machine's, so it is the tool's — a script that laid the bytes out
  /// itself would be laying out the order of the host it ran on.
  [[nodiscard]] std::optional<std::vector<std::uint8_t>> wordsOf( JSValue value, std::string const& where )
  {
    std::optional<std::vector<std::int64_t>> const values = elementsOf( value, where, 0xFFFF, "a word" );
    if ( !values.has_value() )
    {
      return std::nullopt;
    }

    std::vector<std::uint8_t> bytes;
    bytes.reserve( values->size() * 2 );
    for ( std::int64_t const word : *values )
    {
      bytes.push_back( static_cast<std::uint8_t>( word & 0xFF ) );
      bytes.push_back( static_cast<std::uint8_t>( word >> 8 & 0xFF ) );
    }
    return bytes;
  }

  [[nodiscard]] std::optional<std::vector<std::uint8_t>> bytesOf( JSValue value, std::string const& where )
  {
    std::size_t size = 0;
    if ( std::uint8_t const* bytes = JS_GetUint8Array( mCtx, &size, value ); bytes != nullptr )
    {
      return std::vector<std::uint8_t>{ bytes, bytes + size };
    }

    // JS_GetUint8Array leaves a TypeError behind when the value is not one.
    JS_FreeValue( mCtx, JS_GetException( mCtx ) );

    std::optional<std::vector<std::int64_t>> const values = elementsOf( value, where, 0xFF, "a byte" );
    if ( !values.has_value() )
    {
      return std::nullopt;
    }
    return std::vector<std::uint8_t>{ values->begin(), values->end() };
  }

  /// The elements of anything with a length — a typed array or an ordinary one —
  /// each within `most`, so that one reader answers for bytes and for words.
  [[nodiscard]] std::optional<std::vector<std::int64_t>>
  elementsOf( JSValue value, std::string const& where, std::int64_t most, char const* what )
  {
    if ( !JS_IsObject( value ) )
    {
      return std::nullopt;
    }

    Value const length{ mCtx, JS_GetPropertyStr( mCtx, value, "length" ) };
    if ( !JS_IsNumber( length.get() ) )
    {
      return std::nullopt;
    }

    std::int64_t count = 0;
    JS_ToInt64( mCtx, &count, length.get() );

    std::vector<std::int64_t> read;
    read.reserve( static_cast<std::size_t>( count ) );
    for ( std::int64_t at = 0; at < count; ++at )
    {
      Value const element{ mCtx, JS_GetPropertyUint32( mCtx, value, static_cast<std::uint32_t>( at ) ) };
      std::int64_t number = 0;
      if ( !JS_IsNumber( element.get() ) || JS_ToInt64( mCtx, &number, element.get() ) != 0 || number < 0 ||
           number > most )
      {
        JS_FreeValue( mCtx, JS_GetException( mCtx ) );
        fail( where + "[" + std::to_string( at ) + "]: not " + what );
        return std::nullopt;
      }
      read.push_back( number );
    }
    return read;
  }

  [[nodiscard]] std::optional<std::string> stringAt( JSValue object, char const* key, std::string const& where )
  {
    Value const value{ mCtx, JS_GetPropertyStr( mCtx, object, key ) };
    if ( JS_IsUndefined( value.get() ) || JS_IsNull( value.get() ) )
    {
      return std::nullopt;
    }
    if ( !JS_IsString( value.get() ) )
    {
      fail( where + ": not a string" );
      return std::nullopt;
    }
    return stringOf( mCtx, value.get() );
  }

  [[nodiscard]] std::optional<std::int64_t> numberAt( JSValue object, char const* key, std::string const& where )
  {
    Value const value{ mCtx, JS_GetPropertyStr( mCtx, object, key ) };
    if ( JS_IsUndefined( value.get() ) || JS_IsNull( value.get() ) )
    {
      return std::nullopt;
    }
    std::int64_t number = 0;
    if ( !JS_IsNumber( value.get() ) || JS_ToInt64( mCtx, &number, value.get() ) != 0 )
    {
      JS_FreeValue( mCtx, JS_GetException( mCtx ) );
      fail( where + ": not a number" );
      return std::nullopt;
    }
    return number;
  }

  [[nodiscard]] bool flagAt( JSValue object, char const* key, std::string const& where )
  {
    Value const value{ mCtx, JS_GetPropertyStr( mCtx, object, key ) };
    if ( JS_IsUndefined( value.get() ) || JS_IsNull( value.get() ) )
    {
      return false;
    }
    if ( !JS_IsBool( value.get() ) )
    {
      fail( where + ": not true or false" );
      return false;
    }
    return JS_ToBool( mCtx, value.get() ) != 0;
  }

  JSContext* mCtx;
  std::string mFault;
};

/// The arguments the entry wrote, as the object a script is called with: a
/// quoted literal is a string, a value is a number, a word is a string, and a
/// flag is `true`.
Value argumentsOf( JSContext* ctx, GeneratorCall const& call )
{
  Value args{ ctx, JS_NewObject( ctx ) };
  for ( GeneratorArgument const& argument : call.arguments )
  {
    if ( argument.name.empty() )
    {
      continue;
    }

    JSValue value = JS_TRUE;
    if ( argument.text.has_value() )
    {
      value = JS_NewString( ctx, argument.text->c_str() );
    }
    else if ( argument.number.has_value() )
    {
      value = JS_NewInt64( ctx, *argument.number );
    }
    else if ( argument.word.has_value() )
    {
      value = JS_NewString( ctx, argument.word->c_str() );
    }
    JS_SetPropertyStr( ctx, args.get(), argument.name.c_str(), value );
  }
  return args;
}

} // namespace

std::optional<GeneratedModule>
runScript( GeneratorCall const& call, GeneratorFiles const& files, diag::DiagnosticSink& sink )
{
  std::string path;
  for ( GeneratorArgument const& argument : call.arguments )
  {
    if ( argument.name.empty() && argument.text.has_value() )
    {
      path = *argument.text;
      break;
    }
  }

  if ( path.empty() )
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::GENERATOR_NEEDS_FILE )
                  .at( call.generatorSpan.begin, call.generatorSpan.length )
                  .arg( "generator", call.generator ) );
    return std::nullopt;
  }

  Host host{ .files = &files };
  std::string resolved;
  std::optional<std::string> const source = files.read( path, resolved );
  if ( !source.has_value() )
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::CANNOT_READ_FILE )
                  .at( call.generatorSpan.begin, call.generatorSpan.length )
                  .arg( "path", resolved ) );
    return std::nullopt;
  }

  auto const failed = [&]( std::string message )
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::SCRIPT_FAILED )
                  .at( call.generatorSpan.begin, call.generatorSpan.length )
                  .arg( "path", path )
                  .arg( "message", std::move( message ) ) );
    return std::nullopt;
  };

  auto const stopped = [&]
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::SCRIPT_BUDGET )
                  .at( call.generatorSpan.begin, call.generatorSpan.length )
                  .arg( "path", path ) );
    return std::nullopt;
  };

  Engine engine{ host };
  JSContext* ctx = engine.context();

  Value compiled{
    ctx, JS_Eval( ctx, source->data(), source->size(), path.c_str(), JS_EVAL_TYPE_MODULE | JS_EVAL_FLAG_COMPILE_ONLY )
  };
  if ( compiled.isException() )
  {
    return failed( messageOf( ctx ) );
  }

  // JS_EvalFunction consumes what it is given, so the handle hands it over.
  auto* module = static_cast<JSModuleDef*>( JS_VALUE_GET_PTR( compiled.get() ) );
  Value const ran{ ctx, JS_EvalFunction( ctx, compiled.release() ) };

  if ( ran.isException() )
  {
    return host.work > WORK_ALLOWED ? stopped() : failed( messageOf( ctx ) );
  }

  Value const space{ ctx, JS_GetModuleNamespace( ctx, module ) };
  Value const entry{ ctx, JS_GetPropertyStr( ctx, space.get(), "default" ) };
  if ( !JS_IsFunction( ctx, entry.get() ) )
  {
    return failed( "the script has no default export, and a generator is one function" );
  }

  Value const args = argumentsOf( ctx, call );
  Value nga{ ctx, JS_NewObject( ctx ) };
  JS_SetPropertyStr( ctx, nga.get(), "read", JS_NewCFunction( ctx, readFile, "read", 1 ) );
  JS_SetPropertyStr( ctx, nga.get(), "error", JS_NewCFunction( ctx, reportError, "error", 1 ) );
  JS_SetPropertyStr( ctx, nga.get(), "warn", JS_NewCFunction( ctx, reportWarning, "warn", 1 ) );

  std::array<JSValue, 2> given{ args.get(), nga.get() };
  Value const returned{ ctx, JS_Call( ctx, entry.get(), JS_UNDEFINED, 2, given.data() ) };
  if ( returned.isException() )
  {
    return host.work > WORK_ALLOWED ? stopped() : failed( messageOf( ctx ) );
  }

  // What the script said, in the order it said it, at the call in the `.ngp`:
  // that is where an author can act, and the script's own line is the note.
  for ( Report const& report : host.reports )
  {
    diag::Diagnostic finding = diag::diagnostic( report.isError ? diag::DiagnosticId::SCRIPT_REPORTED_ERROR
                                                                : diag::DiagnosticId::SCRIPT_REPORTED_WARNING )
                                   .at( call.generatorSpan.begin, call.generatorSpan.length )
                                   .arg( "module", call.module )
                                   .arg( "message", report.message );
    if ( !report.where.empty() )
    {
      finding = std::move( finding ).note(
          diag::diagnostic( diag::DiagnosticId::SCRIPT_REPORTED_AT ).arg( "where", report.where ) );
    }
    sink.add( std::move( finding ) );
  }

  spdlog::debug( "script {} used {} of {} work and read {} files", path, host.work, WORK_ALLOWED, host.read.size() );

  Reader reader{ ctx };
  std::optional<GeneratedModule> description = reader.read( returned.get(), path );
  if ( !description.has_value() )
  {
    sink.add( diag::diagnostic( diag::DiagnosticId::GENERATOR_DESCRIPTION )
                  .at( call.generatorSpan.begin, call.generatorSpan.length )
                  .arg( "generator", call.generator )
                  .arg( "where", reader.fault() ) );
    return std::nullopt;
  }
  return description;
}

} // namespace nga::model
