#pragma once

#include "nga/diag/DiagnosticSink.hpp"
#include "nga/model/Build.hpp"
#include "nga/model/Merge.hpp"
#include "nga/model/ReadOnly.hpp"
#include "nga/model/Size.hpp"
#include "nga/model/Types.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
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

  /// That some edge which must be quick opens this image, so it begins where a
  /// unit does and the driver never reads its way up to an offset. Set by the
  /// first half of PlaceStorage and read by the second — see
  /// docs/decisions/0221-an-edge-that-must-be-quick.md.
  void alignImage( SectionRef where );
  [[nodiscard]] bool isImageAligned( SectionRef where ) const;

  void alignFrame( FrameIndex index );
  [[nodiscard]] bool isFrameAligned( FrameIndex index ) const;

  /// The Phases the image is live in — see liveAcross — recorded by the
  /// first half, which knows the edges, and read by the second, which packs
  /// by it, and by the memory map.
  void live( SectionRef where, Residency live );
  [[nodiscard]] Residency const& liveOf( SectionRef where ) const;

  /// One of the stored forms a Payload comes to: its bytes, and how far into
  /// its Section they land once turned back. There is one of these unless no
  /// unit holds the whole — then the input is cut and each part encoded on its
  /// own, so that each is a form a decoder can read from its first byte. See
  /// docs/spec/cbm-disk.md for the medium that asks for it.
  struct StoredPiece
  {
    std::vector<std::uint8_t> form;
    std::uint32_t landing = 0;
  };

  /// The Payload's stored forms and which transform made them. Recorded by the
  /// Transform Step, before the Banks are packed by what the forms came to.
  void store( SectionRef where, std::uint8_t transform, std::vector<StoredPiece> pieces );

  void place( SectionRef where, std::uint32_t piece, StorageAddress at );

  [[nodiscard]] bool hasPayload( SectionRef where ) const;

  /// How many forms the Payload came to: one unless it had to be cut.
  [[nodiscard]] std::uint32_t pieceCountOf( SectionRef where ) const;

  [[nodiscard]] bool isPlaced( SectionRef where, std::uint32_t piece ) const;
  [[nodiscard]] StorageAddress addressOf( SectionRef where, std::uint32_t piece ) const;

  /// The Payload's bytes **as they wait**, which is what the Container writes
  /// into a Bank and what a transform reads at run time. The Section's own
  /// bytes where the transform is `copy`, and something else otherwise — which
  /// is why this is answered here and not by Patch.
  [[nodiscard]] std::span<std::uint8_t const> formOf( SectionRef where, std::uint32_t piece ) const;

  /// How many bytes that comes to: the length of the stored form, and never
  /// the Section's size, which is what it occupies once loaded.
  [[nodiscard]] std::uint32_t sizeOf( SectionRef where, std::uint32_t piece ) const;

  /// Which transform turns the stored form back into the Section.
  [[nodiscard]] std::uint8_t transformOf( SectionRef where ) const;

  /// Where the transform's output lands, as an offset from the Section's
  /// runtime address: the start of its initialised extent, since reserved
  /// space in front of the first byte that emits is neither stored nor
  /// written. See docs/decisions/0026-a-payload-is-the-initialised-extent.md.
  [[nodiscard]] std::uint32_t landingOf( SectionRef where, std::uint32_t piece ) const;

  /// A Frame: the edge it belongs to, every Section the edge may have to
  /// load and every Cell it may have to write, from which its size follows
  /// before a byte of it exists — placed by that size, and written to
  /// exactly it later.
  ///
  /// `from` is absent for the **cold start**, the edge into the entry Phase
  /// from nowhere, which a Container with no loader takes to put the entry
  /// Phase's own bytes where they belong — see
  /// docs/decisions/0216-a-car-names-its-format-and-the-cold-start-is-an-edge.md.
  /// `blocks` is what the Frame is **sized** for, which is not the number of
  /// Payloads where one of them may be cut into several: see
  /// blocksForPayload. The Frame's address is in the bytes of a
  /// `.transition`, so Patch has to know it before a form exists, and it is
  /// therefore sized here for the most it could come to.
  void declareFrame( FrameIndex index,
                     std::optional<PhaseIndex> from,
                     PhaseIndex to,
                     std::vector<SectionRef> payloads,
                     std::uint32_t blocks,
                     std::uint32_t cellWrites,
                     std::uint32_t headerBytes,
                     std::uint32_t unitBytes );
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

  /// A Section with bytes in a Pane pinned to a named state of the Window the
  /// driver's stream reads through: written by a `.xex` into that state, which
  /// the loader shows as it shows a unit, at the Section's offset within the
  /// Window — see docs/decisions/0256-a-named-state-is-filled-where-a-xex-can-show-it.md.
  /// No other Container fills one, so no other reads these.
  struct StateImage
  {
    SectionRef where{};
    std::uint32_t state = 0;
    std::uint32_t offset = 0;
  };

  void addStateImage( SectionRef where, std::uint32_t state, std::uint32_t offset );

  /// A Section a `.xex` without storage carries encoded and decodes in place
  /// while it loads: its stored form, how far past the end of the Section's
  /// initialised extent the form has to end, and the format — see
  /// docs/decisions/0235-a-xex-decodes-in-place-while-it-loads.md. Ordered by
  /// the address it decodes to, which is the order the loader decodes in.
  struct LoadForm
  {
    SectionRef where{};
    std::vector<std::uint8_t> form;
    std::uint32_t margin = 0;
    std::string format;

    /// Where the form is loaded, and where it decodes to.
    std::uint32_t at = 0;
    std::uint32_t to = 0;
  };

  void decodeAtLoad( LoadForm form );

  [[nodiscard]] std::span<LoadForm const> loadForms() const
  {
    return mLoadForms;
  }

  [[nodiscard]] bool isDecodedAtLoad( SectionRef where ) const;

  /// A Section whose bytes lie under a ROM the entry Phase has in, which a
  /// `.xex` writes by moving them there while it loads: the bytes, and where
  /// they go — see
  /// docs/decisions/0241-bytes-kept-for-a-later-phase-may-lie-hidden-under-a-rom.md.
  struct Hidden
  {
    SectionRef where{};
    std::uint32_t to = 0;
    std::vector<std::uint8_t> bytes;

    /// Where the Section is named in a `transform` and its form saves
    /// something: the stored form, and the format that decodes it.
    std::vector<std::uint8_t> form{};
    std::string format{};
  };

  /// One hidden Section decoded under its ROM while the program loads: its
  /// form, where it is written and where it decodes to.
  struct HiddenForm
  {
    std::uint32_t at = 0;
    std::uint32_t to = 0;
    std::vector<std::uint8_t> form;
    std::string format;
  };

  /// One batch of that move: what the `.xex` writes at `at` — a table, then
  /// the pieces it names — before `INITAD` at the move.
  struct HiddenBatch
  {
    std::uint32_t at = 0;
    std::vector<std::uint8_t> image;

    /// The Sections it carries a piece of.
    std::vector<SectionRef> holds{};

    /// Where in the image each piece stands, and which bytes of its Section it
    /// is: what a `.xex` built from a `choose` block changes a copy at, before
    /// the move takes it under the ROM.
    struct Piece
    {
      SectionRef where{};
      std::uint32_t offset = 0;
      std::uint32_t at = 0;
      std::uint32_t size = 0;
    };

    std::vector<Piece> pieces{};
  };

  void hideAtLoad( Hidden hidden );
  void stageHidden( HiddenBatch batch );
  void decodeHidden( HiddenForm form );

  [[nodiscard]] std::span<HiddenForm const> hiddenForms() const
  {
    return mHiddenForms;
  }

  [[nodiscard]] std::span<Hidden const> hiddenAtLoad() const
  {
    return mHidden;
  }

  [[nodiscard]] std::span<HiddenBatch const> hiddenBatches() const
  {
    return mHiddenBatches;
  }

  [[nodiscard]] bool isHiddenAtLoad( SectionRef where ) const;

  /// A byte the Choices of a `choose` block differ in: its Section, its offset
  /// in it, and what it is under each Choice, the first's first — see
  /// docs/decisions/0302-a-document-may-be-chosen-while-a-xex-loads.md.
  struct ChoiceSite
  {
    SectionRef where{};
    std::uint32_t offset = 0;
    std::vector<std::uint8_t> values;
  };

  void chooseAtLoad( std::vector<ChoiceSite> sites );

  [[nodiscard]] std::span<ChoiceSite const> choiceSites() const
  {
    return mChoiceSites;
  }

  /// Whether the cold start into `entry` brings this Section, which a
  /// Container then does not write at its address.
  [[nodiscard]] bool isLoadedByColdStart( SectionRef where, PhaseIndex entry ) const;

  [[nodiscard]] std::span<PaneImage const> paneImages() const
  {
    return mPaneImages;
  }

  [[nodiscard]] std::span<StateImage const> stateImages() const
  {
    return mStateImages;
  }

