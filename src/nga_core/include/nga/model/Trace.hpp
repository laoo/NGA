#pragma once

#include "nga/diag/DiagnosticSink.hpp"
#include "nga/model/Build.hpp"
#include "nga/model/Chunk.hpp"

#include <cstdint>
#include <set>
#include <utility>

namespace nga::model
{

/// The result of the Trace Step: which pairs of Sections may share addresses.
///
/// Every pair interferes unless it is allowed here, and a pair is allowed
/// only when both are Temporaries whose Livenesses are disjoint — no state of
/// the program has an Owner of each active at once. Whether the two are ever
/// in memory together is the other half of the rule and is the solver's to
/// read from their Residencies: two Sections interfere when their Residencies
/// intersect *and* their Livenesses intersect, and this holds the second
/// half. Read by Place and by the layout verifier. See the glossary.
class Interference
{
public:
  void allow( SectionRef a, SectionRef b );

  [[nodiscard]] bool mayShare( SectionRef a, SectionRef b ) const;

private:
  static std::pair<std::uint64_t, std::uint64_t> keyOf( SectionRef a, SectionRef b );

  std::set<std::pair<std::uint64_t, std::uint64_t>> mAllowed;
};

/// The Trace Step: the stack of Activations a program can have, over the
/// Calls and Jumps between Sections, each at a Chunk, and from it which
/// Temporaries are never live at once.
///
/// A Section is the node. P and Q can be active at once iff a path from
/// one to the other begins with a Call — the caller stays on the stack for
/// everything the callee does, however it goes on — while a Jump ends the
/// jumper's Activation, so a cycle of Jumps alone is a state machine and no
/// interference. Within a Section a Temporary is live at the Chunks between
/// a write of a byte of it and a read, along the Chunks' Successors, and at
/// every Chunk that touches it; a Call needs what its callee needs on
/// entry. Two Temporaries interfere when some state has both live: at one
/// Chunk, or one at a Call the other is live below. A Section that calls
/// into itself has the calling Chunk waiting while any of its Chunks run;
/// a `root` Section is active in every state, and so is everything it
/// reaches; a Section that owns an address of a position in itself has no
/// known flow, and is live throughout. A Temporary whose own address
/// escaped shares with nothing and is warned about, and one owned by a
/// Section that can call itself is an error, since a second Activation would
/// destroy the first's value. See docs/decisions/0034-trace.md and
/// 0075-liveness-within-an-activation.md.
Interference trace( Pruned const& build, diag::DiagnosticSink& sink );

} // namespace nga::model
