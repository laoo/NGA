#pragma once

#include "nga/c/Front.hpp"

namespace nga::c::detail
{

/// The whole expression the type check was given, which a compound assignment
/// used as a value is held against: set by the outermost call, whichever entry
/// it came in by, and cleared when that call returns — see
/// docs/decisions/0198-a-compound-assignment-is-a-value.md.
class Rooted
{
public:
  Rooted( Expression const*& root, Expression const& node ) : mRoot( &root ), mSet( root == nullptr )
  {
    if ( mSet )
    {
      root = &node;
    }
  }

  Rooted( Rooted const& ) = delete;
  Rooted( Rooted&& ) = delete;
  Rooted& operator=( Rooted const& ) = delete;
  Rooted& operator=( Rooted&& ) = delete;

  ~Rooted()
  {
    if ( mSet )
    {
      *mRoot = nullptr;
    }
  }

private:
  Expression const** mRoot;
  bool mSet;
};

/// The types of a file's expressions and the rules of
/// docs/decisions/0076-arithmetic-in-the-subset.md, reporting into a sink
/// where it is given one: the checking pass reports, and the lowering pass,
/// which runs only on a file the check passed, asks the same questions again
/// without one.
class Typing
{
public:
  Typing( diag::SourceManager const& sources,
          Names const& names,
          Unit const& unit,
          diag::DiagnosticSink* sink,
          Membership* members = nullptr )
      : mSources( &sources ), mNames( &names ), mUnit( &unit ), mSink( sink ), mMembers( members )
  {
  }

  /// Says whose body is being read, so that what is recorded names it.
  void within( Definition const* function )
  {
    mWithin = function;
  }

  [[nodiscard]] AggregateOf aggregateOf() const
  {
    return [names = mNames]( std::uint32_t index ) -> AggregateType const& { return names->aggregate( index ); };
  }

  /// The shape of what an expression is a place of, as the lowering reads it.
  [[nodiscard]] std::optional<Shape> shape( Expression const& node ) const
  {
    return shapeOf( node, [this]( Expression const& read ) { return lookup( read ); }, aggregateOf(), *mSources );
  }

  void add( diag::Diagnostic value )
  {
    if ( mSink != nullptr )
    {
      mSink->add( std::move( value ) );
    }
  }

  /// What a name means, reporting it where it names nothing, or names what
  /// of the assembler's C does not read; a call reports that itself.
  std::optional<Meaning> resolve( Expression const& identifier, bool called = false )
  {
    std::string const spelling = nameSpelled( mSources->textOf( identifier.token.span() ) );
    if ( std::optional<Meaning> const local = declared( spelling ); local.has_value() )
    {
      return local;
    }
    std::optional<Meaning> meaning = mNames->resolve( *mUnit, spelling );
    if ( !meaning.has_value() )
    {
      name( diag::DiagnosticId::C_NOT_DECLARED, identifier );
    }
    if ( meaning.has_value() && meaning->kind == NameKind::ASSEMBLER_OTHER && !called )
    {
      unread( identifier, *meaning->external );
      return std::nullopt;
    }
    return meaning;
  }

  /// A local of a block being read, the innermost of that name.
  [[nodiscard]] std::optional<Meaning> declared( std::string_view spelling ) const
  {
    for ( auto const& scope : std::views::reverse( mScopes ) )
    {
      if ( auto const found = scope.find( spelling ); found != scope.end() )
      {
        return found->second;
      }
    }
    return std::nullopt;
  }

  void openScope()
  {
    mScopes.emplace_back();
  }

  void closeScope()
  {
    mScopes.pop_back();
  }

  /// Declares a local in the innermost scope. False where that scope declares
  /// the name already, which is the caller's to report; a name of an outer
  /// scope, or of the program, is hidden as C hides it.
  bool declareLocal( Token const& name, Meaning const& meaning )
  {
    if ( mScopes.empty() )
    {
      openScope();
    }
    return mScopes.back().try_emplace( spellingOf( name ), meaning ).second;
  }

  /// The type a declaration names: a keyword, or an `enum struct`.
  /// What each label of a `switch` stands for, in the order written: an
  /// enumerator's place in its type, or the constant the label is. A label
  /// that is neither counts as zero, and is refused where it is checked.
  [[nodiscard]] std::vector<std::int64_t> labelValues( Statement const& node, ir::Type type )
  {
    std::vector<std::int64_t> values;
    for ( Statement const& clause : node.items )
    {
      if ( clause.expression == nullptr )
      {
        continue;
      }
      if ( std::optional<std::uint32_t> const enumerator = mNames->enumeratorIndex( *mUnit, *clause.expression ) )
      {
        values.push_back( *enumerator );
        continue;
      }
      values.push_back( isKnown( expression( *clause.expression ) ) ? wrapped( fold( *clause.expression, type ), type )
                                                                    : 0 );
    }
    return values;
  }

  /// The type a local written `auto` takes: what the value it is given is,
  /// which every pass reads the same way. Nothing where the value has no type
  /// of its own — an integer constant, `nullptr` — which is reported at the
  /// declarator, or where the value was refused, which has been reported. See
  /// docs/decisions/0161-auto-takes-the-type-of-the-value.md.
  [[nodiscard]] std::optional<Meaning> deducedType( Token const& declarator, Expression const& value )
  {
    Typed const kind = expression( value );
    if ( !kind.valid )
    {
      return std::nullopt;
    }
    if ( !kind.type.has_value() || kind.isNull || kind.taken != nullptr || kind.takenProc != nullptr )
    {
      add( diagnostic( diag::DiagnosticId::C_AUTO_WITHOUT_TYPE )
               .at( declarator.location, declarator.length )
               .arg( "name", spellingOf( declarator ) )
               .arg( "what", describe( kind ) ) );
      return std::nullopt;
    }
    Meaning object;
    object.kind = NameKind::OBJECT;
    giveType( object, typeOfTyped( kind ) );
    return object;
  }

  std::optional<Meaning> declaredType( Token const& token )
  {
    if ( token.kind != TokenKind::IDENTIFIER )
    {
      Meaning object;
      object.kind = NameKind::OBJECT;
      object.type = typeNamed( token.keyword );
      return object;
    }
    std::optional<Meaning> const named = mNames->resolve( *mUnit, nameSpelled( mSources->textOf( token.span() ) ) );
    if ( !named.has_value() || named->kind != NameKind::TYPE )
    {
      add( diagnostic( diag::DiagnosticId::C_NOT_A_TYPE )
               .at( token.location, token.length )
               .arg( "name", spellingOf( token ) ) );
      return std::nullopt;
    }

    Meaning object;
    object.kind = NameKind::OBJECT;
    giveType( object, typeOfValue( *named ) );
    return object;
  }

  /// What a name of the program means in this file, without a word where it
  /// names nothing.
  [[nodiscard]] std::optional<Meaning> lookupName( std::string_view spelling ) const
  {
    return mNames->resolve( *mUnit, spelling );
  }

  /// A name as it is read here, by its spelling: a local first, then the
  /// program's.
  [[nodiscard]] std::optional<Meaning> lookupSpelling( std::string_view spelling ) const
  {
    if ( std::optional<Meaning> const local = declared( spelling ); local.has_value() )
    {
      return local;
    }
    return mNames->resolve( *mUnit, spelling );
  }

  /// A name as it is read here, without a word where it names nothing.
  [[nodiscard]] std::optional<Meaning> lookup( Expression const& identifier ) const
  {
    std::string const spelling = nameSpelled( mSources->textOf( identifier.token.span() ) );
    if ( std::optional<Meaning> const local = declared( spelling ); local.has_value() )
    {
      return local;
    }
    return mNames->resolve( *mUnit, spelling );
  }

  /// The value of an expression of constants, as `folded` has it, its names
  /// read here.
  [[nodiscard]] std::int64_t fold( Expression const& node, std::optional<ir::Type> type ) const
  {
    return folded(
        node,
        type,
        constantValues( *mNames, *mUnit, [this]( Expression const& read ) { return lookup( read ); }, *mSources ) );
  }

  /// Gives the innermost local of that name what it was found to be once its
  /// declaration was read.
  void settleLocal( Token const& name, Meaning const& meaning )
  {
    if ( auto const found = mScopes.back().find( spellingOf( name ) ); found != mScopes.back().end() )
    {
      found->second = meaning;
    }
  }

  /// A kind of value as a finding names it.
  [[nodiscard]] std::string describe( Typed const& kind ) const
  {
    if ( kind.isNull )
    {
      return "`nullptr`";
    }
    if ( kind.aggregate.has_value() )
    {
      return articled( mNames->aggregate( *kind.aggregate ).name );
    }
    if ( std::optional<Pointee> const pointee = kind.pointee; pointee.has_value() )
    {
      std::string target = pointee->enumeration.has_value() ? mNames->enumeration( *pointee->enumeration ).name
                                                            : std::string{ ir::spellingOf( pointee->type ) };
      if ( pointee->aggregate.has_value() )
      {
        target = mNames->aggregate( *pointee->aggregate ).name;
      }
      return articled( std::string{ pointee->isConst ? "const " : "" } +
                       std::string{ pointee->isVolatile ? "volatile " : "" } + target + "*" );
    }
    if ( kind.enumeration.has_value() )
    {
      return articled( mNames->enumeration( *kind.enumeration ).name );
    }
    if ( kind.type.has_value() )
    {
      return articled( ir::spellingOf( *kind.type ) );
    }
    return "an integer constant";
  }

  /// Where the calls the expressions typed from now on make are written
  /// down, or nowhere.
  void recordCalls( std::vector<CallSite>* into )
  {
    mCallSites = into;
  }

  /// Where the locals whose address is taken are written down, by where they
  /// are declared, or nowhere.
  void recordAddresses( std::set<std::uint32_t>* into )
  {
    mAddressed = into;
  }

  /// A call through a pointer of a function type, held to the type's
  /// signature: every member reads the arguments from the same Temporaries,
  /// so the type is the whole of what a call may know — see
  /// docs/decisions/0065-handlers.md.
  Typed throughPointer( Expression const& node, FunctionType const& type, std::uint32_t which, bool read )
  {
    Expression const& target = *node.left;
    if ( mMembers != nullptr && mWithin != nullptr )
    {
      mMembers->callsOfType.push_back(
          Membership::CallOfType{ .caller = mWithin, .type = which, .span = node.span, .sink = mSink } );
    }
    passed( node, &type.parameters );
    if ( !read )
    {
      return typed( ir::Type::U8 );
    }
    if ( !type.result.has_value() )
    {
      add( diagnostic( diag::DiagnosticId::C_NOTHING_RETURNED_READ )
               .at( node.span.begin, node.span.length )
               .arg( "name", spellingOf( target.token ) ) );
      return refused();
    }
    return typedAs( *type.result );
  }