private:
  struct Piece
  {
    bool placed = false;
    StorageAddress address;
    std::uint32_t landing = 0;
    std::vector<std::uint8_t> form;
  };

  struct Entry
  {
    bool payload = false;
    bool aligned = false;
    std::uint8_t transform = 0;
    std::vector<Piece> pieces;
    Residency live;
  };

  struct Frame
  {
    bool declared = false;
    bool aligned = false;
    bool placed = false;
    std::uint32_t size = 0;
    StorageAddress address;

    /// Absent for the cold start, which comes from no Phase.
    std::optional<PhaseIndex> from;
    PhaseIndex to;
    std::vector<SectionRef> payloads;
    std::vector<std::uint8_t> bytes;
    Residency live;
  };

  [[nodiscard]] Entry const& at( SectionRef where ) const;

  std::vector<std::vector<Entry>> mByModule;
  std::vector<Frame> mFrames;
  std::vector<PaneImage> mPaneImages;
  std::vector<StateImage> mStateImages;
  std::vector<LoadForm> mLoadForms;
  std::vector<Hidden> mHidden;
  std::vector<HiddenBatch> mHiddenBatches;
  std::vector<HiddenForm> mHiddenForms;
  std::vector<ChoiceSite> mChoiceSites;
};

/// The Phases across which bytes written once and never restored are still
/// needed: every Phase of `needed`, and every Phase from which one of them
/// is reachable. What a Pane's Section with bytes occupies, since the
/// Container writes it at load and no edge writes it again; and what an
/// image waits in storage for, `needed` being the Phases whose edges load
/// it. The glossary's **live**; see docs/decisions/0057-live.md.
Residency liveAcross( PhaseGraph const& graph, Residency const& needed );

/// The Phases a Section's bytes are held apart across, given the Residency it
/// has: that Residency closed by liveAcross where nothing restores the bytes —
/// a Pane's Section with bytes, and in a program without storage every Section
/// with bytes, which the Container writes at load and no edge writes again —
/// and the Residency itself otherwise. The one place the choice is made: Place
/// claims by it, the Containers write the start by it and the verifier holds a
/// Layout to it — see docs/decisions/0057-live.md and
/// docs/decisions/0234-without-storage-bytes-stay-where-they-are-live.md.
Residency
heldAcross( PhaseGraph const& graph, Target const& target, Section const& section, Residency const& residency );

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
Storage findPayloads( Pruned const& build, ReadOnly const& readOnly, Sizes const& sizes, diag::DiagnosticSink& sink );

/// How many blocks a Payload may come to: one, unless no unit of storage holds
/// the whole of it and the Transform Step has to cut it. A Frame is sized for
/// this before the cutting happens — the address it waits at is in the bytes of
/// a `.transition`, so Patch has to know it before a form exists — and a Frame
/// sized for more blocks than are used leaves the rest unread.
std::uint32_t blocksForPayload( std::uint32_t storedCeiling, std::uint32_t extent );

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
