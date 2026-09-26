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

/// A diskette, as bytes: the sixteen-byte header every reader knows an `.atr`
/// by, then 720 sectors of 128 bytes. The CLI writes them; `nga_core` does no
/// I/O.
struct AtrFile
{
  std::vector<std::uint8_t> bytes;
};

/// The Emit Step in the `.atr` Container, the one whose storage is a medium
/// nothing maps: the boot record in the first three sectors, storage a sector
/// to the unit after it, and then the load image — the Sections the entry
/// Phase needs as the segments a `.xex` carries, which the boot record's
/// loader reads through the OS's disk handler. The layout is the contract in
/// docs/spec/atr.md and the reasoning is
/// docs/decisions/0211-a-diskette-is-a-container-and-its-sectors-are-storage.md.
///
/// Refused, with nothing emitted, when storage is a unit set or its units are
/// not sectors, when the driver streams through a Window, when a Section the
/// image loads stands in the boot record, and when the whole comes to more
/// sectors than a single-density diskette has.
AtrFile emitAtr( Patched const& build, diag::DiagnosticSink& sink );

/// A cartridge image, as bytes: the sixteen-byte header every reader knows a
/// `.car` by, and then the board's ROM exactly as large as the board is. The
/// CLI writes them; `nga_core` does no I/O.
struct CarFile
{
  std::vector<std::uint8_t> bytes;
};

/// The Emit Step in the `.car` Container, the one that loads nothing: every
/// Section standing in the machine's `rom` Region at its offset in the image,
/// `$FF` wherever nothing accounts for a byte, the program's start patched
/// into the six bytes at `$BFFA`, and the header. The layout is the contract
/// in docs/spec/car.md and the reasoning is
/// docs/decisions/0216-a-car-names-its-format-and-the-cold-start-is-an-edge.md.
///
/// Refused, with nothing emitted, when the board has banks — which is not
/// written yet — when a Section holding bytes stands where no part of the
/// image reaches, and when the entry Phase has no entry Label defined exactly
/// once among the Modules it needs.
CarFile emitCar( Patched const& build, diag::DiagnosticSink& sink );

} // namespace nga::model
