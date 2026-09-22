#pragma once

#include "nga/c/Ir.hpp"
#include "nga/diag/DiagnosticSink.hpp"

#include <optional>
#include <span>

namespace nga::c
{

/// Wraps a call into its caller where it is the only call of its callee the
/// program's C writes: the body is spliced where the call stood, and the
/// callee's Proc is **left standing** for Prune to drop where nothing reaches
/// it any more, so that correctness never rests on knowing every caller — an
/// `.asm` Module's included, which the IR cannot see. One copy of a body
/// replaces one call of it, which is why the rule needs no justification of
/// any other kind; the bytes it costs are another matter, since a spliced
/// body has neither the parameters of a Proc nor a `__ret` and loses what
/// 0145, 0120 and `mergeReturnedLocals` gave it at that boundary. What is
/// refused, and why each shape is, is the table of
/// docs/decisions/0172-a-call-with-one-site-is-wrapped.md.
///
/// A function written `inline` is wrapped at **every** call of it instead,
/// which is not free and is therefore the programmer's to ask for; and where
/// one of those calls meets the table, the refusal is a finding rather than
/// the silence it is without the word. `sinks` holds one per unit, in the
/// units' own order, and a null where a unit has none.
///
/// Over the whole program, since a callee is another unit's as often as its
/// own, and callees before their callers, which terminates because recursion
/// is already refused
/// (docs/decisions/0082-a-call-writes-the-callees-bytes.md).
/// `forConstants` is the third trigger, and it is the Intent's first reader:
/// under `speed`, a call is wrapped because an argument of it is a constant,
/// so that 0175's propagation and the fold of a constant divisor reach the
/// body. It is worth 239 580 cycles on `prime` for two bytes and takes `puff`
/// out of its View, which is why it is asked for and not assumed — see
/// docs/decisions/0177-intent.md.
void wrapCalls( std::span<std::optional<ir::Unit>> units,
                std::span<diag::DiagnosticSink* const> sinks,
                bool forConstants );

} // namespace nga::c