  /// Whether a call gives as many arguments as `count`, refused where not.
  bool countedArguments( Expression const& node, std::size_t count )
  {
    if ( count == node.arguments.size() )
    {
      return true;
    }
    add( diagnostic( diag::DiagnosticId::C_ARGUMENT_COUNT )
             .at( node.span.begin, node.span.length )
             .arg( "name", spellingOf( node.left->token ) )
             .arg( "given", static_cast<std::int64_t>( node.arguments.size() ) )
             .arg( "count", static_cast<std::int64_t>( count ) ) );
    return false;
  }

  /// The arguments of a call of C's kind, each typed, and each held to its
  /// parameter where the call gives one for each of `parameters`; nothing
  /// is held where what is called is not known. Whether one was given for
  /// each. A `struct` read out of stripes is no value to copy whole — see
  /// docs/decisions/0089-a-stripe-is-a-section.md.
  bool passed( Expression const& node, std::vector<ParameterType> const* parameters )
  {
    bool const paired = parameters != nullptr && countedArguments( node, parameters->size() );
    for ( std::size_t index = 0; index < node.arguments.size(); ++index )
    {
      Expression const& argument = *node.arguments[index];
      Typed const kind = expression( argument );
      if ( kind.valid && kind.aggregate.has_value() && throughStripes( argument ) )
      {
        refuse( argument.span, "a struct in a striped array given whole as an argument" );
      }
      if ( paired && kind.valid )
      {
        NamedType const& parameter = ( *parameters )[index].type;
        assignable( argument, kind, parameter.type, parameter.enumeration, parameter.pointee, parameter.aggregate );
      }
    }
    return paired;
  }

  /// A call: of a name, and one that can be called, with an argument for each
  /// parameter, each assignable to it. Read as a value where `read` says, and
  /// then of the callee's result, which it must have — see
  /// docs/decisions/0082-a-call-writes-the-callees-bytes.md.
  Typed call( Expression const& node, bool read )
  {
    Expression const& target = *node.left;
    std::optional<Meaning> meaning;
    if ( target.kind != ExpressionKind::IDENTIFIER )
    {
      refuse( node.span, "a call through an expression" );
    }
    else
    {
      meaning = resolve( target, true );
    }

    if ( meaning.has_value() && meaning->kind == NameKind::ASSEMBLER_PROC )
    {
      return assemblerCall( node, *meaning, read );
    }
    // A call through a pointer of a function type: the arguments go to the
    // type's Temporaries and the result comes from its own, so what a call is
    // held to is the type's signature — see docs/decisions/0065-handlers.md.
    if ( meaning.has_value() && meaning->kind == NameKind::OBJECT && meaning->pointee.has_value() &&
         meaning->pointee->function.has_value() )
    {
      return throughPointer(
          node, mNames->functionType( *meaning->pointee->function ), *meaning->pointee->function, read );
    }
    Definition const* const callee =
        meaning.has_value() && meaning->kind == NameKind::FUNCTION ? meaning->definition : nullptr;
    if ( meaning.has_value() && ( meaning->kind == NameKind::OBJECT || meaning->kind == NameKind::TYPE ||
                                  meaning->kind == NameKind::ASSEMBLER_OTHER ) )
    {
      name( diag::DiagnosticId::C_NOT_A_FUNCTION, target );
    }
    bool const paired = passed( node, callee != nullptr ? &callee->parameters : nullptr );

    if ( callee == nullptr )
    {
      return refused();
    }
    if ( mCallSites != nullptr )
    {
      mCallSites->push_back( CallSite{ .callee = callee, .span = node.span, .external = nullptr } );
    }
    if ( !read )
    {
      return typed( ir::Type::U8 );
    }
    if ( !callee->result.has_value() )
    {
      add( diagnostic( diag::DiagnosticId::C_NOTHING_RETURNED_READ )
               .at( node.span.begin, node.span.length )
               .arg( "name", spellingOf( target.token ) ) );
      return refused();
    }
    return paired ? typedAs( *callee->result ) : refused();
  }

  /// Whether a call or a step stands anywhere in the expression: a compound
  /// assignment reaches its target twice in the tree and once in the text, so
  /// either there would be taken twice where the source wrote it once — see
  /// docs/decisions/0158-a-compound-assignment-and-a-step.md and
  /// docs/decisions/0198-a-compound-assignment-is-a-value.md.
  [[nodiscard]] static bool holdsCallOrStep( Expression const& node )
  {
    if ( node.kind == ExpressionKind::CALL || ( node.kind == ExpressionKind::ASSIGNMENT && node.compound ) )
    {
      return true;
    }
    if ( ( node.left != nullptr && holdsCallOrStep( *node.left ) ) ||
         ( node.right != nullptr && holdsCallOrStep( *node.right ) ) )
    {
      return true;
    }
    return std::ranges::any_of( node.arguments,
                                []( ExpressionPtr const& argument )
                                { return argument != nullptr && holdsCallOrStep( *argument ); } );
  }

  /// An assignment held to its rules — the target assignable, the value of the
  /// target's type — wherever it is written, as a statement or as a value:
  /// what it assigns to, or nothing where either side was refused.
  std::optional<Meaning> assignment( Expression const& node )
  {
    Rooted const rooted( mRoot, node );
    Expression const& target = *node.left;
    if ( node.compound && holdsCallOrStep( target ) )
    {
      refuse( target.span, "a call or a step in what `+=`, `++` or their kin assign to" );
      return std::nullopt;
    }
    std::optional<Meaning> object;
    if ( target.kind == ExpressionKind::INDEX || target.kind == ExpressionKind::DEREFERENCE ||
         target.kind == ExpressionKind::MEMBER )
    {
      // An element, or what a pointer points at, is assigned as an object of
      // its type, unless it is `const`; a member that is an array is not.
      Typed const reached = expression( target );
      if ( std::optional<Shape> const place = shape( target );
           reached.valid && place.has_value() && place->isArray && target.kind == ExpressionKind::MEMBER )
      {
        add( diagnostic( diag::DiagnosticId::C_ARRAY_ASSIGNED )
                 .at( target.span.begin, target.span.length )
                 .arg( "name", textOf( target.span ) ) );
      }
      else if ( reached.valid && reached.readOnly )
      {
        if ( reached.throughPointer )
        {
          add( diagnostic( diag::DiagnosticId::C_WRITE_THROUGH_CONST ).at( target.span.begin, target.span.length ) );
        }
        else
        {
          Expression const* root = target.left.get();
          while ( root->left != nullptr )
          {
            root = root->left.get();
          }
          name( diag::DiagnosticId::C_CONST_ASSIGNED, *root );
        }
      }
      else if ( reached.valid )
      {
        object = Meaning{};
        object->kind = NameKind::OBJECT;
        giveType( *object, typeOfTyped( reached ) );
      }
    }
    else if ( target.kind != ExpressionKind::IDENTIFIER )
    {
      refuse( target.span, "an assignment to an expression" );
    }
    else if ( std::optional<Meaning> const meaning = resolve( target ); meaning.has_value() )
    {
      if ( meaning->kind == NameKind::OBJECT && meaning->isArray )
      {
        name( diag::DiagnosticId::C_ARRAY_ASSIGNED, target );
      }
      else if ( meaning->kind == NameKind::OBJECT && meaning->isConst )
      {
        name( diag::DiagnosticId::C_CONST_ASSIGNED, target );
      }
      else if ( meaning->kind == NameKind::FUNCTION || meaning->kind == NameKind::ASSEMBLER_PROC )
      {
        name( diag::DiagnosticId::C_NOT_ASSIGNABLE, target );
      }
      else if ( meaning->kind == NameKind::TYPE )
      {
        name( diag::DiagnosticId::C_NOT_DECLARED, target );
      }
      else
      {
        object = meaning;
      }
    }

    if ( !object.has_value() && node.compound && node.right->kind == ExpressionKind::BINARY )
    {
      // The value of `+=` or `++` reads the target again, and would report
      // a second time what was just reported of it: only the operand the
      // source wrote is left to check.
      expression( *node.right->right );
      return std::nullopt;
    }
    Typed const value = expression( *node.right );
    if ( !object.has_value() || !value.valid )
    {
      return std::nullopt;
    }
    assignable( *node.right, value, object->type, object->enumeration, object->pointee, object->aggregate );
    return object;
  }

  /// The name at the root of what an assignment assigns to: the object, the
  /// array an element is of, or the pointer a pointee is reached through.
  [[nodiscard]] static Expression const& rootOf( Expression const& target )
  {
    Expression const* root = &target;
    while ( root->left != nullptr && root->kind != ExpressionKind::IDENTIFIER )
    {
      root = root->left.get();
    }
    return *root;
  }

  /// How often `spelling` is reached in an expression as the source wrote it,
  /// `stepped` — the step being held to the rule — and the copy of its target
  /// every compound assignment's tree holds not counting, the source having
  /// written neither — see
  /// docs/decisions/0198-a-compound-assignment-is-a-value.md.
  [[nodiscard]] std::uint32_t
  reaches( Expression const& node, std::string_view spelling, Expression const& stepped ) const
  {
    if ( &node == &stepped )
    {
      return 0;
    }
    if ( node.kind == ExpressionKind::IDENTIFIER )
    {
      return nameSpelled( mSources->textOf( node.token.span() ) ) == spelling ? 1 : 0;
    }
    std::uint32_t count = 0;
    // A member's name is the member's, not an object's, and an enumerator's
    // is its `enum struct`'s.
    bool const named = node.kind == ExpressionKind::MEMBER || node.kind == ExpressionKind::QUALIFIED_NAME;
    if ( node.left != nullptr )
    {
      count += reaches( *node.left, spelling, stepped );
    }
    if ( node.right != nullptr && !named )
    {
      // `a += b` is `a = a + b`: the target under the operator is the copy the
      // parser wrote, and only what stands beside it is the source's.
      Expression const& value = *node.right;
      bool const desugared = node.kind == ExpressionKind::ASSIGNMENT && node.compound &&
                             value.kind == ExpressionKind::BINARY && value.right != nullptr;
      count += desugared ? reaches( *value.right, spelling, stepped ) : reaches( value, spelling, stepped );
    }
    for ( ExpressionPtr const& argument : node.arguments )
    {
      if ( argument != nullptr )
      {
        count += reaches( *argument, spelling, stepped );
      }
    }
    return count;
  }

