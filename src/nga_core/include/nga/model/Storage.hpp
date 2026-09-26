#pragma once

#include "nga/diag/DiagnosticSink.hpp"
#include "nga/model/Build.hpp"
#include "nga/model/Merge.hpp"
#include "nga/model/ReadOnly.hpp"
#include "nga/model/Size.hpp"
#include "nga/model/Types.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace nga::model
{

/// The result of PlaceStorage: which Sections have a Payload, where each one
/// waits, and where every Frame waits — the stored form of an edge, which the
/// routine reads from a Bank and which is never in runtime memory. See
/// docs/decisions/0036-frames.md.
///
/// Two questions answered apart, on purpose. Place needs the first — a Section
/// with a Payload cannot live in the Window — and never the second; and once
/// compression arrives a Payload's size is known only after Patch, so the
/// second answer moves behind Patch while the first stays where it is. See
/// docs/decisions/0017-payloads-and-banks.md.
class Storage
{
public:
  explicit Storage( std::span<Module const> modules );

  void markPayload( SectionRef where );

  /// The Phases the image is live in — see liveAcross — recorded by the
  /// first half, which knows the edges, and read by the second, which packs
  /// by it, and by the memory map.
  void live( SectionRef where, Residency live );
  [[nodiscard]] Residency const& liveOf( SectionRef where ) const;

  /// The Payload's stored form, which transform made it, and how far into its
  /// Section it lands once turned back. Recorded by the Transform Step, before
  /// the Banks are packed by what the forms came to.
  void store( SectionRef where, std::uint8_t transform, std::vector<std::uint8_t> form, std::uint32_t landing );

  void place( SectionRef where, StorageAddress at );

  [[nodiscard]] bool hasPayload( SectionRef where ) const;
  [[nodiscard]] bool isPlaced( SectionRef where ) const;
  [[nodiscard]] StorageAddress addressOf( SectionRef where ) const;

  /// The Payload's bytes **as they wait**, which is what the Container writes
  /// into a Bank and what a transform reads at run time. The Section's own
  /// bytes where the transform is `copy`, and something else otherwise — which
  /// is why this is answered here and not by Patch.
  [[nodiscard]] std::span<std::uint8_t const> formOf( SectionRef where ) const;

  /// How many bytes that comes to: the length of the stored form, and never
  /// the Section's size, which is what it occupies once loaded.
  [[nodiscard]] std::uint32_t sizeOf( SectionRef where ) const;

  /// Which transform turns the stored form back into the Section.
  [[nodiscard]] std::uint8_t transformOf( SectionRef where ) const;

  /// Where the transform's output lands, as an offset from the Section's
  /// runtime address: the start of its initialised extent, since reserved
  /// space in front of the first byte that emits is neither stored nor
  /// written. See docs/decisions/0026-a-payload-is-the-initialised-extent.md.
  [[nodiscard]] std::uint32_t landingOf( SectionRef where ) const;

  /// A Frame: the edge it belongs to, every Section the edge may have to
  /// load and every Cell it may have to write, from which its size follows
  /// before a byte of it exists — placed by that size, and written to
  /// exactly it later.
  ///
  /// `from` is absent for the **cold start**, the edge into the entry Phase
  /// from nowhere, which a Container with no loader takes to put the entry
  /// Phase's own bytes where they belong — see
  /// docs/decisions/0216-a-car-names-its-format-and-the-cold-start-is-an-edge.md.
  void declareFrame( FrameIndex index,
                     std::optional<PhaseIndex> from,
                     PhaseIndex to,
                     std::vector<SectionRef> payloads,
                     std::uint32_t cellWrites,
                     std::uint32_t windows );
  void placeFrame( FrameIndex index, StorageAddress at );
  void liveFrame( FrameIndex index, Residency live );
  [[nodiscard]] Residency const& frameLiveOf( FrameIndex index ) const;
  void storeFrame( FrameIndex index, std::vector<std::uint8_t> bytes );

  [[nodiscard]] std::uint32_t frameCount() const;
  [[nodiscard]] bool isFrameDeclared( FrameIndex index ) const;
  [[nodiscard]] bool isFramePlaced( FrameIndex index ) const;

  /// The Frame of an edge, when some `.transition` takes it.
  [[nodiscard]] std::optional<FrameIndex> frameOf( std::optional<PhaseIndex> from, PhaseIndex to ) const;

  [[nodiscard]] StorageAddress frameAddressOf( FrameIndex index ) const;
  [[nodiscard]] std::uint32_t frameSizeOf( FrameIndex index ) const;
  [[nodiscard]] std::span<std::uint8_t const> frameBytesOf( FrameIndex index ) const;
  /// Nothing for the cold start, which comes from no Phase.
  [[nodiscard]] std::optional<PhaseIndex> frameFrom( FrameIndex index ) const;
  [[nodiscard]] PhaseIndex frameTo( FrameIndex index ) const;
  [[nodiscard]] std::span<SectionRef const> framePayloadsOf( FrameIndex index ) const;
  [[nodiscard]] std::uint32_t frameCellWritesOf( FrameIndex index ) const;

  /// A Pane's Section with bytes: written by the Container into the Bank the
  /// Pane was given, at its offset within the Window, once, and never copied
  /// by an edge — see docs/decisions/0054-panes.md. Recorded where storage is
  /// placed, since that is when the Layout has said which Bank.
  struct PaneImage
  {
    SectionRef where{};
    StorageAddress at{};
  };

  void addPaneImage( SectionRef where, StorageAddress at );

  [[nodiscard]] std::span<PaneImage const> paneImages() const
  {
    return mPaneImages;
  }

private:
  struct Entry
  {
    bool payload = false;
    bool placed = false;
    StorageAddress address;
    std::uint8_t transform = 0;
    std::uint32_t landing = 0;
    std::vector<std::uint8_t> form;
    Residency live;
  };

  struct Frame
  {
    bool declared = false;
    bool placed = false;
    std::uint32_t size = 0;
    StorageAddress address;

    /// Absent for the cold start, which comes from no Phase.
    std::optional<PhaseIndex> from;
    PhaseIndex to;
    std::vector<SectionRef> payloads;
    std::uint32_t cellWrites = 0;
    std::vector<std::uint8_t> bytes;
    Residency live;
  };

  [[nodiscard]] Entry const& at( SectionRef where ) const;

  std::vector<std::vector<Entry>> mByModule;
  std::vector<Frame> mFrames;
  std::vector<PaneImage> mPaneImages;
};

/// The Phases across which bytes written once and never restored are still
/// needed: every Phase of `needed`, and every Phase from which one of them
/// is reachable. What a Pane's Section with bytes occupies, since the
/// Container writes it at load and no edge writes it again; and what an
/// image waits in storage for, `needed` being the Phases whose edges load
/// it. The glossary's **live**; see docs/decisions/0057-live.md.
Residency liveAcross( PhaseGraph const& graph, Residency const& needed );

/// The first half of the PlaceStorage Step: which Sections have a Payload,
/// and where every Frame waits.
///
/// A Section has a Payload iff it emits bytes and its Module is in the load
/// set of some Transition — needed in the Phase entered and not in the Phase
/// left — which is derived from the PhaseGraph and declared nowhere. An edge
/// some reachable `.transition` takes has a Frame, sized by what it may load
/// and write, and the Frames are placed here, first fit from the start of the
/// Banks: a `.transition` names where its Frames wait, so Patch has to know
/// before it writes a byte, and a Frame's size is known before any byte is.
///
/// Apart from the second half because it is the half **Place** needs, and it
/// needs nothing but the PhaseGraph to produce: a Section with a Payload may
/// not live in the Window, and that is decided before any size or address of
/// it exists. See docs/decisions/0017-payloads-and-banks.md.
///
/// A Section standing in ROM has no Payload and is no block of a Frame: its
/// bytes are in the address space already, and a Transition copying them would
/// be writing to ROM — see
/// docs/decisions/0215-a-cartridge-is-rom-and-a-section-stands-in-it-when-nothing-writes-it.md.
///
/// The one thing reported here is the complement of a Payload: a `root`
/// Section holding no bytes, evicted in a Phase from which a Phase needing it
/// is reachable. Nothing restores it, and what reaches a Root knows no
/// Phases — see docs/decisions/0040-root-evicted.md.
Storage findPayloads( Pruned const& build, ReadOnly const& readOnly, diag::DiagnosticSink& sink );

/// Whether the Project's Container takes a **cold start**: an edge into the
/// entry Phase from nowhere, which puts that Phase's own initialised bytes
/// where they belong. A `.xex` and an `.atr` have a loader for that and take
/// none; a cartridge loads nothing and takes one wherever it has a Phase to
/// enter — see
/// docs/decisions/0216-a-car-names-its-format-and-the-cold-start-is-an-edge.md.
[[nodiscard]] bool takesColdStart( Project const& project );

/// The second half: where each Payload waits, after the Frames.
///
/// Every Payload is given a Bank and an offset by first fit over storage end
/// to end in Project order, sized by the stored form the Transform Step
/// recorded, over the bytes free for it: not the Frames, not a Pane's
/// Section with bytes, not an image placed before it — all written at load,
/// as it is — and not a reservation in a Pane present in a Phase the image
/// is live in. A reservation the image outlives, or that is never present
/// while the image is needed, gives it its bytes; that is what frees a Bank
/// once a Phase can no longer be reached — see docs/decisions/0057-live.md.
/// A Payload for which no bytes are free is an error here, where the cause
/// is, rather than at Emit.
///
/// Fills the Storage that `findPayloads` produced rather than making one of its
/// own: the two answers belong to one result, and only the first of them is
/// ready before Place. It runs after Patch, because a stored form is made from
/// the bytes Patch produced — see docs/decisions/0022-transforms.md.
void placeStorage( Placed const& build, Storage& storage, diag::DiagnosticSink& sink );

} // namespace nga::model
