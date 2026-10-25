#pragma once

#include "nga/c/Front.hpp"

#include <cstddef>
#include <cstdint>
#include <set>
#include <vector>

namespace nga::c::detail
{

/// A call of one function of the program by another, and where it stands:
/// an edge of the call graph recursion is refused on.
struct CallEdge
{
  Definition const* caller = nullptr;
  Definition const* callee = nullptr;

  /// The file it stands in, by its place among the program's.
  std::size_t unit = 0;
  diag::SourceSpan span;
  diag::DiagnosticSink* sink = nullptr;
};

/// The type check of one file, reporting into its sink: and, over the whole
/// program, the calls each function makes, the locals of this file whose
/// address is taken, and which function type each function taken as one
/// belongs to.
void checkUnit( diag::SourceManager const& sources,
                Names const& names,
                Unit const& unit,
                std::size_t index,
                std::vector<CallEdge>& edges,
                std::set<std::uint32_t>& addressed,
                Membership& members );

/// Recursion refused on the program's call graph: each cycle once, at the
/// call closing it that stands last in the order the files were given, with
/// the cycle named — see docs/decisions/0082-a-call-writes-the-callees-bytes.md.
/// The graph's components are found by Tarjan's algorithm, run on a stack of
/// its own so that a long chain of calls costs no depth of the tool's.
void refuseRecursion( diag::SourceManager const& sources, std::vector<CallEdge> const& edges );

/// A member of a function type that calls its own type — through a pointer of
/// it, or by naming another member — writes the Temporaries it is reading
/// itself. The copy 0065 proposed does not save it: the type's jump reaches
/// every member, so the call may come back to this one and write the copy too,
/// which the recursion rule then refuses. The owner gave the shape up rather
/// than keep it — see docs/decisions/0104-handlers-stop-at-one-dispatch.md — so
/// it is refused here, where the finding can name it.
void refuseCallsOfOwnType( std::vector<CallEdge> const& edges, Membership const& members );

} // namespace nga::c::detail