  /// An assignment read for its value: `a = b` is none of them, and `a += b`,
  /// `++a` and `a++` are — what the target holds after the step, or before it
  /// where the operator was written after the target — see
  /// docs/decisions/0198-a-compound-assignment-is-a-value.md.
  Typed assigned( Expression const& node )
  {
    Rooted const rooted( mRoot, node );
    if ( !node.compound )
    {
      add( diagnostic( diag::DiagnosticId::C_ASSIGNMENT_NOT_VALUE ).at( node.span.begin, node.span.length ) );
      return refused();
    }
    std::optional<Meaning> const object = assignment( node );
    if ( !object.has_value() )
    {
      return refused();
    }
    if ( Expression const& root = rootOf( *node.left );
         mRoot != nullptr && root.kind == ExpressionKind::IDENTIFIER &&
         reaches( *mRoot, nameSpelled( mSources->textOf( root.token.span() ) ), node ) > 0 )
    {
      add( diagnostic( diag::DiagnosticId::C_STEP_REACHED_AGAIN )
               .at( node.span.begin, node.span.length )
               .arg( "name", spellingOf( root.token ) )
               .arg( "operator", spellingOf( node.token ) ) );
      return refused();
    }
    Typed result = typed( object->type );
    result.enumeration = object->enumeration;
    result.pointee = object->pointee;
    result.aggregate = object->aggregate;
    return result;
  }

  /// An expression read for its value.
  Typed expression( Expression const& node )
  {
    Rooted const rooted( mRoot, node );
    switch ( node.kind )
    {
    case ExpressionKind::INTEGER_CONSTANT:
      return constant();
    case ExpressionKind::STRING_LITERAL:
    {
      // A `const u8*` to a Section of its bytes, whose address the assembler
      // knows.
      charsetOf( node.token );
      Typed result = pointing( pointeeOf( ValueType{ .type = ir::Type::U8 }, true ) );
      result.address = true;
      return result;
    }
    case ExpressionKind::CHARACTER_CONSTANT:
    {
      charsetOf( node.token );
      if ( !isTranslated( node ) )
      {
        return constant();
      }
      // A byte only the assembler knows, once the Charset is resolved.
      Typed result = typed( ir::Type::U8 );
      result.address = true;
      return result;
    }
    case ExpressionKind::PREDEFINED_CONSTANT:
    {
      Typed result = typed( ir::Type::BOOL );
      result.known = true;
      return result;
    }
    case ExpressionKind::IDENTIFIER:
      return identifier( node );
    case ExpressionKind::QUALIFIED_NAME:
      return qualified( node );
    case ExpressionKind::CALL:
      return call( node, true );
    case ExpressionKind::ASSIGNMENT:
      return assigned( node );
    case ExpressionKind::UNARY:
      return unary( node );
    case ExpressionKind::CAST:
      return cast( node );
    case ExpressionKind::INDEX:
      return element( node );
    case ExpressionKind::SIZEOF:
      return sizeOf( node );
    case ExpressionKind::DEREFERENCE:
      return dereference( node );
    case ExpressionKind::ADDRESS:
      return addressOf( node );
    case ExpressionKind::NULL_POINTER:
    {
      Typed result = typed( ir::Type::POINTER );
      result.isNull = true;
      return result;
    }
    case ExpressionKind::MEMBER:
      return member( node );
    case ExpressionKind::LIST:
      add( diagnostic( diag::DiagnosticId::C_LIST_SHAPE )
               .at( node.span.begin, node.span.length )
               .arg( "what", std::string{ "a value" } )
               .arg( "given", std::string{ "a list" } ) );
      return refused();
    case ExpressionKind::BINARY:
      return binary( node );
    case ExpressionKind::CONDITIONAL:
      return conditional( node );
    }
    return refused();
  }

  /// `c ? a : b`: a `bool` decides, and the ways are of one type, which the
  /// answer is. A constant takes the other way's type, and two constants
  /// take the type of where the answer is used, as one constant does — see
  /// docs/decisions/0169-a-conditional-operator.md.
  Typed conditional( Expression const& node )
  {
    bool const decided = condition( *node.left );
    Typed const whenTrue = expression( *node.arguments.front() );
    Typed const whenFalse = expression( *node.arguments.back() );
    if ( !decided || !whenTrue.valid || !whenFalse.valid )
    {
      return refused();
    }
    if ( whenTrue.aggregate.has_value() || whenFalse.aggregate.has_value() )
    {
      refuse( node.span, "a `?:` of a `struct` or a `union`" );
      return refused();
    }
    if ( !whenTrue.type.has_value() && !whenFalse.type.has_value() )
    {
      // Two constants: the answer takes its type from where it is used, as
      // one constant does, but is no constant itself — a condition decides
      // it where the program runs.
      Typed result = constant();
      result.known = false;
      return result;
    }
    if ( !whenTrue.type.has_value() || !whenFalse.type.has_value() )
    {
      // One constant beside a value: the constant is held to the value's
      // type, as it would be assigned to an object of it.
      bool const constantFirst = !whenTrue.type.has_value();
      Typed const& typed = constantFirst ? whenFalse : whenTrue;
      Expression const& given = constantFirst ? *node.arguments.front() : *node.arguments.back();
      assignable( given,
                  constantFirst ? whenTrue : whenFalse,
                  typed.type.value_or( ir::Type::U8 ),
                  typed.enumeration,
                  typed.pointee );
      Typed result = typed;
      result.known = false;
      result.address = false;
      return result;
    }
    if ( whenTrue.type != whenFalse.type || whenTrue.enumeration != whenFalse.enumeration ||
         ( whenTrue.pointee.has_value() != whenFalse.pointee.has_value() ) ||
         ( whenTrue.pointee.has_value() && !samePointee( *whenTrue.pointee, *whenFalse.pointee ) ) )
    {
      add( diagnostic( diag::DiagnosticId::C_TYPES_DIFFER )
               .at( node.span.begin, node.span.length )
               .arg( "left", describe( whenTrue ) )
               .arg( "right", describe( whenFalse ) ) );
      return refused();
    }
    Typed result = whenTrue;
    result.known = false;
    result.address = false;
    // What either way points at is `const` or `volatile` where either is.
    if ( result.pointee.has_value() && whenFalse.pointee.has_value() )
    {
      result.pointee->isConst = whenTrue.pointee->isConst || whenFalse.pointee->isConst;
      result.pointee->isVolatile = whenTrue.pointee->isVolatile || whenFalse.pointee->isVolatile;
    }
    result.readOnly = whenTrue.readOnly || whenFalse.readOnly;
    return result;
  }

  /// A call of an assembler's `.proc`, held to its Signature as a call of C is
  /// to its parameters: scalars only — see
  /// docs/decisions/0092-what-the-assembler-exports-is-typed-by-its-shape.md#calls--task-4b.
  Typed assemblerCall( Expression const& node, Meaning const& meaning, bool read )
  {
    Expression const& target = *node.left;
    // An `extern` nothing the assembler exports answers to is refused where
    // it is declared, and a call of it has no Proc whose Pane to check.
    if ( mCallSites != nullptr && meaning.external != nullptr )
    {
      mCallSites->push_back( CallSite{ .callee = nullptr, .span = node.span, .external = meaning.external } );
    }
    if ( meaning.definition != nullptr )
    {
      return declaredCall( node, *meaning.definition, meaning.external, read );
    }
    ExternalName const& proc = *meaning.external;
    countedArguments( node, proc.arguments.size() );
    bool const handsBytes =
        std::ranges::any_of( proc.arguments, []( ExternalByte const& byte ) { return byte.isBytes; } ) ||
        ( proc.result.has_value() && proc.result->isBytes );
    if ( handsBytes )
    {
      add( diagnostic( diag::DiagnosticId::C_BYTES_HANDED_OVER )
               .at( node.span.begin, node.span.length )
               .arg( "name", spellingOf( target.token ) ) );
    }
    bool const paired = proc.arguments.size() == node.arguments.size();
    for ( std::size_t index = 0; index < node.arguments.size(); ++index )
    {
      Expression const& argument = *node.arguments[index];
      Typed const kind = expression( argument );
      if ( paired && kind.valid && !proc.arguments[index].isBytes )
      {
        assignable( argument, kind, proc.arguments[index].type, std::nullopt, std::nullopt, std::nullopt );
      }
    }
    if ( !read )
    {
      return handsBytes ? refused() : typed( ir::Type::U8 );
    }
    if ( !proc.result.has_value() )
    {
      add( diagnostic( diag::DiagnosticId::C_NOTHING_RETURNED_READ )
               .at( node.span.begin, node.span.length )
               .arg( "name", spellingOf( target.token ) ) );
      return refused();
    }
    return paired && !handsBytes ? typed( proc.result->type ) : refused();
  }

  /// A call of a Proc of the assembler an `extern` declares, held to the
  /// declaration as a call of C is to its function's; the declaration is held
  /// to the Proc where it stands — see
  /// docs/decisions/0094-extern-declares-how-c-calls-a-proc.md.
  Typed declaredCall( Expression const& node, Definition const& declared, ExternalName const* proc, bool read )
  {
    Expression const& target = *node.left;
    bool const paired = passed( node, &declared.parameters );
    bool const proper = proc != nullptr && proc->kind == ExternalKind::PROC;
    if ( !read )
    {
      return proper ? typed( ir::Type::U8 ) : refused();
    }
    if ( !declared.result.has_value() )
    {
      add( diagnostic( diag::DiagnosticId::C_NOTHING_RETURNED_READ )
               .at( node.span.begin, node.span.length )
               .arg( "name", spellingOf( target.token ) ) );
      return refused();
    }
    return paired && proper ? typedAs( *declared.result ) : refused();
  }

  /// A condition: a `bool`, or `!`, `&&` and `||` over conditions — see
  /// docs/decisions/0079-a-condition-is-a-bool.md. True where it is one.
  bool condition( Expression const& node )
  {
    if ( node.kind == ExpressionKind::BINARY && isLogical( node.token.kind ) )
    {
      bool const left = condition( *node.left );
      bool const right = condition( *node.right );
      return left && right;
    }
    if ( node.kind == ExpressionKind::UNARY && node.token.kind == TokenKind::BANG )
    {
      return condition( *node.left );
    }
    Typed const kind = expression( node );
    if ( !kind.valid )
    {
      return false;
    }
    if ( kind.type == ir::Type::BOOL )
    {
      return true;
    }
    add( diagnostic( diag::DiagnosticId::C_CONDITION_NOT_BOOL )
             .at( node.span.begin, node.span.length )
             .arg( "what", describe( kind ) ) );
    return false;
  }

