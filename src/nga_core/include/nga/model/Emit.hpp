#pragma once

#include "nga/diag/DiagnosticSink.hpp"
#include "nga/model/Build.hpp"
#include "nga/model/Merge.hpp"
#include "nga/model/Patch.hpp"
#include "nga/model/Place.hpp"
#include "nga/model/Size.hpp"
#include "nga/model/Storage.hpp"

#include <cstdint>
#include <vector>

namespace nga::model
{

/// The memory a program occupies, from its lowest byte to its highest.
struct RawImage
{
  std::uint32_t origin = 0;
  std::vector<std::uint8_t> bytes;
};

/// The Emit Step in the raw Container: no header and no segments, just the
/// bytes at the addresses they were placed at, with the holes between Sections
/// left as zero. It spans the Sections that emit bytes; a reservation puts
/// nothing in it.
///
/// A raw image is memory at one instant, so two Sections that emit bytes and
/// share an address — legal under the co-visibility rule when their Residency
/// is disjoint — cannot both be in it. Such a Layout is refused, and nothing
/// is emitted, rather than one Section being written over the other. Two
/// reserved buffers at one address are a hole either way and are not refused.
///
/// A Container that says where its parts belong — `.xex` and the cartridge
/// images — is the next one and not a change to this; the one that can hold
/// an Overlay is in docs/open-questions.md.
RawImage emitRawImage( Patched const& build, diag::DiagnosticSink& sink );

/// An Atari DOS binary load file, as bytes. The CLI writes them; `nga_core`
/// does no I/O.
struct XexFile
{
  std::vector<std::uint8_t> bytes;
};

/// The Emit Step in the `.xex` Container, which is the one that holds a
/// Payload: the Sections the entry Phase needs at their runtime addresses,
/// the driver among them; then the Banks, each shown through the driver's
/// `showAt` and filled by a segment per Payload at `window + offset`; base
/// memory back through the driver; and `RUNAD` at the entry Phase's entry
/// Label — the first Root, resolved here because this is when addresses
/// exist. The layout is the contract in docs/spec/xex.md and the reasoning
/// is docs/decisions/0018-xex-container.md.
///
/// Refused, with nothing emitted, when a Payload waits in storage and no
/// driver names the Window its stream reads through, and when the entry
/// Phase has no entry Label defined exactly once among the Modules it needs.
XexFile emitXex( Patched const& build, diag::DiagnosticSink& sink );

} // namespace nga::model
