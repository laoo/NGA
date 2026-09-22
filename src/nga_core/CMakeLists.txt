add_library( nga_core
  src/Version.cpp
  src/PortabilityChecks.cpp
  src/c/Compile.cpp
  src/c/Fold.cpp
  src/c/Hoist.cpp
  src/c/Inline.cpp
  src/c/Ir.cpp
  src/c/Lexer.cpp
  src/c/Liveness.cpp
  src/c/Loops.cpp
  src/c/Narrow.cpp
  src/c/Parser.cpp
  src/c/Ranges.cpp
  src/c/Result.cpp
  src/c/Reuse.cpp
  src/c/Select.cpp
  src/c/Token.cpp
  src/c/Walk.cpp
  src/diag/Diagnostic.cpp
  src/diag/DiagnosticId.cpp
  src/diag/DiagnosticSink.cpp
  src/diag/Renderer.cpp
  src/diag/SeverityPolicy.cpp
  src/diag/SourceManager.cpp
  src/model/Assemble.cpp
  src/model/Charsets.cpp
  src/model/Emit.cpp
  src/model/Generator.cpp
  src/model/Script.cpp
  src/model/MemoryMap.cpp
  src/model/Evaluate.cpp
  src/model/Isa.cpp
  src/model/Expand.cpp
  src/model/Merge.cpp
  src/model/Module.cpp
  src/model/ModuleBuilder.cpp
  src/model/Patch.cpp
  src/model/Pipeline.cpp
  src/model/Place.cpp
  src/model/PlaceStorage.cpp
  src/model/Transform.cpp
  src/model/Dispatch.cpp
  src/model/Transition.cpp
  src/model/VerifyLayout.cpp
  src/model/Xex.cpp
  src/model/Zx0.cpp
  src/model/PlacementClass.cpp
  src/model/Project.cpp
  src/model/Driver.cpp
  src/model/Panes.cpp
  src/model/With.cpp
  src/model/Constants.cpp
  src/model/Regions.cpp
  src/model/Runtime.cpp
  src/model/Suppressions.cpp
  src/model/Prune.cpp
  src/model/Trace.cpp
  src/model/References.cpp
  src/model/ProjectFile.cpp
  src/model/Section.cpp
  src/model/Size.cpp
  src/model/Slots.cpp
  src/model/Solver.cpp
  src/model/TypeCheck.cpp
  src/model/Types.cpp
  src/model/Symbol.cpp
  src/syntax/Operand.cpp
  src/syntax/Expression.cpp
  src/syntax/ExpressionParser.cpp
  src/syntax/Lexer.cpp
  src/syntax/Literal.cpp
  src/syntax/Parser.cpp
  src/syntax/Payload.cpp
  src/syntax/ProjectParser.cpp
  src/syntax/Token.cpp
  src/syntax/TokenCursor.cpp )

add_library( nga::core ALIAS nga_core )

target_include_directories( nga_core PUBLIC include )

target_compile_definitions( nga_core PRIVATE NGA_VERSION="${PROJECT_VERSION}" )

# ortools::ortools is PRIVATE because no NGA header includes an OR-Tools header,
# and keeping it that way is what makes the solver replaceable later: the model
# reaches CP-SAT through plain structs, in one .cpp. Its headers come in as
# SYSTEM, since the target is imported, so the warning set does not see them.
target_link_libraries( nga_core
  PUBLIC  spdlog::spdlog
  PRIVATE nga::warnings ortools::ortools quickjs )