  /// Whether a value of `kind` may be assigned to an object of `target`, and
  /// of the `enum struct` `targetEnumeration` where it is of one, reporting
  /// where it may not.
  void assignable( Expression const& value,
                   Typed const& kind,
                   ir::Type target,
                   std::optional<std::uint32_t> targetEnumeration,
                   std::optional<Pointee> const& targetPointee = std::nullopt,
                   std::optional<std::uint32_t> targetAggregate = std::nullopt )
  {
    // A `struct` or a `union` takes a value of its own type, copied — see
    // docs/decisions/0086-a-struct-by-value.md.
    if ( targetAggregate.has_value() || kind.aggregate.has_value() )
    {
      if ( kind.aggregate != targetAggregate )
      {
        Typed const object = targetAggregate.has_value() ? blockOf( *targetAggregate ) : typed( target );
        differ( value, object, kind );
        return;
      }
      if ( mNames->aggregate( *targetAggregate ).size > MAX_COPY_BYTES )
      {
        refuse( value.span, "a copy of more than 256 bytes" );
      }
      return;
    }
    // The address of a function goes to a pointer of a function type, whose
    // signature it must have; the taking is what makes it a member — see
    // docs/decisions/0065-handlers.md.
    if ( kind.taken != nullptr || kind.takenProc != nullptr )
    {
      takesFunction( value, targetPointee, kind );
      return;
    }
    // A pointer takes `nullptr`, or a pointer to its own type that adds `const`
    // or `volatile` or keeps it.
    if ( targetPointee.has_value() || kind.pointee.has_value() || kind.isNull )
    {
      bool const fits = targetPointee.has_value() &&
                        ( kind.isNull || ( kind.pointee.has_value() && samePointee( *kind.pointee, *targetPointee ) &&
                                           ( !kind.pointee->isConst || targetPointee->isConst ) &&
                                           ( !kind.pointee->isVolatile || targetPointee->isVolatile ) ) );
      if ( !fits )
      {
        Typed object = targetEnumeration.has_value() ? enumerated( *targetEnumeration ) : typed( target );
        object.pointee = targetPointee;
        pointersDiffer( value, object, kind );
      }
      return;
    }
    if ( kind.enumeration.has_value() || targetEnumeration.has_value() )
    {
      if ( kind.enumeration != targetEnumeration )
      {
        Typed const object = targetEnumeration.has_value() ? enumerated( *targetEnumeration ) : typed( target );
        differ( value, object, kind );
      }
      return;
    }
    if ( !kind.type.has_value() )
    {
      if ( target == ir::Type::BOOL )
      {
        mixedBool( value );
        return;
      }
      constantFits( value, target );
      return;
    }

    ir::Type const source = *kind.type;
    if ( ( source == ir::Type::BOOL ) != ( target == ir::Type::BOOL ) )
    {
      mixedBool( value );
    }
    else if ( target == ir::Type::BOOL )
    {
      return;
    }
    else if ( ir::isSigned( source ) != ir::isSigned( target ) )
    {
      add( diagnostic( diag::DiagnosticId::C_MIXED_SIGNEDNESS )
               .at( value.span.begin, value.span.length )
               .arg( "left", std::string{ ir::spellingOf( target ) } )
               .arg( "right", std::string{ ir::spellingOf( source ) } ) );
    }
    else if ( ir::sizeOf( source ) > ir::sizeOf( target ) )
    {
      add( diagnostic( diag::DiagnosticId::C_NARROWING_ASSIGNMENT )
               .at( value.span.begin, value.span.length )
               .arg( "from", std::string{ ir::spellingOf( source ) } )
               .arg( "to", std::string{ ir::spellingOf( target ) } ) );
    }
  }

  /// Whether an expression of constants folds into a value `type` holds,
  /// reported where it is written. What is held is the answer and not the
  /// numbers on the way to it — `256 - 1` is 255 and stands where a `u8` is
  /// wanted, `200 + 100` is 300 and does not — see
  /// docs/decisions/0206-a-constant-is-folded-exactly.md. A `?:` is two
  /// constants and neither of them is the answer, so each way is held on its
  /// own; a `-` written directly before a constant is its sign, as the fold
  /// reads it.
  bool constantFits( Expression const& node, ir::Type type )
  {
    if ( node.kind == ExpressionKind::CONDITIONAL )
    {
      bool const whenTrue = constantFits( *node.arguments.front(), type );
      bool const whenFalse = constantFits( *node.arguments.back(), type );
      return whenTrue && whenFalse;
    }
    if ( !foldsHere( node ) )
    {
      return true;
    }
    std::int64_t const value = fold( node, type );
    if ( value >= lowestOf( type ) && value <= highestOf( type ) )
    {
      return true;
    }
    add( diagnostic( diag::DiagnosticId::C_CONSTANT_OUT_OF_RANGE )
             .at( node.span.begin, node.span.length )
             .arg( "value", value )
             .arg( "type", std::string{ ir::spellingOf( type ) } ) );
    return false;
  }

  /// Whether every name of an expression of constants has a value here, so
  /// that the fold is the expression's own answer: a Constant the assembler
  /// keeps as a name has none, and a character of a Charset is the
  /// assembler's to translate, so neither is judged against a type.
  [[nodiscard]] bool foldsHere( Expression const& node ) const
  {
    switch ( node.kind )
    {
    case ExpressionKind::INTEGER_CONSTANT:
    case ExpressionKind::SIZEOF:
    case ExpressionKind::QUALIFIED_NAME:
      return true;
    case ExpressionKind::IDENTIFIER:
    {
      std::optional<Meaning> const meaning = lookup( node );
      return meaning.has_value() && isFoldedConstant( *meaning );
    }
    case ExpressionKind::CAST:
    case ExpressionKind::UNARY:
      return foldsHere( *node.left );
    case ExpressionKind::BINARY:
      return foldsHere( *node.left ) && foldsHere( *node.right );
    default:
      return false;
    }
  }

  /// Whether an element of a type lies in stripes: a `u16` or an `i16`, its
  /// low bytes and its high ones, or a `struct` of members that do, a member
  /// of one byte a stripe of its own; no union, whose members overlap, and no
  /// member array, which a second index would reach.
  [[nodiscard]] bool stripable( ir::Type type, std::optional<std::uint32_t> aggregateIndex ) const
  {
    if ( !aggregateIndex.has_value() )
    {
      return type == ir::Type::U16 || type == ir::Type::I16;
    }
    AggregateType const& type2 = mNames->aggregate( *aggregateIndex );
    if ( type2.isUnion )
    {
      return false;
    }
    return std::ranges::all_of( type2.members,
                                [this]( MemberType const& member )
                                {
                                  return !member.isArray &&
                                         ( !member.type.aggregate.has_value() || member.type.pointee.has_value() ||
                                           stripable( member.type.type, member.type.aggregate ) );
                                } );
  }

  /// Whether a place lies in a striped array's stripes: an element of one, or a
  /// member of an element by `.`.
  [[nodiscard]] bool throughStripes( Expression const& node ) const
  {
    Expression const* at = &node;
    while ( ( at->kind == ExpressionKind::MEMBER && at->token.kind == TokenKind::DOT ) ||
            at->kind == ExpressionKind::INDEX )
    {
      if ( at->kind == ExpressionKind::INDEX && at->left->kind == ExpressionKind::IDENTIFIER )
      {
        std::optional<Meaning> const array = lookup( *at->left );
        return array.has_value() && array->isStriped;
      }
      at = at->left.get();
    }
    return false;
  }

  /// Whether a character constant is translated by a Charset, and so no
  /// constant where it is compiled.
  [[nodiscard]] bool isTranslated( Expression const& node ) const
  {
    std::string_view const text = node.spelling.empty() ? mSources->textOf( node.token.span() ) : node.spelling;
    return !text.empty() && text.front() != '\'' && text.front() != '"';
  }

  /// A literal's prefix, held to be a Charset the assembler exports, reported
  /// once for the literal however many characters it holds.
  void charsetOf( Token const& token )
  {
    std::string_view const text = mSources->textOf( token.span() );
    std::string_view const prefix = text.substr( 0, text.find_first_of( "\"'" ) );
    if ( prefix.empty() || !mCheckedPrefixes.insert( token.location.rawOffset() ).second )
    {
      return;
    }
    ExternalName const* const external = mNames->externalNamed( prefix );
    if ( external == nullptr )
    {
      add( diagnostic( diag::DiagnosticId::C_NOT_DECLARED )
               .at( token.location, static_cast<std::uint32_t>( prefix.size() ) )
               .arg( "name", std::string{ prefix } ) );
      return;
    }
    if ( external->kind != ExternalKind::CHARSET )
    {
      add( diagnostic( diag::DiagnosticId::C_NOT_A_CHARSET )
               .at( token.location, static_cast<std::uint32_t>( prefix.size() ) )
               .arg( "name", std::string{ prefix } ) );
    }
  }

  void refuse( diag::SourceSpan span, std::string_view construct )
  {
    add( diagnostic( diag::DiagnosticId::C_NOT_COMPILED_YET )
             .at( span.begin, span.length )
             .arg( "construct", std::string{ construct } ) );
  }

  void name( diag::DiagnosticId id, Expression const& identifier )
  {
    add( diagnostic( id )
             .at( identifier.token.location, identifier.token.length )
             .arg( "name", spellingOf( identifier.token ) ) );
  }

  /// A name of the assembler's that C reads nothing of: a Label with no
  /// shape, a `reserved` Region, or a Symbol of a kind C does not read — see
  /// docs/decisions/0092-what-the-assembler-exports-is-typed-by-its-shape.md.
  void unread( Expression const& identifier, ExternalName const& external )
  {
    unread( identifier.token, external );
  }

  void unread( Token const& token, ExternalName const& external )
  {
    diag::DiagnosticId id = diag::DiagnosticId::C_NOT_READ;
    if ( external.kind == ExternalKind::SHAPELESS )
    {
      id = diag::DiagnosticId::C_NO_SHAPE;
    }
    else if ( external.kind == ExternalKind::RESERVED )
    {
      id = diag::DiagnosticId::C_RESERVED_REGION;
    }
    diag::Diagnostic finding = diagnostic( id )
                                   .at( token.location, token.length )
                                   .arg( "name", spellingOf( token ) )
                                   .arg( "reason", external.reason );
    if ( external.definition.has_value() )
    {
      finding = std::move( finding ).note( diagnostic( diag::DiagnosticId::C_DEFINED_HERE )
                                               .at( external.definition->begin, external.definition->length )
                                               .arg( "name", external.name ) );
    }
    add( std::move( finding ) );
  }

  /// Whether a function's signature is the type's, and what differs where it
  /// is not: what a taking asks of `&f`, and what an `[[implements]]` asks of
  /// what fills a Slot that takes or returns.
  [[nodiscard]] std::optional<std::string> signatureDiffers( FunctionType const& type,
                                                             Definition const& function ) const
  {
    if ( function.parameters.size() != type.parameters.size() )
    {
      return "it takes " + std::to_string( function.parameters.size() ) + " and the type " +
             std::to_string( type.parameters.size() );
    }
    for ( std::size_t index = 0; index < type.parameters.size(); ++index )
    {
      if ( !sameType( function.parameters[index].type, type.parameters[index].type ) )
      {
        return "its parameter " + std::to_string( index + 1 ) + " is " +
               describe( typedAs( function.parameters[index].type ) ) + " and the type's " +
               describe( typedAs( type.parameters[index].type ) );
      }
    }
    if ( function.result.has_value() != type.result.has_value() ||
         ( function.result.has_value() && !sameType( *function.result, *type.result ) ) )
    {
      return "its result is not the type's";
    }
    return std::nullopt;
  }

