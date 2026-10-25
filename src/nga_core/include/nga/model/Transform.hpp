#pragma once

#include "nga/diag/DiagnosticSink.hpp"
#include "nga/diag/SourceManager.hpp"
#include "nga/model/Build.hpp"
#include "nga/model/Merge.hpp"
#include "nga/model/Patch.hpp"
#include "nga/model/Project.hpp"
#include "nga/model/Storage.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nga::model
{

/// Which transform turns a Payload where it waits into the Section at its
/// runtime address. Travels as the last byte of a table entry — see
/// docs/spec/transition.md.
using TransformId = std::uint8_t;

/// Plain copying is the first transform of the library rather than the absence
/// of one, so that the routine has no case for "not transformed".
constexpr TransformId TRANSFORM_COPY = 0;

/// One format the tool can encode: the name a Project and a `.transform`
/// write, and the host half — the bytes as they wait, from the bytes as they
/// run. Its inverse is a decoder some Module declares with `.transform`, and
/// nothing but running both says the two are inverses — see
/// docs/decisions/0022-transforms.md. `copy` is the identity.
struct Format
{
  std::string_view name;
  std::vector<std::uint8_t> ( *encode )( std::span<std::uint8_t const> );

  /// How far past the end of what a stored form decodes to the form has to end
  /// for its decoder to turn it back in place, reading forward while the output
  /// overtakes it; nothing where the form does not read to its end. See
  /// docs/decisions/0235-a-xex-decodes-in-place-while-it-loads.md.
  std::optional<std::uint32_t> ( *margin )( std::span<std::uint8_t const> );
};

[[nodiscard]] std::span<Format const> formats();

/// The format of that name, or null where the tool encodes none.
[[nodiscard]] Format const* formatNamed( std::string_view name );

/// The end of Assemble for a Project with Transitions: the dispatcher, a
/// Module of its own assembled from source once every decoder and the driver
/// are known — the driver's roles as `ngaOpen`, `ngaRead` and `ngaMap`, and
/// a compare and a `jmp` per number, in the order the
/// Project's `decoders` list — resident, outside the Window, and named in
/// every Phase's needs.
void addTransformDispatcher( diag::SourceManager& sources,
                             Project& project,
                             std::vector<Module>& modules,
                             diag::DiagnosticSink& sink );

/// The end of Assemble for a `.xex` without storage that has a Pane's Section
/// with bytes: the glue the Container fills the Pane's Bank through, which a
/// program with storage has in its dispatcher.
void addUnitsGlue( diag::SourceManager& sources,
                   Project& project,
                   std::vector<Module>& modules,
                   diag::DiagnosticSink& sink );

/// The end of Assemble for a driver that declares `init`: `ngaInit`, a Module
/// of its own holding that one macro, so that a Container has an address to
/// call before it uses the driver. A Module of its own and not the
/// dispatcher's, since a Project without Transitions has no dispatcher and
/// still needs the hardware reached. Resident, outside the Window, named in
/// every Phase's needs, and nothing at all where the role is not declared.
void addDriverInit( diag::SourceManager& sources,
                    Project& project,
                    std::vector<Module>& modules,
                    diag::DiagnosticSink& sink );

/// The stub a program that can return is started by, where the driver declares
/// `finalize` and the Container is one whose program returns — a `.xex` to its
/// DOS, a `.prg` to BASIC: `nga.run`, present in the entry Phase alone, and
/// `nga.finish`, resident, since the Phase the program returns from may be any.
/// See docs/decisions/0229-no-window-has-a-base.md.
void addRunStub( diag::SourceManager& sources,
                 Project& project,
                 std::vector<Module>& modules,
                 diag::DiagnosticSink& sink );

/// The end of Assemble for the decoders and the requests: numbers every
/// `.transform` the Modules declared — `copy` first and required where a
/// Transition exists, then Project and declaration order — holds a format to
/// one decoder and every decoder to a format the tool encodes, and resolves
/// every `transform` request of the Project onto the Section it applies to.
///
/// Here rather than where the document is read because a Section does not
/// exist until its Module is assembled, which is the same reason
/// `.transition NAME` resolves here.
void resolveTransforms( diag::SourceManager const& sources,
                        Project& project,
                        std::span<Module> modules,
                        diag::DiagnosticSink& sink );

/// The Transform Step: the stored form of every Payload.
///
/// Runs after Patch, because a Payload's stored form is made from the bytes
/// Patch produced, and before PlaceStorage, which packs the Banks by what
/// those forms came to. See docs/decisions/0022-transforms.md.
///
/// What is encoded is the Section's initialised extent and not the Section:
/// reserved space at either end is nobody's to carry. See
/// docs/decisions/0026-a-payload-is-the-initialised-extent.md.
void transformPayloads( Sized const& build, Storage& storage, Bytes const& bytes );

} // namespace nga::model
