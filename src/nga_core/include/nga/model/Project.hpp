#pragma once

#include "nga/diag/DiagnosticSink.hpp"
#include "nga/diag/SourceManager.hpp"
#include "nga/model/Board.hpp"
#include "nga/model/Indices.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nga::model
{

/// The set of Phases in which a Section must be present in memory.
///
/// Never declared directly: a Module's Residency is the set of Phases naming
/// it, and its Sections inherit it, so no two places can disagree about it.
/// See docs/decisions/0016-phases-and-residency.md.
class Residency
{
public:
  Residency() = default;

  explicit Residency( std::size_t phaseCount ) : mPhases( phaseCount, false ) {}

  /// Every Phase: what `resident` declares, and what every Module has in a
  /// Project with one Phase.
  static Residency all( std::size_t phaseCount )
  {
    Residency result;
    result.mPhases.assign( phaseCount, true );
    return result;
  }

  void add( PhaseIndex phase )
  {
    mPhases[phase.value] = true;
  }

  /// Whether `phase` is one of these. A Residency shorter than the Phases —
  /// one of none, of a Module the tool built before the Phases were counted
  /// — includes none past its end.
  [[nodiscard]] bool includes( PhaseIndex phase ) const
  {
    return phase.value < mPhases.size() && mPhases[phase.value];
  }

  /// True when the two share a Phase: the first half of the co-visibility
  /// rule, and the whole of it until Views exist.
  [[nodiscard]] bool intersects( Residency const& other ) const;

  [[nodiscard]] bool isEmpty() const;

  /// Every Phase: what the glossary calls Resident, and what
  /// `section.resident` reports in source.
  [[nodiscard]] bool isAll() const;

  [[nodiscard]] std::size_t phaseCount() const
  {
    return mPhases.size();
  }

  friend bool operator==( Residency const&, Residency const& ) = default;

private:
  std::vector<bool> mPhases;
};

/// A named state of program execution: the Modules it needs in memory, and
/// the Phases a Transition may lead to. A Transition is an edge and nothing
/// more; what crosses one is not settled.
/// `without MODULE` in a Phase: a ROM the driver switches out, which the
/// Phase does without although `resident` lists it, and where it said so —
/// see docs/decisions/0229-no-window-has-a-base.md.
struct PhaseWithout
{
  ModuleIndex module;
  diag::SourceSpan span;
};

struct Phase
{
  /// Absent for the one Phase of a Project that declares none.
  std::optional<std::string> name;

  /// What a finding, a map and a listing call it: its name, or `(implicit)`
  /// for that one Phase, which no name can be spelled as.
  [[nodiscard]] std::string displayName() const
  {
    return name.value_or( "(implicit)" );
  }

  std::vector<ModuleIndex> needs;
  std::vector<PhaseIndex> then;

  /// The Label the Phase starts at; absent means one named `entry`. Resolved
  /// against the Modules the Phase needs when a Container asks — see
  /// docs/decisions/0018-xex-container.md.
  std::optional<std::string> entry;

  /// Where `entry` wrote the Label's name, which a reader renaming the Label
  /// renames there too — see docs/spec/facts.md.
  std::optional<diag::SourceSpan> entrySpan{};

  /// What the Phase does without, taken out of its needs once `resident` has
  /// put everything in; held to the driver's `rom` lines once the driver is
  /// resolved.
  std::vector<PhaseWithout> without{};

  /// Where `final` was said: nothing leaves the Phase, by an edge or by the
  /// program returning, so what only an exit runs is not kept in it — see
  /// docs/decisions/0245-a-final-phase-keeps-nothing-only-an-exit-runs.md.
  std::optional<diag::SourceSpan> finalAt{};
};

/// Phases, Transitions and the entry Phase. May contain cycles.
struct PhaseGraph
{
  std::vector<Phase> phases;
  PhaseIndex entry;
};

/// One past the last address the CPU can name, and one past the zero page.
constexpr std::uint32_t ADDRESS_SPACE_END = 0x10000;
constexpr std::uint32_t ZERO_PAGE_END = 0x100;

/// What a Region says about its addresses, in the order a more restrictive
/// one wins where two overlap: a Project declaring `reserved` over the
/// variant's `ram` narrows the pool, and nothing a Project declares turns a
/// register into RAM.
///
/// `ROM` is memory the solver places in and no code writes — a cartridge, and
/// what makes it a property of the Region rather than of the Container is that
/// it is true in every state of the hardware. It sits between `RAM` and
/// `RESERVED` because a Project may narrow its machine's ROM to a range nothing
/// may use, and nothing a Project writes turns ROM into RAM. See
/// docs/decisions/0215-a-cartridge-is-rom-and-a-section-stands-in-it-when-nothing-writes-it.md.
enum class RegionProperty : std::uint8_t
{
  RAM,
  ROM,
  RESERVED,
  REGISTER,
};

std::string_view nameOf( RegionProperty property );

/// The property a Region's word names, if it names one.
std::optional<RegionProperty> regionPropertyNamed( std::string_view word );

/// A contiguous address range with one property — see the glossary. A truth
/// about addresses in every state of the hardware: what occupies memory for
/// a while is a Module with a Residency, never a Region. A named one defines
/// a Symbol of kind `Region`, visible in every Module, whose value is its
/// start address — a `register` entry is a Region of one or two bytes.
struct Region
{
  /// Where the name was written, which is what the Symbol's text is a view
  /// into; absent for a Region that only describes addresses.
  std::optional<diag::SourceSpan> nameSpan;
  std::string name;
  AddressRange range;
  RegionProperty property = RegionProperty::RAM;

  /// The entry that declared it; a finding about the Region points here.
  /// Absent for the stand-in, which no document wrote.
  std::optional<diag::SourceSpan> site;
};

/// What a diagnostic calls a Region: its name, or its range where it has none.
std::string displayNameOf( Region const& region );

/// Where the solver may allocate from: the Regions with every more restrictive
/// Region cut out. The `ram` of them in two lists — below `$100` and above —
/// because a PlacementClass names one of them, and the `rom` of them in a
/// third, which a read-only Section is placed in. Derived from the Regions when
/// the Project is read, and what Place, the layout verifier and the map read
/// instead of the Regions themselves.
///
/// The ROM pool has **no zero-page half**: the class exists so that an
/// instruction naming a variable can be a byte shorter, and a Section that is
/// never written gains nothing from the scarcest memory the machine has, so
/// `zeropage` and `rom` together are refused. A `rom` Region below `$100` is
/// therefore in no pool, as a Window's ranges are.
struct Pools
{
  /// Sorted and disjoint.
  std::vector<AddressRange> zeroPage;
  std::vector<AddressRange> general;
  std::vector<AddressRange> readOnly;

  [[nodiscard]] std::uint32_t zeroPageSize() const;
  [[nodiscard]] std::uint32_t generalSize() const;
  [[nodiscard]] std::uint32_t readOnlySize() const;
};

/// The pools the Regions leave: every address that some `ram` Region covers and
/// no more restrictive Region does, and, above `$100`, the same of `rom` — less
/// `windows`, the ranges of every Window, in which only a Pane or storage stands
/// — see docs/decisions/0229-no-window-has-a-base.md.
Pools poolsOf( std::span<Region const> regions, std::span<AddressRange const> windows = {} );

/// The Regions of a Target no document described. Only what stands in for
/// what an OS and a DOS in memory cost until a variant says so: `$80-$FF`,
/// the program's half of the zero page while BASIC is out, and `$2000` up,
/// where the DOSes in common use have finished. The stack page is the one
/// hardware truth among them and is left out like the rest below `$2000`.
std::vector<Region> standInRegions();

/// Where an image waits: the unit its first byte is in, and the offset
/// there. An image may run on into the units after — see Target.
struct StorageAddress
{
  BankIndex bank;
  std::uint32_t offset = 0;

  friend bool operator==( StorageAddress, StorageAddress ) = default;
};

/// A named set of Banks, declared by the variant with `units NAME N`: how
/// many there are, and where they lie in the medium's own memory where the
/// hardware addresses it as one. What the hardware calls each is the
/// driver's, and the model names one by its index. A Window shows a set as
/// one state per Bank, and `storage` names the set its units are — see
/// docs/decisions/0053-a-window-names-its-units.md. The name is a Symbol in
/// scope in every Module, an `Integer` whose value is the count.
struct UnitSet
{
  diag::SourceSpan nameSpan{};
  std::string name{};
  std::uint32_t count = 0;
  diag::SourceSpan site{};

  /// Declared `linear BEGIN .. END`: the medium's own addresses, which a
  /// blitter or a video chip reads as one memory — Bank 0 starts at `begin`
  /// and each next one a Window's size further on. Absent where nothing but
  /// the CPU reads the set, through a Window. See
  /// docs/decisions/0251-a-bank-has-a-linear-address.md.
  std::optional<AddressRange> linear{};
};

/// One entry of a Window's `views`: a named state, or a unit set that
/// contributes one state per Bank.
struct WindowState
{
  diag::SourceSpan nameSpan{};
  std::string name{};
  std::optional<UnitSetIndex> units{};

  /// How many of the Window's state indices this takes: a set's Banks, or
  /// one where it is named.
  [[nodiscard]] std::uint32_t indices( std::span<UnitSet const> sets ) const
  {
    return units.has_value() ? sets[units->value].count : 1;
  }
};

/// A named set of address ranges the hardware switches as one, the states it
/// can show, and which named state holds when nothing has switched — see the
/// glossary and docs/decisions/0052-a-view-is-a-state-of-a-window.md. The
/// states are indexed from zero in the order written, a set's Banks in their
/// order, so that a state is named in a byte wherever the driver is handed
/// one. Declared by the variant and naming no register: what it takes to
/// show a state is the driver's — see
/// docs/decisions/0053-a-window-names-its-units.md.
struct Window
{
  diag::SourceSpan nameSpan{};
  std::string name{};

  /// Sorted as written; need not be contiguous.
  std::vector<AddressRange> ranges{};
  std::vector<WindowState> states{};
  diag::SourceSpan site{};

  /// What the ranges hold together, which is what one state of it is.
  [[nodiscard]] std::uint32_t size() const;

  /// Whether some range covers the address.
  [[nodiscard]] bool covers( std::uint32_t address ) const;

  /// Whether some range meets the half-open one given.
  [[nodiscard]] bool meets( AddressRange other ) const;

  /// The index of the first state a unit set contributes, or nothing when the
  /// Window does not show it. Needs the sets, since a set's count is theirs.
  [[nodiscard]] std::optional<std::uint32_t> firstStateOf( UnitSetIndex set, std::span<UnitSet const> sets ) const;

  /// How many states the Window shows: one per named state and one per Bank
  /// of every unit set among them.
  [[nodiscard]] std::uint64_t stateCount( std::span<UnitSet const> sets ) const;

  /// The index of the named state called `name`, or nothing: a unit set's
  /// name is not a state.
  [[nodiscard]] std::optional<std::uint32_t> namedStateOf( std::string_view wanted,
                                                           std::span<UnitSet const> sets ) const;
};

/// A named set of Sections that one switch shows together — see the glossary
/// and docs/decisions/0054-panes.md. Declared in the Project's `panes` block
/// under a Window; a Section names it with `in`; the solver gives it one
/// state of that Window for the whole run — a Bank of the set the Window
/// shows, or the named state the block pinned it to. A family is `count`
/// Panes of one layout on consecutive Banks, and its Sections stand in every
/// member at one address.
struct Pane
{
  diag::SourceSpan nameSpan{};
  std::string name{};
  WindowIndex window{};

  /// Where the `panes` block named the Window, a use of the Window's name.
  diag::SourceSpan windowSpan{};

  /// The named state the block pinned it to, as an index among the Window's
  /// states; absent for a Pane the solver gives a Bank.
  std::optional<std::uint32_t> state{};
  std::uint32_t count = 1;
  diag::SourceSpan site{};
};

/// The platform: what the hardware is, as opposed to what the program wants
/// of it.
///
/// Its Regions, unit sets and Windows, from `target` blocks a machine
/// variant and the Project supply, and its storage — the units and their
/// size — from a `storage` block. With no `target` block the Regions are the
/// stand-in; with no `storage` block there is no storage, and a Section that
/// a Transition loads is an error at PlaceStorage, which is the signal that
/// a Project file with a Target is what the program needs.
/// What a program is optimised for where one choice costs speed against size
/// — see docs/decisions/0177-intent.md. `FIT` is what a Project saying nothing
/// is, and means *as much speed as the tool can promise to give back*: a
/// choice it cannot undo is one it does not take.
enum class Intent : std::uint8_t
{
  SPEED,
  SIZE,
  FIT,
};

/// `speed`, `size` and `fit`, and nothing where the word names none of them.
std::optional<Intent> intentNamed( std::string_view word );

/// The word an Intent is written as.
std::string_view nameOf( Intent intent );

/// The processor a Target's machine has. Hardware truth, like where its
/// registers are: what the program may be written in, not what it would
/// rather be — see docs/decisions/0182-the-target-names-its-processor.md.
enum class Cpu : std::uint8_t
{
  MOS6502,
  WDC65SC02,
};

/// `6502` and `65sc02`, and nothing where the word names neither.
std::optional<Cpu> cpuNamed( std::string_view word );

/// The word a processor is written as.
std::string_view nameOf( Cpu cpu );

/// The output file format. Not merely a packaging: it constrains Place, so
/// the Project names the one it is and the Variant names the ones the machine
/// takes — see docs/decisions/0179-a-container-is-chosen-in-the-project.md.
enum class Container : std::uint8_t
{
  RAW_IMAGE,
  XEX,
  ATR,
  CAR,
  BS93,
  LNX,
  LYX,
  PRG,
  D64,
  D80,
  D82,
};

/// `raw`, `xex`, `atr`, `car`, `bs93`, `lnx`, `lyx`, `prg`, `d64`, `d80` and
/// `d82`, and nothing where the word names none of them. `car` and the two Lynx cartridges name a board
/// beside them — see Car.hpp and Lnx.hpp.
std::optional<Container> containerNamed( std::string_view word );

/// The word a Container is written as, which is what a finding lists.
std::string_view nameOf( Container container );

/// Every Container, in the order a finding lists them.
std::span<Container const> containers();

/// A piece of hardware that holds one Section at a time: a display list's
/// pointer, an interrupt's vector. What it holds stays where it is for as
/// long as it may hold it — see
/// docs/decisions/0247-the-hardware-holds-a-section-for-as-long-as-it-is-given-it.md.
struct Holder
{
  std::string name;
  diag::SourceSpan site;
};

struct Target
{
  /// In declaration order, the stand-in until a document declares one.
  std::vector<Region> regions = standInRegions();

  /// In declaration order, one per name however often it is declared.
  std::vector<Holder> holders{};

  /// The Holder of that name.
  [[nodiscard]] std::optional<std::uint32_t> holderNamed( std::string_view name ) const
  {
    for ( std::uint32_t index = 0; index < holders.size(); ++index )
    {
      if ( holders[index].name == name )
      {
        return index;
      }
    }
    return std::nullopt;
  }

  /// Derived from the Regions — see poolsOf — and held here so that a Claim
  /// is built from a list rather than from a walk over every Region.
  Pools pools = poolsOf( regions );

  /// In declaration order.
  std::vector<UnitSet> unitSets{};
  std::vector<Window> windows{};
  std::vector<Pane> panes{};

  /// The units of storage: the Banks of `storageUnits` where `storage` names
  /// a set, and then `unitSize` is the size of the Window that shows it; or
  /// `unitCount` units of `unitSize` known by their numbers, a disk's
  /// sectors. An image may run from one unit into the next: the driver's
  /// stream carries on, so the space is `unitCount * unitSize` bytes end to
  /// end.
  std::optional<UnitSetIndex> storageUnits{};
  std::uint32_t unitCount = 0;
  std::uint32_t unitSize = 0;

  /// The processor, which decides what instructions a Module of this Project
  /// may be written in. A Target that says nothing is a 6502, which every
  /// machine of the family has.
  Cpu cpu = Cpu::MOS6502;

  /// Where `cpu` was said, so a refusal can point at it.
  diag::SourceSpan cpuSite{};

  /// What the machine takes, in declaration order, and empty where no
  /// document said — hardware truth, like where the registers are. A `target`
  /// block written inline by a regression case is a stand-in and not a
  /// machine, so where this is empty nothing is checked and the Project's word
  /// stands — see docs/decisions/0179-a-container-is-chosen-in-the-project.md.
  std::vector<Container> containers{};

  /// Where the first `containers` entry stood, so a refusal can point at it.
  diag::SourceSpan containersSite{};

  /// The ranges of the Window the driver's stream reads through — declared
  /// by the driver with `.driver stream` and set here at the end of Assemble;
  /// empty for a driver that reads through none. Place keeps every Section
  /// with a Payload, every Cell, the driver and every decoder out of them,
  /// since the stream shows a Bank there while it is open.
  std::vector<AddressRange> streamRanges{};

  /// The unit set that Window shows, and how big one of its units is, set
  /// with `streamRanges`: what a Container fills a Pane's Bank through where
  /// there is no storage to say it — see filledSet.
  std::optional<UnitSetIndex> streamUnits{};
  std::uint32_t streamUnitSize = 0;

  /// How much of each unit a program may use, indexed by the unit, where a
  /// unit is not usable to its end. Empty where every one is, which is every
  /// medium but a diskette whose tracks hold different numbers of sectors:
  /// there `unitSize` is the **widest** track and so a stride and not a size,
  /// and what a narrower track does not have is held against every image. What
  /// makes that safe is that nothing crosses a unit — see `span` in
  /// docs/spec/transition.md — so no image ever runs through the gap.
  std::vector<std::uint32_t> unitUsable{};

  /// What one unit holds: the stride, which is what a position is divided by.
  [[nodiscard]] std::uint32_t bankSize() const
  {
    return unitSize;
  }

  /// How much of unit `unit` a program may use, which is all of it unless the
  /// units are unequal.
  [[nodiscard]] std::uint32_t usableInUnit( std::uint32_t unit ) const
  {
    if ( unitUsable.empty() )
    {
      return unitSize;
    }
    return unit < unitUsable.size() ? unitUsable[unit] : 0;
  }

  /// The most of one unit a program may use, over every unit: what one image
  /// is held to where images stay within a unit. Zero without storage.
  [[nodiscard]] std::uint32_t widestUsableUnit() const;

  /// The whole of storage, end to end.
  [[nodiscard]] std::uint32_t storageSize() const
  {
    return unitCount * unitSize;
  }

  /// Whether the Project, with what it includes, declared any storage at all.
  /// Without it an edge has nowhere for a Frame to wait, so it loads nothing
  /// and is a jump to a stub of the Phase it enters — see
  /// docs/decisions/0234-without-storage-bytes-stay-where-they-are-live.md.
  [[nodiscard]] bool hasStorage() const
  {
    return unitCount > 0;
  }

  /// How many bytes a unit's number takes wherever the tool writes one — in a
  /// Frame, in a `.transition`'s table, in the driver's cell: one while the
  /// units fit a byte and two once they do not. The variant decides it, since
  /// the routine is written before any driver is read, and the driver's cell is
  /// held to it — see
  /// docs/decisions/0228-a-driver-names-the-cells-its-stream-stands-in.md.
  [[nodiscard]] std::uint32_t unitBytes() const
  {
    return unitCount > 256 ? 2 : 1;
  }

  /// The unit set or the Window of a name, or nothing.
  [[nodiscard]] std::optional<UnitSetIndex> unitSetNamed( std::string_view name ) const;
  [[nodiscard]] std::optional<WindowIndex> windowNamed( std::string_view name ) const;
  [[nodiscard]] std::optional<PaneIndex> paneNamed( std::string_view name ) const;

  /// The unit set a Window shows, when it shows one: the first among its
  /// states, since a Window showing two sets is not one this tool has met.
  [[nodiscard]] std::optional<UnitSetIndex> unitSetShownBy( WindowIndex window ) const;

  /// What one Bank of a set holds: the size of the Windows that show it, or
  /// nothing where no Window does.
  [[nodiscard]] std::optional<std::uint32_t> bankSizeOf( UnitSetIndex set ) const;

  /// Where a Section of a Pane stands in the medium's own addresses, given
  /// the state Place gave the Pane and the Section's address in the Window:
  /// the set's `linear` start, a Bank's size for every Bank before the
  /// Pane's, and the Section's distance into the Window's ranges laid end to
  /// end. Nothing for a Pane pinned to a named state or one whose set
  /// declares no `linear` — see
  /// docs/decisions/0251-a-bank-has-a-linear-address.md.
  [[nodiscard]] std::optional<std::uint32_t>
  linearAddressOf( PaneIndex pane, std::uint32_t state, std::uint32_t address ) const;

  /// Where a set's Banks lie in the medium end to end: from its `linear`
  /// start, a Bank's size each, or the whole declared range where no Window
  /// shows the set to say how large a Bank is. Nothing for a set that
  /// declares no `linear` — see
  /// docs/decisions/0254-a-section-may-stand-in-a-unit-set.md.
  [[nodiscard]] std::optional<AddressRange> linearBanksOf( UnitSetIndex set ) const;

  /// Where a position in storage, counted end to end, falls: the unit and
  /// the offset in it. What a StorageAddress names is the start of an
  /// image, and the image may run on into the units after.
  [[nodiscard]] StorageAddress addressAt( std::uint32_t position ) const;

  /// The position, end to end, of a StorageAddress.
  [[nodiscard]] std::uint32_t positionOf( StorageAddress at ) const;

  /// The units a Container fills at load, which is where a Pane's Bank is:
  /// storage's where there is storage, and otherwise the set the driver's
  /// stream reads through, which a program without storage still fills its
  /// Panes through — see
  /// docs/decisions/0234-without-storage-bytes-stay-where-they-are-live.md.
  /// The set, how many units it has and how big one is; and the position and
  /// address of an image in them, as positionOf and addressAt are for
  /// storage.
  [[nodiscard]] std::optional<UnitSetIndex> filledSet() const;
  [[nodiscard]] std::uint32_t filledCount() const;
  [[nodiscard]] std::uint32_t filledUnitSize() const;
  [[nodiscard]] StorageAddress filledAddressAt( std::uint32_t position ) const;
  [[nodiscard]] std::uint32_t filledPositionOf( StorageAddress at ) const;

  /// The most restrictive Region any address of `range` lies in, or nothing
  /// when no Region covers any of it. What a pin is held against: a pin in a
  /// register is refused, a pin in a reserved Region is a warning, and a pin
  /// in `ram` or in nothing at all is the author's Constraint and is silent.
  [[nodiscard]] std::optional<RegionIndex> mostRestrictiveOver( AddressRange range ) const;
};

/// What a Module the tool added holds, where it is not `.asm` to assemble.
enum class Generated : std::uint8_t
{
  NONE,
  TRANSITION_CELL,
  SLOT_CELLS,
  ENTRY_STUB,
  REGIONS,
  CONSTANTS,
};

/// One entry of a `constants` block: a name the Project gives a value, and
/// the number it gave. The value is a **literal** and not an expression,
/// because this grammar has no terminator to end one with — see
/// docs/decisions/0178-a-configuration-is-a-document.md. A literal may carry
/// a `-` as its sign, kept apart from the digits it negates.
struct ProjectConstant
{
  diag::SourceSpan nameSpan;
  std::optional<diag::SourceSpan> signSpan;
  diag::SourceSpan valueSpan;
};

/// A `register` a Choice declares: its name, the range it gives it, and where.
struct ChoiceRegister
{
  std::string name;
  AddressRange range;
  diag::SourceSpan span;
};

/// One document of a `choose` block: the values it gives the names every
/// Choice declares. The first is the one the program is built as, and the
/// others are what the program may be changed into while a `.xex` loads —
/// see docs/decisions/0302-a-document-may-be-chosen-while-a-xex-loads.md.
struct Choice
{
  /// The path as the `choose` block wrote it, and where it wrote it.
  std::string document;
  diag::SourceSpan site;
  std::vector<ChoiceRegister> registers;
  std::vector<ProjectConstant> constants;
};

/// One entry of the Project's module list.
struct ProjectModule
{
  std::string name;
  diag::FileId file;
  Residency residency;

  /// The Target's own Modules are built rather than assembled: the Cell, the
  /// Slot Cells, or the Module whose Symbols are the named Regions. The
  /// routine is `.asm` like any other and is NONE.
  Generated generated = Generated::NONE;

  /// Runs while a Bank is switched in, so it may not live in the Window: the
  /// Constraint a Section with a Payload has, put on a whole Module.
  bool outsideWindow = false;

  /// Written by a Generator rather than read from a file, which is what
  /// `--emit-asm` writes out — see docs/spec/generators.md. Assembled like any
  /// other Module, so it is not a kind of `Generated`.
  bool fromGenerator = false;

  /// The path as the entry wrote it — in `modules`, or on the command line —
  /// and empty for a Module no path names. What a `.ngc` Module's compiled
  /// text names in its `.source` marks, so that the text is the same wherever
  /// the tool runs, as a generator's is: see docs/spec/generators.md.
  std::string path{};

  /// The text a `.ngc` Module compiled to, registered by Assemble once it has
  /// compiled it, and nothing before that or when the file held an error.
  /// `file` stays the `.ngc`, which the compiler's own findings point into;
  /// this is what `--emit-asm` writes and the suite holds to a golden.
  std::optional<diag::FileId> compiledText{};
};

/// What the Project said about transforming a Payload: which transform, and
/// the Section it applies to.
///
/// The Module is resolved when the document is read; the Section cannot be,
/// because a Section does not exist until its Module is assembled. Its name
/// therefore travels as text and is resolved at the end of Assemble, which is
/// where `.transition NAME` resolves for the same reason.
struct TransformRequest
{
  std::string transform;
  ModuleIndex module;
  std::string section;
  diag::SourceSpan span;

  /// Where the format is named, which a finding about the format stands at.
  diag::SourceSpan formatSpan;
};

/// A decoder the program declared with `.transform`, resolved and numbered:
/// the format it decodes, the host half that encodes it, and the Label the
/// dispatcher jumps to. Its position in the Project's list is the number a
/// block of a Frame carries — see docs/spec/transition.md.
struct Decoder
{
  std::string format;
  std::vector<std::uint8_t> ( *encode )( std::span<std::uint8_t const> ) = nullptr;
  ModuleIndex module;
  std::uint32_t symbol = 0;
};

/// What the driver provides for one Window: `show` a state named where the
/// use is written, and `showAt` one whose index is in `X` — and, where its
/// declaration ends with the place `xy`, whose high byte is in `Y`.
struct WindowRoles
{
  MacroIndex show;
  MacroIndex showAt;

  /// 2 where `showAt` was declared with the place `xy`, 1 for `x` or none.
  std::uint32_t stateBytes = 1;
};

/// A ROM the driver switches out, declared `.driver rom MODULE [ALSO]`: the
/// Module that is the ROM, and the Module whose ranges showing it shows too —
/// on an XL the floating-point pack and the OS share one bit. Its position
/// among the driver's `rom` lines is its bit in the set `select` is handed.
/// A ROM whose Module the Project does not list is one no Phase needs, and
/// its bit is never set: BASIC, which a program that wants it lists itself —
/// see docs/decisions/0229-no-window-has-a-base.md.
struct RomRole
{
  std::optional<ModuleIndex> module{};
  std::optional<ModuleIndex> also{};
};

/// The storage driver, resolved: the Module that declared it, the macro of
/// that Module each role expands to — under `nga.open`, `nga.read`,
/// `nga.show` and `nga.showAt` wherever the routine, the glue, a decoder or
/// a `.with` uses them, bound from the declaration and never by name, so the
/// driver exports nothing — and the Window its stream reads through. See
/// docs/spec/transition.md and docs/decisions/0053-a-window-names-its-units.md.
struct Driver
{
  ModuleIndex module;

  /// Whether the driver has a stream: `open`, `read` and the two cells. Every
  /// driver of storage has one; a driver in a program without storage
  /// declares none of them, and then the four below mean nothing — see
  /// docs/decisions/0242-a-driver-may-switch-roms-and-have-no-stream.md. Such
  /// a driver may still show a Window, which every `.with` expands — see
  /// docs/decisions/0253-a-driver-that-shows-a-window-stays.md.
  bool streams = true;
  MacroIndex open;
  MacroIndex read;

  /// The cells the stream stands in, declared `.driver unit` and `.driver
  /// offset`: Symbols of the driver's Module, by position in its table. The
  /// routine writes the position into them under `nga.unit` and `nga.offset`
  /// and then expands `open`, which takes nothing in registers — see
  /// docs/decisions/0228-a-driver-names-the-cells-its-stream-stands-in.md.
  std::uint32_t unit = 0;
  std::uint32_t offset = 0;

  /// Declared `.driver init`: what the hardware takes before the driver can
  /// be used at all, which the Container calls once before it uses the driver
  /// and before the entry. Absent for the hardware that takes nothing, which
  /// is every medium this tool shipped a driver for before MariaCEL — and
  /// where it is absent nothing is emitted and no Container segment is
  /// written. See docs/decisions/0227-a-driver-may-need-the-hardware-reached-first.md.
  std::optional<MacroIndex> init{};

  /// Declared `.driver finalize`: what leaves the machine as the program found
  /// it, called where the program ends — after its entry returns, through the
  /// stub a Container that can be returned from starts it by, and wherever the
  /// program writes `nga.finalize`. Absent where there is nothing to put back,
  /// and then `nga.finalize` is nothing at all — see
  /// docs/decisions/0229-no-window-has-a-base.md.
  std::optional<MacroIndex> finalize{};

  /// The ROMs the driver switches out, in the order it declared them, and the
  /// macro that switches them: `select`, handed the set a Phase needs in `A`,
  /// by the routine before it loads a byte of the Phase and at the start of
  /// the program. Both empty where the driver switches nothing.
  std::vector<RomRole> roms{};
  std::optional<MacroIndex> select{};

  /// Declared `.driver mask` and `.driver unmask`: what keeps the machine's
  /// interrupts away while the load has every ROM out, and what lets them in
  /// again. The program is not running yet, so they are the tool's to mind,
  /// and how is the machine's. Absent where the driver says nothing, and then
  /// nothing is hidden under a ROM — see
  /// docs/decisions/0241-bytes-kept-for-a-later-phase-may-lie-hidden-under-a-rom.md.
  std::optional<MacroIndex> mask{};
  std::optional<MacroIndex> unmask{};

  /// One entry per Window of the Target, in the Target's order.
  std::vector<WindowRoles> windows{};

  /// Absent for a driver whose medium has no Window.
  std::optional<WindowIndex> stream{};

  /// Declared `.driver seek forward`: the medium reaches an offset by reading
  /// up to it, so an image an edge opens is worth beginning where a unit does.
  /// Silence means an offset costs the driver nothing, which is true of a
  /// sector in a buffer and of a Bank in a Window — see
  /// docs/decisions/0221-an-edge-that-must-be-quick.md.
  bool seeksForward = false;

  /// Declared `.driver span none`: no image and no Frame crosses from one unit
  /// into the next, so the driver needs no way of carrying there and may know
  /// nothing of how the units follow one another. Silence means the driver
  /// carries, which every driver this tool ships does — a Bank number and a
  /// page number are one more than the last, and a sector is the next sector.
  /// It is a word about the medium and not a role, as `seek` is; what pays for
  /// it is the placement, which must then refuse an image no single unit holds.
  bool spansNothing = false;
};

/// A group as written: its name and what it lists, Modules and groups alike.
/// For the facts alone — no Step reads it, since nothing below the Project
/// learns a group exists, docs/decisions/0042-groups.md.
struct ProjectGroup
{
  std::string name;
  std::vector<std::string> members;
};

/// A Label a `load` block names with `init`, and where it was named.
struct LoadInit
{
  std::string name;
  diag::SourceSpan span;
};

/// The description of the program a run works on.
///
/// A run always has one. What is not here yet — the rest of the Target,
/// Roots — is absent rather than defaulted, and adding it is what a Project
/// file is for.
struct Project
{
  std::vector<ProjectModule> modules;
  PhaseGraph phases;
  Target target;

  /// What a `load` block names: the memory the machine holds while a `.xex`
  /// loads, as Modules of pinned Sections with no bytes, and where the first
  /// block opened — see
  /// docs/decisions/0246-the-load-of-a-xex-needs-what-the-machine-holds-then.md.
  std::vector<ModuleIndex> loadNeeds;

  /// The groups the Project declared, and the directory of the shipped
  /// library its paths were looked for in: for the facts, which an editor
  /// completes a `.ngp` from — see docs/spec/facts.md.
  std::vector<ProjectGroup> groups{};
  std::string library{};

  std::optional<diag::SourceSpan> loadSite{};

  /// The Labels the `load` block names with `init`, in order: what the `.xex`
  /// calls through `INITAD` before it writes anything else.
  std::vector<LoadInit> loadInits;

  /// Declared `frames held`: an edge's block descriptors are read **once**
  /// into the memory of the block that loads last, and the Frame is then
  /// opened twice an edge instead of once a block. What it costs is the
  /// routine's second way of fetching them, some 138 bytes of it and two of
  /// zero page, which every Phase carries — so it is the Project's to ask for
  /// and not the tool's to decide: it pays where an `open` is a disk operation
  /// or a page shifted in, and buys nothing at all where one is fifty cycles.
  /// See docs/spec/project-file.md and
  /// docs/decisions/0226-an-edge-holds-its-descriptors-in-the-block-that-loads-last.md.
  bool framesHeld = false;

  /// Where `frames` was said, so a refusal can point at it.
  std::optional<diag::SourceSpan> framesSite;

  /// The Modules the generator added for Transitions, when the graph has an
  /// edge: the routine and the Cell.
  std::optional<ModuleIndex> transitionRoutine;
  std::optional<ModuleIndex> transitionCell;

  /// What a `.xex` without storage decodes its transformed Sections with while
  /// it loads: the decoders, reading memory, and the glue the loader calls.
  /// Present in no Phase and placed by no solver — its own Step places it once
  /// the stored forms are known — see
  /// docs/decisions/0235-a-xex-decodes-in-place-while-it-loads.md.
  std::optional<ModuleIndex> loadModule;

  /// The Module holding one Cell per Slot, added at the end of Assemble when
  /// the program declares any — see docs/decisions/0031-slots.md.
  std::optional<ModuleIndex> slotCells;

  /// The Module whose Symbols are the Target's named Regions, added when the
  /// Project is read and any Region has a name — see
  /// docs/decisions/0038-regions.md.
  std::optional<ModuleIndex> regionModule;

  /// Every `constants` entry, in document order, and the Module whose Symbols
  /// they are — added when the Project is read and it declared any. The two
  /// travel together for the reason the Regions do: a name the Project
  /// declares reaches a Module the way an export does, so no Step learns that
  /// names come from anywhere but a Module.
  std::vector<ProjectConstant> constants;
  std::optional<ModuleIndex> constantModule;

  /// What a `choose` block named, in its order, where it was written, and the
  /// Module holding the byte the program chooses with and the routine that
  /// changes the bytes — see
  /// docs/decisions/0302-a-document-may-be-chosen-while-a-xex-loads.md.
  std::vector<Choice> choices;
  std::optional<diag::SourceSpan> chooseSite;
  std::optional<ModuleIndex> chooseModule;

  /// Every `transform` entry, in document order and unresolved.
  std::vector<TransformRequest> transforms;

  /// Every decoder the program declares, numbered as the routine dispatches:
  /// `copy` first, then in Project and declaration order. Filled at the end
  /// of Assemble, since a declaration is a Module's.
  std::vector<Decoder> decoders;

  /// The storage driver, when a Module declares one. Filled at the end of
  /// Assemble.
  std::optional<Driver> driver;

  /// What the program is, as its document named it. The raw image is what a
  /// document naming none is, which is what a program with no Payloads has
  /// always been written as. Known before Assemble, because a Container that
  /// loads through memory asks the program for a few bytes of glue — see
  /// docs/spec/xex.md.
  Container container = Container::RAW_IMAGE;

  /// The board `container car "xegs128"` named, or `container lnx "256k"`
  /// for a `.lnx` or a `.lyx`, and where it was written: resolved where the
  /// document was read, which refuses a name no board has. Absent for every
  /// other Container, and at most one of the two is there.
  std::optional<Cartridge> cartridge{};
  std::optional<LynxBoard> lynxBoard{};
  std::optional<diag::SourceSpan> cartridgeSite{};

  /// The format `container xex compress zx0` named, in which the file is
  /// compressed segment by segment and decoded where each lands while it
  /// loads, and nothing where the document asked for no compression — see
  /// docs/decisions/0259-a-xex-may-be-compressed-segment-by-segment.md.
  std::optional<std::string> compress{};

  /// The Modules `except` names after `compress`, groups expanded: a
  /// compressed file writes their bytes as they are, where a file without
  /// compression would — see
  /// docs/decisions/0260-a-compressed-xex-writes-what-it-is-told-as-it-is.md.
  std::vector<ModuleIndex> uncompressed{};

  /// Where the `container` statement stood, and nothing where the document
  /// gave none — which is also what says the raw image was nobody's choice
  /// and so is refused by no machine.
  std::optional<diag::SourceSpan> containerSite{};

  /// What the program is optimised for, and where the `optimize` statement
  /// stood. A document saying nothing is `FIT`, under which the tool takes no
  /// choice it cannot give back, so nothing compiles differently for the
  /// default being there — see docs/decisions/0177-intent.md.
  Intent intent = Intent::FIT;
  std::optional<diag::SourceSpan> intentSite{};

  /// What a `cartridge` block said, and where the block stood.
  LynxHeader lynxHeader{};
  std::optional<diag::SourceSpan> cartridgeBlockSite{};
};

/// Whether every image and every Frame of storage stays inside one unit: a
/// driver was promised `.driver span none`, or a unit is not usable to its end
/// and an image crossing into the next would stand in the gap. Where it does,
/// a stored form longer than the widest unit is cut into blocks and placed a
/// block to a unit; where it does not, nothing is cut and anything may cross.
/// One answer for every Step that asks, since a form cut for units it may
/// cross, or confined to units it was not cut for, is no layout at all.
[[nodiscard]] bool imagesStayInOneUnit( Project const& project );

/// Fills in every Module's Residency from the PhaseGraph: the set of Phases
/// whose `needs` name it. The one way a Residency comes to exist, for a read
/// Project and a synthesised one alike.
void deriveResidency( Project& project );

/// Adds a Module the tool writes itself, before anything is assembled: needed
/// by every Phase, every Module's Residency derived again. Its index.
ModuleIndex addToolModule( Project& project, ProjectModule module );

/// Every Phase but a `final` one other than the entry: where a program may
/// still leave, by an edge or by returning, or is entered by its Container,
/// and so where what only that runs has to be — see
/// docs/decisions/0245-a-final-phase-keeps-nothing-only-an-exit-runs.md.
[[nodiscard]] Residency notFinal( PhaseGraph const& phases );

/// The Project of a run given no Project file: one Module per file, named by
/// the file's stem, one unnamed Phase holding all of them, a default Target.
///
/// Colliding stems are an error and not a guess from the path: that is the
/// point at which a program has outgrown having no Project file, and aliases
/// are what a Project file is for. See
/// docs/decisions/0011-there-is-always-a-project.md.
Project
synthesiseProject( diag::SourceManager& sources, std::span<diag::FileId const> files, diag::DiagnosticSink& sink );

} // namespace nga::model