  /// A function written `[[implements(SLOT)]]` of a Slot that takes or
  /// returns joins the Slot's own function type, so that it reads the Slot's
  /// Temporaries and declares none of its own — see
  /// docs/decisions/0174-a-slot-takes-and-returns.md.
  void joinType( Definition const* function, std::uint32_t type )
  {
    if ( mMembers != nullptr && function != nullptr )
    {
      mMembers->ofFunction.try_emplace( function, type );
    }
  }

private:
  [[nodiscard]] std::string spellingOf( Token const& token ) const
  {
    return nameSpelled( mSources->textOf( token.span() ) );
  }

  [[nodiscard]] std::string textOf( diag::SourceSpan span ) const
  {
    return std::string{ mSources->textOf( span ) };
  }

  void aggregateOperand( Expression const& node )
  {
    add( diagnostic( diag::DiagnosticId::C_AGGREGATE_OPERATOR )
             .at( node.span.begin, node.span.length )
             .arg( "what", std::string{ "this" } ) );
  }

  /// `s.m` and `p->m`: a member of a `struct` or a `union`, or of one a
  /// pointer points at, `const` where what holds it is. An array member is a
  /// pointer to its first element, as an array's name is.
  Typed member( Expression const& node )
  {
    Typed const outer = expression( *node.left );
    if ( !outer.valid )
    {
      return refused();
    }
    bool const arrow = node.token.kind == TokenKind::ARROW;
    std::optional<Pointee> const pointee = outer.pointee;
    std::optional<std::uint32_t> aggregateIndex = outer.aggregate;
    if ( arrow )
    {
      aggregateIndex = pointee.has_value() ? pointee->aggregate : std::nullopt;
    }
    if ( !aggregateIndex.has_value() )
    {
      add( diagnostic( diag::DiagnosticId::C_NOT_AN_AGGREGATE )
               .at( node.left->span.begin, node.left->span.length )
               .arg( "what", arrow ? "what " + describe( outer ) + " points at" : describe( outer ) ) );
      return refused();
    }
    AggregateType const& type = mNames->aggregate( *aggregateIndex );
    std::string const name = spellingOf( node.right->token );
    MemberType const* const found = memberNamed( type, name, *mSources );
    if ( found == nullptr )
    {
      add( diagnostic( diag::DiagnosticId::C_NO_SUCH_MEMBER )
               .at( node.right->token.location, node.right->token.length )
               .arg( "type", type.name )
               .arg( "name", name ) );
      return refused();
    }
    bool const readOnly = arrow ? pointee.value_or( Pointee{} ).isConst : outer.readOnly;
    bool const isVolatile = arrow ? pointee.value_or( Pointee{} ).isVolatile : outer.isVolatile;
    Typed result = typedAs( found->type );
    if ( found->isArray )
    {
      result = pointing( pointeeOf( found->type, readOnly, isVolatile ) );
      result.address = fixedPlace( node );
    }
    result.readOnly = readOnly;
    result.isVolatile = isVolatile;
    result.throughPointer = arrow || outer.throughPointer;
    return result;
  }

  void pointersDiffer( Expression const& node, Typed const& left, Typed const& right )
  {
    add( diagnostic( diag::DiagnosticId::C_POINTERS_DIFFER )
             .at( node.span.begin, node.span.length )
             .arg( "left", describe( left ) )
             .arg( "right", describe( right ) ) );
  }

  /// Whether a place is at an address the assembler knows: an object at file
  /// scope, and a member of one by `.` or an element of an array by a constant
  /// index, however deep — see
  /// docs/decisions/0084-widths-data-arrays-pointers-aggregates.md#aggregates--task-3e.
  bool fixedPlace( Expression const& node )
  {
    switch ( node.kind )
    {
    case ExpressionKind::IDENTIFIER:
    {
      std::optional<Meaning> const meaning = lookup( node );
      return meaning.has_value() && meaning->kind == NameKind::OBJECT && !meaning->pointee.has_value() &&
             atFixedAddress( *meaning ) && !isAssemblerConstant( *meaning );
    }
    case ExpressionKind::MEMBER:
      return node.token.kind == TokenKind::DOT && fixedPlace( *node.left );
    case ExpressionKind::INDEX:
    {
      std::optional<Shape> const outer = shape( *node.left );
      return outer.has_value() && outer->isArray && fixedPlace( *node.left ) && isKnown( expression( *node.right ) );
    }
    default:
      return false;
    }
  }

  /// The object at the root of `s.a.b` or `t[i].a`, whose address `&` takes
  /// with the member's: a local among them is written down as addressed.
  void markRoot( Expression const& node )
  {
    Expression const* root = &node;
    while ( ( root->kind == ExpressionKind::MEMBER && root->token.kind == TokenKind::DOT ) ||
            root->kind == ExpressionKind::INDEX )
    {
      root = root->left.get();
    }
    if ( root->kind != ExpressionKind::IDENTIFIER )
    {
      return;
    }
    if ( std::optional<Meaning> const meaning = lookup( *root ); meaning.has_value() && !meaning->pointee.has_value() )
    {
      addressed( *meaning );
    }
  }

  /// A local whose address is taken, written down so that the lowering gives
  /// it a Section rather than a Temporary.
  void addressed( Meaning const& meaning )
  {
    if ( mAddressed != nullptr && meaning.declaredAt.has_value() )
    {
      mAddressed->insert( meaning.declaredAt->rawOffset() );
    }
  }

  /// `*p`: what a pointer points at.
  Typed dereference( Expression const& node )
  {
    Typed const pointer = expression( *node.left );
    if ( !pointer.valid )
    {
      return refused();
    }
    if ( !pointer.pointee.has_value() )
    {
      add( diagnostic( diag::DiagnosticId::C_NOT_A_POINTER )
               .at( node.left->span.begin, node.left->span.length )
               .arg( "what", describe( pointer ) ) );
      return refused();
    }
    return reached( *pointer.pointee );
  }

  /// `&x`: of an object, of an element, or of what a pointer points at. A
  /// parameter has no address to give, being its call's `.ztemp`.
  Typed addressOf( Expression const& node )
  {
    Expression const& operand = *node.left;
    if ( operand.kind == ExpressionKind::DEREFERENCE )
    {
      Typed const pointer = expression( *operand.left );
      if ( pointer.valid && !pointer.pointee.has_value() )
      {
        add( diagnostic( diag::DiagnosticId::C_NOT_A_POINTER )
                 .at( operand.left->span.begin, operand.left->span.length )
                 .arg( "what", describe( pointer ) ) );
        return refused();
      }
      return pointer.valid ? pointing( *pointer.pointee ) : refused();
    }
    if ( operand.kind == ExpressionKind::INDEX && operand.left->kind != ExpressionKind::IDENTIFIER )
    {
      // An element of a member array, or of what a pointer an expression gives
      // points at.
      Typed const element = expression( operand );
      if ( !element.valid )
      {
        return refused();
      }
      markRoot( operand );
      Typed result = pointing( pointeeOf( typeOfTyped( element ), element.readOnly, element.isVolatile ) );
      result.address = fixedPlace( operand );
      return result;
    }
    if ( operand.kind == ExpressionKind::INDEX )
    {
      Typed const element = expression( operand );
      std::optional<Meaning> const target = lookup( *operand.left );
      if ( !element.valid || !target.has_value() )
      {
        return refused();
      }
      if ( target->isStriped )
      {
        stripedElement( node, spellingOf( operand.left->token ) );
        return refused();
      }
      if ( target->pointee.has_value() )
      {
        return pointing( *target->pointee );
      }
      addressed( *target );
      Typed result = pointing( pointeeOf( *target, target->isConst, target->isVolatile ) );
      result.address = atFixedAddress( *target ) && isKnown( expression( *operand.right ) );
      return result;
    }
    if ( operand.kind == ExpressionKind::MEMBER )
    {
      Typed const reachedMember = expression( operand );
      if ( !reachedMember.valid )
      {
        return refused();
      }
      // In stripes a byte has an address, and a value of two bytes none.
      if ( throughStripes( operand ) &&
           ( reachedMember.aggregate.has_value() || ir::sizeOf( reachedMember.type.value_or( ir::Type::U8 ) ) != 1 ) )
      {
        stripedElement( node, textOf( operand.span ) );
        return refused();
      }
      markRoot( operand );
      Typed result =
          pointing( pointeeOf( typeOfTyped( reachedMember ), reachedMember.readOnly, reachedMember.isVolatile ) );
      result.address = fixedPlace( operand );
      return result;
    }
    if ( operand.kind != ExpressionKind::IDENTIFIER )
    {
      refuse( node.span, "`&` of anything but an object, an element or what a pointer points at" );
      return refused();
    }
    std::optional<Meaning> const meaning = resolve( operand );
    if ( !meaning.has_value() )
    {
      return refused();
    }
    if ( meaning->kind == NameKind::FUNCTION || meaning->kind == NameKind::ASSEMBLER_PROC )
    {
      // The address of a function is a pointer to a named function type, and
      // where it goes decides which — see docs/decisions/0065-handlers.md.
      // The taking is carried here; the assignment holds it to the type's
      // signature and records the member.
      Typed result = typed( ir::Type::POINTER );
      result.address = true;
      result.taken = meaning->definition;
      result.takenProc = meaning->external;
      result.takenAt = operand.token;
      return result;
    }
    if ( meaning->kind != NameKind::OBJECT )
    {
      refuse( node.span, "`&` of anything but an object, an element or what a pointer points at" );
      return refused();
    }
    if ( meaning->isParameter )
    {
      name( diag::DiagnosticId::C_ADDRESS_OF_PARAMETER, operand );
      return refused();
    }
    if ( meaning->isArray )
    {
      refuse( node.span, "`&` of a whole array" );
      return refused();
    }
    if ( meaning->pointee.has_value() )
    {
      refuse( node.span, "a pointer to a pointer" );
      return refused();
    }
    if ( meaning->value.has_value() || isOpaqueConstant( *meaning ) )
    {
      refuse( node.span, "`&` of a `const` that takes no byte" );
      return refused();
    }
    addressed( *meaning );
    Typed result = pointing( pointeeOf( *meaning, meaning->isConst, meaning->isVolatile ) );
    result.address = atFixedAddress( *meaning );
    return result;
  }

  /// An operator with a pointer: `p + n`, `n + p` and `p - n` over any
  /// integer, `p - q` of one pointee counting elements, and `==` and `!=` of
  /// one pointee or with `nullptr` — see
  /// docs/decisions/0084-widths-data-arrays-pointers-aggregates.md#pointers--task-3d.
  Typed pointerOperator( Expression const& node, Typed const& left, Typed const& right )
  {
    TokenKind const op = node.token.kind;
    auto const offset = [this]( Expression const& expression, Typed const& kind )
    {
      if ( kind.pointee.has_value() || kind.isNull || kind.enumeration.has_value() || kind.type == ir::Type::BOOL ||
           kind.aggregate.has_value() )
      {
        return false;
      }
      if ( kind.type.has_value() )
      {
        return true;
      }
      std::int64_t const value = fold( expression, std::nullopt );
      if ( value >= lowestOf( ir::Type::I16 ) && value <= highestOf( ir::Type::U16 ) )
      {
        return true;
      }
      // An offset is an `i16` or a `u16`, and the one it ran past is named.
      add( diagnostic( diag::DiagnosticId::C_CONSTANT_OUT_OF_RANGE )
               .at( expression.span.begin, expression.span.length )
               .arg( "value", value )
               .arg( "type", std::string{ value < 0 ? "i16" : "u16" } ) );
      return false;
    };
    bool const leftPointer = left.pointee.has_value();
    bool const rightPointer = right.pointee.has_value();
    if ( op == TokenKind::PLUS && ( leftPointer || rightPointer ) && !( leftPointer && rightPointer ) )
    {
      bool const fits = leftPointer ? offset( *node.right, right ) : offset( *node.left, left );
      return fits ? pointing( leftPointer ? *left.pointee : *right.pointee ) : refused();
    }
    if ( op == TokenKind::MINUS && leftPointer && !rightPointer && !right.isNull )
    {
      return offset( *node.right, right ) ? pointing( *left.pointee ) : refused();
    }
    if ( op == TokenKind::MINUS && leftPointer && rightPointer )
    {
      if ( samePointee( *left.pointee, *right.pointee ) )
      {
        return typed( ir::Type::I16 );
      }
      pointersDiffer( node, left, right );
      return refused();
    }
    if ( op == TokenKind::EQUAL_EQUAL || op == TokenKind::BANG_EQUAL )
    {
      if ( left.isNull && right.isNull )
      {
        refuse( node.span, "a comparison of two constants" );
        return refused();
      }
      if ( ( leftPointer && right.isNull ) || ( left.isNull && rightPointer ) ||
           ( leftPointer && rightPointer && samePointee( *left.pointee, *right.pointee ) ) )
      {
        return typed( ir::Type::BOOL );
      }
      pointersDiffer( node, left, right );
      return refused();
    }
    if ( ( op == TokenKind::PLUS || op == TokenKind::MINUS ) && ( left.isNull || right.isNull ) )
    {
      pointersDiffer( node, left, right );
      return refused();
    }
    refuse( node.span, "this operator of a pointer" );
    return refused();
  }

  void mixedBool( Expression const& node )
  {
    add( diagnostic( diag::DiagnosticId::C_BOOL_MIXED ).at( node.span.begin, node.span.length ) );
  }

  void differ( Expression const& node, Typed const& left, Typed const& right )
  {
    add( diagnostic( diag::DiagnosticId::C_TYPES_DIFFER )
             .at( node.span.begin, node.span.length )
             .arg( "left", describe( left ) )
             .arg( "right", describe( right ) ) );
  }

  void computed( Expression const& node, std::uint32_t enumeration )
  {
    add( diagnostic( diag::DiagnosticId::C_ENUMERATION_COMPUTED )
             .at( node.span.begin, node.span.length )
             .arg( "type", mNames->enumeration( enumeration ).name ) );
  }

  Typed identifier( Expression const& node )
  {
    std::optional<Meaning> const meaning = resolve( node );
    if ( !meaning.has_value() )
    {
      return refused();
    }
    switch ( meaning->kind )
    {
    case NameKind::OBJECT:
    {
      if ( meaning->isArray && meaning->isStriped )
      {
        stripedElement( node, spellingOf( node.token ) );
        return refused();
      }
      if ( meaning->isArray )
      {
        // An array's name alone is a pointer to its first element, as C has it.
        addressed( *meaning );
        Typed result = pointing( pointeeOf( *meaning, meaning->isConst, meaning->isVolatile ) );
        result.address = atFixedAddress( *meaning );
        return result;
      }
      if ( isFoldedConstant( *meaning ) )
      {
        return constant();
      }
      if ( isOpaqueConstant( *meaning ) )
      {
        // Known to the assembler where the text is assembled, and so a value
        // given where an object is declared, as an address is: a `u16`, or
        // what an `extern` declares.
        Typed result = meaning->definition != nullptr ? typedAs( *meaning ) : typed( ir::Type::U16 );
        result.address = true;
        return result;
      }
      Typed result = typedAs( *meaning );
      result.readOnly = meaning->aggregate.has_value() && meaning->isConst;
      result.isVolatile = meaning->aggregate.has_value() && meaning->isVolatile;
      result.known = meaning->value.has_value();
      return result;
    }
    case NameKind::TYPE:
      name( diag::DiagnosticId::C_NOT_DECLARED, node );
      return refused();
    case NameKind::FUNCTION:
    case NameKind::ASSEMBLER_PROC:
    {
      // A function's name alone is its address, as C has it and as an array's
      // name above is its first element's. Which named type the pointer is of
      // is decided where it goes, and the taking is what makes the function a
      // member of it, exactly as for `&f` — see
      // docs/decisions/0065-handlers.md.
      Typed result = typed( ir::Type::POINTER );
      result.address = true;
      result.taken = meaning->definition;
      result.takenProc = meaning->external;
      result.takenAt = node.token;
      return result;
    }
    case NameKind::ASSEMBLER_OTHER:
      return refused();
    }
    return refused();
  }

  Typed qualified( Expression const& node )
  {
    std::optional<Meaning> const meaning = resolve( *node.left );
    if ( !meaning.has_value() )
    {
      return refused();
    }
    if ( meaning->kind != NameKind::TYPE || !meaning->enumeration.has_value() )
    {
      // Only a local of the type's spelling hides it here, since anything
      // else was made one name again before the check.
      name( diag::DiagnosticId::C_NOT_DECLARED, node );
      return refused();
    }
    if ( !mNames->enumeratorIndex( *mUnit, node ).has_value() )
    {
      add( diagnostic( diag::DiagnosticId::C_NO_SUCH_ENUMERATOR )
               .at( node.right->token.location, node.right->token.length )
               .arg( "type", mNames->enumeration( *meaning->enumeration ).name )
               .arg( "name", spellingOf( node.right->token ) ) );
      return refused();
    }
    Typed result = enumerated( *meaning->enumeration );
    result.known = true;
    return result;
  }

  /// Whether a type may stand under an arithmetic operator, reporting where
  /// it may not.
  bool arithmetic( Expression const& node, ir::Type type )
  {
    if ( type == ir::Type::BLOCK )
    {
      aggregateOperand( node );
      return false;
    }
    if ( type == ir::Type::BOOL )
    {
      mixedBool( node );
      return false;
    }
    return true;
  }

  void stripedElement( Expression const& node, std::string const& name )
  {
    add( diagnostic( diag::DiagnosticId::C_STRIPED_ELEMENT )
             .at( node.span.begin, node.span.length )
             .arg( "name", name ) );
  }

  /// The array or the pointer `t[i]` names, reporting where it names neither.
  std::optional<Meaning> arrayOf( Expression const& node )
  {
    Expression const& target = *node.left;
    if ( target.kind != ExpressionKind::IDENTIFIER )
    {
      add( diagnostic( diag::DiagnosticId::C_NOT_AN_ARRAY )
               .at( target.span.begin, target.span.length )
               .arg( "name", textOf( target.span ) ) );
      return std::nullopt;
    }
    std::optional<Meaning> meaning = resolve( target );
    if ( !meaning.has_value() )
    {
      return std::nullopt;
    }
    if ( meaning->kind != NameKind::OBJECT || ( !meaning->isArray && !meaning->pointee.has_value() ) )
    {
      name( diag::DiagnosticId::C_NOT_AN_ARRAY, target );
      return std::nullopt;
    }
    return meaning;
  }

  /// A constant index inside the array, reporting one outside.
  bool indexInside( Expression const& index, Meaning const& array, Token const& name )
  {
    std::int64_t const value = fold( index, std::nullopt );
    std::int64_t const count = array.count.value_or( 0 );
    // An array of the assembler's whose count a string leaves unknown checks
    // no constant index.
    if ( ( value >= 0 && value < count ) || ( array.isArray && !array.count.has_value() ) )
    {
      return true;
    }
    add( diagnostic( diag::DiagnosticId::C_INDEX_OUTSIDE )
             .at( index.span.begin, index.span.length )
             .arg( "index", value )
             .arg( "name", spellingOf( name ) )
             .arg( "count", count )
             .arg( "last", count - 1 ) );
    return false;
  }

  /// `t[i]`: an element of an array at a static address, indexed by a
  /// constant inside it or by an unsigned integer — see
  /// docs/decisions/0084-widths-data-arrays-pointers-aggregates.md#arrays--task-3c;
  /// or what a pointer points at, `i` elements on, by any integer.
  Typed element( Expression const& node )
  {
    // An expression that is no name is indexed as the pointer it is: a member
    // array, or a pointer a call returns.
    if ( node.left->kind != ExpressionKind::IDENTIFIER )
    {
      Typed const target = expression( *node.left );
      Typed const index = expression( *node.right );
      if ( !target.valid || !index.valid )
      {
        return refused();
      }
      if ( !target.pointee.has_value() )
      {
        add( diagnostic( diag::DiagnosticId::C_NOT_AN_ARRAY )
                 .at( node.left->span.begin, node.left->span.length )
                 .arg( "name", textOf( node.left->span ) ) );
        return refused();
      }
      if ( !isKnown( index ) || index.pointee.has_value() || index.isNull )
      {
        ir::Type const indexType = index.type.value_or( ir::Type::U8 );
        if ( index.enumeration.has_value() || index.pointee.has_value() || index.isNull ||
             index.aggregate.has_value() || indexType == ir::Type::BOOL )
        {
          add( diagnostic( diag::DiagnosticId::C_INDEX_NOT_U8 )
                   .at( node.right->span.begin, node.right->span.length )
                   .arg( "allowed", std::string{ "an integer" } )
                   .arg( "what", describe( index ) ) );
          return refused();
        }
      }
      return reached( *target.pointee );
    }
    std::optional<Meaning> const array = arrayOf( node );
    Typed const index = expression( *node.right );
    if ( !array.has_value() || !index.valid )
    {
      return refused();
    }
    bool const pointer = array->pointee.has_value();
    Typed result = reached( array->pointee.value_or( pointeeOf( *array ) ) );
    result.readOnly = pointer ? result.readOnly : array->isConst;
    result.isVolatile = pointer ? result.isVolatile : array->isVolatile;
    result.throughPointer = pointer;
    if ( isKnown( index ) && !index.pointee.has_value() && !index.isNull )
    {
      return pointer || indexInside( *node.right, *array, node.left->token ) ? result : refused();
    }
    ir::Type const indexType = index.type.value_or( ir::Type::U8 );
    bool const integer = !index.enumeration.has_value() && !index.pointee.has_value() && !index.isNull &&
                         !index.aggregate.has_value() && indexType != ir::Type::BOOL;
    // A striped array's stripes share no address, so its index is `X`'s.
    if ( array->isStriped && ( !integer || indexType != ir::Type::U8 ) )
    {
      add( diagnostic( diag::DiagnosticId::C_INDEX_NOT_U8 )
               .at( node.right->span.begin, node.right->span.length )
               .arg( "allowed", std::string{ "a `u8` or a constant inside the array, which is striped" } )
               .arg( "what", describe( index ) ) );
      return refused();
    }
    if ( !integer || ( !pointer && ir::isSigned( indexType ) ) )
    {
      add( diagnostic( diag::DiagnosticId::C_INDEX_NOT_U8 )
               .at( node.right->span.begin, node.right->span.length )
               .arg( "allowed",
                     std::string{ pointer ? "an integer" : "a `u8`, a `u16` or a constant inside the array" } )
               .arg( "what", describe( index ) ) );
      return refused();
    }
    // An element at a 16-bit index is reached through the array's address as
    // a number, which a Temporary has none of: the array is then a Section,
    // as one whose address `&` takes is — see
    // docs/decisions/0294-a-local-array-at-a-wide-index-is-a-section.md.
    if ( !pointer && ir::sizeOf( indexType ) == 2 )
    {
      addressed( *array );
    }
    return result;
  }

  /// `sizeof`: of a type, an object, an array or an element, never computed;
  /// an integer constant, as a literal is.
  Typed sizeOf( Expression const& node )
  {
    if ( node.left == nullptr )
    {
      return constant();
    }
    Expression const& operand = *node.left;
    if ( operand.kind == ExpressionKind::MEMBER || operand.kind == ExpressionKind::STRING_LITERAL )
    {
      return expression( operand ).valid ? constant() : refused();
    }
    bool const reaches = operand.kind == ExpressionKind::INDEX || operand.kind == ExpressionKind::DEREFERENCE;
    Expression const* const named = reaches ? operand.left.get() : &operand;
    if ( named->kind != ExpressionKind::IDENTIFIER )
    {
      refuse( node.span, "`sizeof` of anything but a type, an object, an element or what a pointer points at" );
      return refused();
    }
    std::optional<Meaning> const meaning = resolve( *named );
    if ( !meaning.has_value() )
    {
      return refused();
    }
    if ( reaches && ( meaning->kind != NameKind::OBJECT || ( !meaning->isArray && !meaning->pointee.has_value() ) ) )
    {
      name( diag::DiagnosticId::C_NOT_AN_ARRAY, *named );
      return refused();
    }
    if ( operand.kind == ExpressionKind::DEREFERENCE || meaning->pointee.has_value() )
    {
      return constant();
    }
    if ( operand.kind == ExpressionKind::INDEX )
    {
      if ( !isKnown( expression( *operand.right ) ) )
      {
        return constant();
      }
      return indexInside( *operand.right, *meaning, named->token ) ? constant() : refused();
    }
    if ( ( meaning->kind != NameKind::OBJECT && meaning->kind != NameKind::TYPE ) ||
         ( isAssemblerConstant( *meaning ) && meaning->definition == nullptr ) )
    {
      refuse( node.span, "`sizeof` of anything but a type, an object or an element" );
      return refused();
    }
    // An array with no count is one whose declaration was refused already: no
    // size where it is defined (`NGA7243`), or an `extern` naming nothing
    // (`NGA7270`).
    if ( meaning->isArray && !meaning->count.has_value() )
    {
      return refused();
    }
    return constant();
  }

  /// `(type)x`: an integer to any integer, and an `enum struct` or a `bool` to
  /// an integer; nothing to a `bool` but a `bool` — see
  /// docs/decisions/0084-widths-data-arrays-pointers-aggregates.md.
  Typed cast( Expression const& node )
  {
    Typed const operand = expression( *node.left );
    if ( !operand.valid )
    {
      return refused();
    }
    if ( operand.aggregate.has_value() )
    {
      aggregateOperand( node );
      return refused();
    }
    auto const castRefused = [&]( std::string const& type )
    {
      add( diagnostic( diag::DiagnosticId::C_CAST_REFUSED )
               .at( node.span.begin, node.span.length )
               .arg( "what", describe( operand ) )
               .arg( "type", type ) );
      return refused();
    };
    // A function's address goes to a pointer of a function type and to
    // nothing else, the taking being what makes the function a member of one
    // — see docs/decisions/0065-handlers.md. No cast turns it into a number.
    if ( operand.taken != nullptr || operand.takenProc != nullptr )
    {
      return castRefused(
          std::string{ node.pointeeConst ? "const " : "" } + std::string{ node.pointeeVolatile ? "volatile " : "" } +
          std::string{ ir::spellingOf( typeNamed( node.token.keyword ) ) } + ( node.pointer ? "*" : "" ) );
    }
    // A pointer casts to a pointer or a `u16`, and a `u16` or a constant to a
    // pointer: an address is what a register of the machine has.
    if ( node.pointer )
    {
      Pointee const pointee =
          pointeeOf( ValueType{ .type = typeNamed( node.token.keyword ) }, node.pointeeConst, node.pointeeVolatile );
      bool const fromAddress =
          operand.pointee.has_value() || operand.isNull ||
          ( !operand.enumeration.has_value() && ( !operand.type.has_value() || operand.type == ir::Type::U16 ) );
      if ( !fromAddress )
      {
        return castRefused( std::string{ node.pointeeConst ? "const " : "" } +
                            std::string{ node.pointeeVolatile ? "volatile " : "" } +
                            std::string{ ir::spellingOf( pointee.type ) } + "*" );
      }
      if ( !operand.type.has_value() && !constantFits( *node.left, ir::Type::U16 ) )
      {
        return refused();
      }
      Typed result = pointing( pointee );
      result.known = isKnown( operand ) && !operand.isNull;
      return result;
    }
    if ( operand.pointee.has_value() || operand.isNull )
    {
      if ( typeNamed( node.token.keyword ) != ir::Type::U16 )
      {
        return castRefused( std::string{ ir::spellingOf( typeNamed( node.token.keyword ) ) } );
      }
      Typed result = typed( ir::Type::U16 );
      result.known = operand.known;
      return result;
    }
    ir::Type const target = typeNamed( node.token.keyword );
    bool const fromBool = !operand.enumeration.has_value() && operand.type == ir::Type::BOOL;
    if ( target == ir::Type::BOOL && !fromBool )
    {
      add( diagnostic( diag::DiagnosticId::C_CAST_REFUSED )
               .at( node.span.begin, node.span.length )
               .arg( "what", describe( operand ) )
               .arg( "type", std::string{ ir::spellingOf( target ) } ) );
      return refused();
    }
    Typed result = typed( target );
    result.known = isKnown( operand ) && !operand.enumeration.has_value() && !fromBool;
    std::optional<ir::Type> const source = operand.type;
    if ( !result.known && !operand.enumeration.has_value() && source.has_value() &&
         ir::sizeOf( *source ) < ir::sizeOf( target ) && ( ir::isSigned( target ) || !ir::isSigned( *source ) ) )
    {
      // Where a signed value is widened to an unsigned type, what it holds is
      // no longer one range.
      result.widenedFrom = source;
    }
    return result;
  }

  Typed unary( Expression const& node )
  {
    Typed const operand = expression( *node.left );
    if ( operand.valid && operand.aggregate.has_value() )
    {
      aggregateOperand( node );
      return refused();
    }
    if ( operand.valid && ( operand.pointee.has_value() || operand.isNull ) )
    {
      refuse( node.span, "this operator of a pointer" );
      return refused();
    }
    if ( operand.valid && operand.enumeration.has_value() )
    {
      computed( node, *operand.enumeration );
      return refused();
    }
    if ( node.token.kind == TokenKind::BANG )
    {
      if ( !operand.valid )
      {
        return refused();
      }
      if ( operand.type != ir::Type::BOOL )
      {
        mixedBool( *node.left );
        return refused();
      }
      Typed result = typed( ir::Type::BOOL );
      if ( operand.decided.has_value() )
      {
        result.decided = !*operand.decided;
      }
      return result;
    }
    if ( !operand.valid || !operand.type.has_value() )
    {
      return operand;
    }
    if ( !arithmetic( node, *operand.type ) )
    {
      return refused();
    }
    Typed result = typed( *operand.type );
    result.known = operand.known;
    return result;
  }

  /// Two typed operands of one operator, of one signedness: the operator works
  /// in the wider of their types.
  Typed agreeing( Expression const& node, ir::Type left, ir::Type right )
  {
    if ( !arithmetic( node, left ) || !arithmetic( node, right ) )
    {
      return refused();
    }
    if ( ir::isSigned( left ) != ir::isSigned( right ) )
    {
      add( diagnostic( diag::DiagnosticId::C_MIXED_SIGNEDNESS )
               .at( node.token.location, node.token.length )
               .arg( "left", std::string{ ir::spellingOf( left ) } )
               .arg( "right", std::string{ ir::spellingOf( right ) } ) );
      return refused();
    }
    return typed( widerOf( left, right ) );
  }

  Typed binary( Expression const& node )
  {
    TokenKind const op = node.token.kind;
    if ( isLogical( op ) )
    {
      // Their operands are conditions, and their answer is a `bool` a byte
      // takes from either path — see
      // docs/decisions/0079-a-condition-is-a-bool.md.
      bool const left = condition( *node.left );
      bool const right = condition( *node.right );
      return left && right ? typed( ir::Type::BOOL ) : refused();
    }

    Typed const left = expression( *node.left );
    Typed const right = expression( *node.right );
    if ( !left.valid || !right.valid )
    {
      return refused();
    }
    if ( left.aggregate.has_value() || right.aggregate.has_value() )
    {
      aggregateOperand( node );
      return refused();
    }
    if ( left.pointee.has_value() || left.isNull || right.pointee.has_value() || right.isNull )
    {
      return pointerOperator( node, left, right );
    }
    if ( op == TokenKind::STAR || op == TokenKind::SLASH || op == TokenKind::PERCENT )
    {
      // Folded where both sides are constants, so that `sizeof t / sizeof t[0]`
      // counts, and computed otherwise — see
      // docs/decisions/0095-literals-and-the-runtime.md. A constant divisor of
      // zero is refused; one a run computes is the runtime's.
      std::optional<ir::Type> const type = left.type.has_value() ? left.type : right.type;
      if ( op != TokenKind::STAR && isKnown( right ) && !right.enumeration.has_value() &&
           fold( *node.right, type ) == 0 )
      {
        add( diagnostic( diag::DiagnosticId::C_DIVISION_BY_ZERO )
                 .at( node.right->span.begin, node.right->span.length ) );
        return refused();
      }
    }

    // Values of an `enum struct` are compared with values of their own type,
    // and never computed — see
    // docs/decisions/0078-switch-over-an-enum-struct.md#the-type.
    if ( left.enumeration.has_value() || right.enumeration.has_value() )
    {
      if ( !isComparison( op ) )
      {
        computed( node, left.enumeration.value_or( right.enumeration.value_or( 0 ) ) );
        return refused();
      }
      if ( left.enumeration != right.enumeration )
      {
        differ( node, left, right );
        return refused();
      }
      return typed( ir::Type::BOOL );
    }

    if ( isShift( op ) )
    {
      return shift( node, left, right );
    }
    if ( isComparison( op ) )
    {
      return comparison( node, left, right );
    }

    if ( !left.type.has_value() && !right.type.has_value() )
    {
      // Constants make a constant, and a `?:` of them among them makes one a
      // condition decides where the program runs.
      Typed result = constant();
      result.known = left.known && right.known;
      return result;
    }
    Typed result = refused();
    if ( left.type.has_value() && right.type.has_value() )
    {
      result = agreeing( node, *left.type, *right.type );
    }
    else
    {
      ir::Type const type = left.type.has_value() ? *left.type : *right.type;
      Expression const& constantSide = left.type.has_value() ? *node.right : *node.left;
      if ( arithmetic( node, type ) && constantFits( constantSide, type ) )
      {
        result = typed( type );
      }
    }
    result.known = result.valid && isKnown( left ) && isKnown( right );
    return result;
  }

  Typed shift( Expression const& node, Typed const& left, Typed const& right )
  {
    if ( !isKnown( right ) && !right.enumeration.has_value() )
    {
      // A count held in a `u8` is a loop, whatever it holds, and so is one a
      // `?:` of two constants decides, which a `u8` holds.
      if ( !left.type.has_value() )
      {
        refuse( node.span, "a constant shifted by a value" );
        return refused();
      }
      if ( right.type.has_value() && *right.type != ir::Type::U8 )
      {
        refuse( node.right->span, "a shift by a value that is not a `u8`" );
        return refused();
      }
      return arithmetic( node, *left.type ) ? typed( *left.type ) : refused();
    }
    if ( !left.type.has_value() )
    {
      Typed result = constant();
      result.known = left.known;
      return result;
    }
    ir::Type const type = *left.type;
    if ( !arithmetic( node, type ) )
    {
      return refused();
    }
    std::int64_t const count = fold( *node.right, std::nullopt );
    std::int64_t const largest = ( 8 * static_cast<std::int64_t>( ir::sizeOf( type ) ) ) - 1;
    if ( count < 0 || count > largest )
    {
      add( diagnostic( diag::DiagnosticId::C_SHIFT_OUT_OF_RANGE )
               .at( node.right->span.begin, node.right->span.length )
               .arg( "type", std::string{ ir::spellingOf( type ) } )
               .arg( "count", count )
               .arg( "largest", largest ) );
      return refused();
    }
    Typed result = typed( type );
    result.known = left.known;
    return result;
  }

  Typed comparison( Expression const& node, Typed const& left, Typed const& right )
  {
    // A translated character is a constant only the assembler knows, so two
    // constants one of which is one are not compared. Two the compiler knows
    // decide the comparison where it is compiled, as `WINDOW_BITS > 8` does
    // in a program written for more than one configuration — see
    // docs/decisions/0298-two-constants-compared-decide-the-comparison.md.
    auto const translated = [this]( Expression const& side )
    { return side.kind == ExpressionKind::CHARACTER_CONSTANT && isTranslated( side ); };
    bool const leftTranslated = translated( *node.left );
    bool const rightTranslated = translated( *node.right );
    if ( ( isKnown( left ) || leftTranslated ) && ( isKnown( right ) || rightTranslated ) )
    {
      if ( leftTranslated || rightTranslated )
      {
        refuse( node.span, "a comparison of two constants" );
        return refused();
      }
      std::int64_t const one = fold( *node.left, std::nullopt );
      std::int64_t const other = fold( *node.right, std::nullopt );
      bool outcome = false;
      switch ( node.token.kind )
      {
      case TokenKind::LESS:
        outcome = one < other;
        break;
      case TokenKind::LESS_EQUAL:
        outcome = one <= other;
        break;
      case TokenKind::GREATER:
        outcome = one > other;
        break;
      case TokenKind::GREATER_EQUAL:
        outcome = one >= other;
        break;
      case TokenKind::EQUAL_EQUAL:
        outcome = one == other;
        break;
      default:
        outcome = one != other;
        break;
      }
      return Typed{
        .valid = true, .type = ir::Type::BOOL, .decided = outcome, .enumeration = std::nullopt, .known = false
      };
    }
    if ( left.type.has_value() && right.type.has_value() )
    {
      return agreeing( node, *left.type, *right.type ).valid ? typed( ir::Type::BOOL ) : refused();
    }

    ir::Type const type = left.type.has_value() ? *left.type : right.type.value_or( ir::Type::U8 );
    if ( !arithmetic( node, type ) )
    {
      return refused();
    }
    Expression const& constantSide = left.type.has_value() ? *node.right : *node.left;
    std::int64_t const value = fold( constantSide, std::nullopt );

    // A value a cast widened holds only what its own type did, and a constant
    // outside that decides the comparison as one outside the wider type does.
    ir::Type const range = ( left.type.has_value() ? left : right ).widenedFrom.value_or( type );
    if ( value >= lowestOf( range ) && value <= highestOf( range ) )
    {
      return typed( ir::Type::BOOL );
    }

    // A constant the type cannot hold decides the comparison: every value of
    // the type stands on one side of it.
    bool const above = value > highestOf( range );
    TokenKind op = node.token.kind;
    if ( !left.type.has_value() )
    {
      // `c < x` is `x > c`.
      switch ( op )
      {
      case TokenKind::LESS:
        op = TokenKind::GREATER;
        break;
      case TokenKind::GREATER:
        op = TokenKind::LESS;
        break;
      case TokenKind::LESS_EQUAL:
        op = TokenKind::GREATER_EQUAL;
        break;
      case TokenKind::GREATER_EQUAL:
        op = TokenKind::LESS_EQUAL;
        break;
      default:
        break;
      }
    }
    bool outcome = false;
    switch ( op )
    {
    case TokenKind::LESS:
    case TokenKind::LESS_EQUAL:
      outcome = above;
      break;
    case TokenKind::GREATER:
    case TokenKind::GREATER_EQUAL:
      outcome = !above;
      break;
    case TokenKind::BANG_EQUAL:
      outcome = true;
      break;
    default:
      break;
    }
    add( diagnostic( diag::DiagnosticId::C_COMPARISON_ALWAYS )
             .at( constantSide.span.begin, constantSide.span.length )
             .arg( "value", value )
             .arg( "type", std::string{ ir::spellingOf( range ) } )
             .arg( "outcome", std::string{ outcome ? "true" : "false" } ) );
    return Typed{
      .valid = true, .type = ir::Type::BOOL, .decided = outcome, .enumeration = std::nullopt, .known = false
    };
  }

  /// `&f` where a pointer of a function type is wanted: the signature must be
  /// the type's, and the function is then a member of it. A member reads the
  /// type's Temporaries, so one function belongs to one type (`NGA7298`).
  void takesFunction( Expression const& value, std::optional<Pointee> const& target, Typed const& kind )
  {
    std::string const name = kind.takenAt.has_value() ? spellingOf( *kind.takenAt ) : std::string{ "a function" };
    if ( !target.has_value() || !target->function.has_value() )
    {
      add( diagnostic( diag::DiagnosticId::C_TAKEN_SIGNATURE )
               .at( value.span.begin, value.span.length )
               .arg( "name", name )
               .arg( "type", std::string{ "pointer the value goes to" } )
               .arg( "what", "the address of a function goes to a pointer of a function type alone" ) );
      return;
    }
    FunctionType const& type = mNames->functionType( *target->function );
    std::optional<std::string> const wrong = signatureDiffers( type, kind );
    if ( wrong.has_value() )
    {
      add( diagnostic( diag::DiagnosticId::C_TAKEN_SIGNATURE )
               .at( value.span.begin, value.span.length )
               .arg( "name", name )
               .arg( "type", type.name )
               .arg( "what", *wrong ) );
      return;
    }
    if ( mMembers == nullptr )
    {
      return;
    }
    if ( kind.taken != nullptr )
    {
      auto const [held, added] = mMembers->ofFunction.try_emplace( kind.taken, *target->function );
      if ( !added && held->second != *target->function )
      {
        add( diagnostic( diag::DiagnosticId::C_TAKEN_TWICE )
                 .at( value.span.begin, value.span.length )
                 .arg( "name", name )
                 .arg( "type", type.name )
                 .arg( "other", mNames->functionType( held->second ).name ) );
        return;
      }
      if ( added && kind.takenAt.has_value() )
      {
        mMembers->takenAt.try_emplace( kind.taken, *kind.takenAt );
      }
      return;
    }
    auto const [held, added] = mMembers->ofProc.try_emplace( kind.takenProc->name, *target->function );
    if ( !added && held->second != *target->function )
    {
      add( diagnostic( diag::DiagnosticId::C_TAKEN_TWICE )
               .at( value.span.begin, value.span.length )
               .arg( "name", name )
               .arg( "type", type.name )
               .arg( "other", mNames->functionType( held->second ).name ) );
    }
  }

  /// How a function differs from a type, or nothing where it does not: the
  /// count of the parameters, their types in order, and the result.
  [[nodiscard]] std::optional<std::string> signatureDiffers( FunctionType const& type, Typed const& kind ) const
  {
    if ( kind.takenProc != nullptr && kind.taken == nullptr )
    {
      // A Proc of the assembler declares its membership with `as`, and its
      // arguments are the type's; nothing else about it is read here.
      return kind.takenProc->as == type.name
                 ? std::nullopt
                 : std::optional{ kind.takenProc->as.empty()
                                      ? std::string{ "a proc of the assembler is a member where it says `as`" }
                                      : "the proc says `as " + kind.takenProc->as + "`" };
    }
    return signatureDiffers( type, *kind.taken );
  }

  diag::SourceManager const* mSources;
  Names const* mNames;
  Unit const* mUnit;
  diag::DiagnosticSink* mSink;

  /// The expression a compound assignment used as a value is held against.
  Expression const* mRoot = nullptr;

  /// Where a taking of a function's address is recorded, over the whole
  /// program; nothing where the walk is not the Checker's.
  Membership* mMembers = nullptr;

  /// The function whose body is being read, for what is recorded of it.
  Definition const* mWithin = nullptr;

  /// The locals of the blocks being walked, innermost last.
  std::vector<std::map<std::string, Meaning, std::less<>>> mScopes;

  std::vector<CallSite>* mCallSites = nullptr;
  std::set<std::uint32_t>* mAddressed = nullptr;

  /// The literals whose prefix has been checked, by where they stand.
  std::set<std::uint32_t> mCheckedPrefixes;
};

} // namespace nga::c::detail
