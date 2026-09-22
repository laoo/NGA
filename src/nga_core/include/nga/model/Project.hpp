#pragma once

#include "nga/diag/DiagnosticSink.hpp"
#include "nga/diag/SourceManager.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace nga::model
{

/// A Module's position in the Project.
///
/// Project order is the tie-break the ordering contract in
/// docs/spec/diagnostics.md rests on, and it is also the order files are
/// registered with the SourceManager, so the two never have to be reconciled.
struct ModuleIndex
{
  std::uint32_t value = 0;

  friend bool operator==( ModuleIndex, ModuleIndex ) = default;
};

/// A macro's position within its Module's list, which is declaration order.
/// Beside the Module's own index because the Project names one: the
/// driver's roles are macros — see Driver.
struct MacroIndex
{
  std::uint32_t value = 0;

  friend bool operator==( MacroIndex, MacroIndex ) = default;
};

/// A Phase's position in the PhaseGraph, which is declaration order.
struct PhaseIndex
{
  std::uint32_t value = 0;

  friend bool operator==( PhaseIndex, PhaseIndex ) = default;
};

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

  [[nodiscard]] bool includes( PhaseIndex phase ) const
  {
    return mPhases[phase.value];
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

private:
  std::vector<bool> mPhases;
};

/// A named state of program execution: the Modules it needs in memory, and
/// the Phases a Transition may lead to. A Transition is an edge and nothing
/// more; what crosses one is not settled.
struct Phase
{
  /// Absent for the one Phase of a Project that declares none.
  std::optional<std::string> name;
  std::vector<ModuleIndex> needs;
  std::vector<PhaseIndex> then;

  /// The Label the Phase starts at; absent means one named `entry`. Resolved
  /// against the Modules the Phase needs when a Container asks — see
  /// docs/decisions/0018-xex-container.md.
  std::optional<std::string> entry;

  /// Per Window of the Target, the state it shows when nothing has switched
  /// while this Phase runs, where the Phase or a group it needs said so;
  /// absent for the variant's base. See
  /// docs/decisions/0056-a-phase-chooses-a-base.md.
  std::vector<std::optional<std::uint32_t>> bases{};
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

/// A half-open range of addresses.
struct AddressRange
{
  std::uint32_t begin = 0;
  std::uint32_t end = 0;

  [[nodiscard]] std::uint32_t size() const
  {
    return end - begin;
  }
};

/// What a Region says about its addresses, in the order a more restrictive
/// one wins where two overlap: a Project declaring `reserved` over the
/// variant's `ram` narrows the pool, and nothing a Project declares turns a
/// register into RAM.
enum class RegionProperty : std::uint8_t
{
  RAM,
  RESERVED,
  REGISTER,
};

std::string_view nameOf( RegionProperty property );

/// A Region's position in the Target's list, which is declaration order.
struct RegionIndex
{
  std::uint32_t value = 0;

  friend bool operator==( RegionIndex, RegionIndex ) = default;
};

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

/// Where the solver may allocate from: the `ram` of the Regions with every
/// more restrictive Region cut out, in two lists — below `$100` and above —
/// because a PlacementClass names one of them. Derived from the Regions when
/// the Project is read, and what Place, the layout verifier and the map read
/// instead of the Regions themselves.
struct Pools
{
  /// Sorted and disjoint.
  std::vector<AddressRange> zeroPage;
  std::vector<AddressRange> general;

  [[nodiscard]] std::uint32_t zeroPageSize() const;
  [[nodiscard]] std::uint32_t generalSize() const;
};

/// The pools the Regions leave: every address that some `ram` Region covers
/// and no `reserved` or `register` Region does.
Pools poolsOf( std::span<Region const> regions );

/// The Regions of a Target no document described. Only what stands in for
/// what an OS and a DOS in memory cost until a variant says so: `$80-$FF`,
/// the program's half of the zero page while BASIC is out, and `$2000` up,
/// where the DOSes in common use have finished. The stack page is the one
/// hardware truth among them and is left out like the rest below `$2000`.
std::vector<Region> standInRegions();

/// A Bank's position in the Target's list, which is the order a Project will
/// one day take a prefix of.
struct BankIndex
{
  std::uint32_t value = 0;

  friend bool operator==( BankIndex, BankIndex ) = default;
};

/// Where an image waits: the unit its first byte is in, and the offset
/// there. An image may run on into the units after — see Target.
struct StorageAddress
{
  BankIndex bank;
  std::uint32_t offset = 0;

  friend bool operator==( StorageAddress, StorageAddress ) = default;
};

/// A unit set's position in the Target's list, which is declaration order.
struct UnitSetIndex
{
  std::uint32_t value = 0;

  friend bool operator==( UnitSetIndex, UnitSetIndex ) = default;
};

/// A named set of Banks, declared by the variant with `units NAME N`: how
/// many there are and nothing else. What the hardware calls each is the
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
};

/// A Window's position in the Target's list, which is declaration order.
struct WindowIndex
{
  std::uint32_t value = 0;

  friend bool operator==( WindowIndex, WindowIndex ) = default;
};

/// One entry of a Window's `views`: a named state, or a unit set that
/// contributes one state per Bank.
struct WindowState
{
  diag::SourceSpan nameSpan{};
  std::string name{};
  std::optional<UnitSetIndex> units{};
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

  /// Which of `states` is the base, a named one; absent for a Window with
  /// no state that holds when nothing switched, whose ranges are in no pool.
  std::optional<std::uint32_t> base{};
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

  /// The index of the base state; nothing where there is none.
  [[nodiscard]] std::optional<std::uint32_t> baseState() const
  {
    return base;
  }

  /// The index of the named state called `name`, or nothing: a unit set's
  /// name is not a state.
  [[nodiscard]] std::optional<std::uint32_t> namedStateOf( std::string_view wanted,
                                                           std::span<UnitSet const> sets ) const;
};

/// A Pane's position in the Target's list, which is declaration order.
struct PaneIndex
{
  std::uint32_t value = 0;

  friend bool operator==( PaneIndex, PaneIndex ) = default;
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
};

/// `raw` and `xex`, and nothing where the word names neither.
std::optional<Container> containerNamed( std::string_view word );

/// The word a Container is written as, which is what a finding lists.
std::string_view nameOf( Container container );

struct Target
{
  /// In declaration order, the stand-in until a document declares one.
  std::vector<Region> regions = standInRegions();

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

  /// What one unit holds.
  [[nodiscard]] std::uint32_t bankSize() const
  {
    return unitSize;
  }

  /// The whole of storage, end to end.
  [[nodiscard]] std::uint32_t storageSize() const
  {
    return unitCount * unitSize;
  }

  /// The unit set or the Window of a name, or nothing.
  [[nodiscard]] std::optional<UnitSetIndex> unitSetNamed( std::string_view name ) const;
  [[nodiscard]] std::optional<WindowIndex> windowNamed( std::string_view name ) const;
  [[nodiscard]] std::optional<PaneIndex> paneNamed( std::string_view name ) const;

  /// The unit set a Window shows, when it shows one: the first among its
  /// states, since a Window showing two sets is not one this tool has met.
  [[nodiscard]] std::optional<UnitSetIndex> unitSetShownBy( WindowIndex window ) const;

  /// Where a position in storage, counted end to end, falls: the unit and
  /// the offset in it. What a StorageAddress names is the start of an
  /// image, and the image may run on into the units after.
  [[nodiscard]] StorageAddress addressAt( std::uint32_t position ) const;

  /// The position, end to end, of a StorageAddress.
  [[nodiscard]] std::uint32_t positionOf( StorageAddress at ) const;

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
  REGIONS,
  CONSTANTS,
};

/// One entry of a `constants` block: a name the Project gives a value, and
/// the number it gave. The value is a **literal** and not an expression,
/// because this grammar has no terminator to end one with — see
/// docs/decisions/0178-a-configuration-is-a-document.md.
struct ProjectConstant
{
  diag::SourceSpan nameSpan;
  diag::SourceSpan valueSpan;
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
/// use is written, and `showAt` one whose index is in `X`.
struct WindowRoles
{
  MacroIndex show;
  MacroIndex showAt;
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
  MacroIndex open;
  MacroIndex read;

  /// One entry per Window of the Target, in the Target's order.
  std::vector<WindowRoles> windows{};

  /// Absent for a driver whose medium has no Window.
  std::optional<WindowIndex> stream{};
};

/// Which Container the run writes. Chosen by the output file's name, and
/// known before Assemble because a Container that loads through memory asks
/// the program for a few bytes of glue — see docs/spec/xex.md.
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

  /// The Modules the generator added for Transitions, when the graph has an
  /// edge: the routine and the Cell.
  std::optional<ModuleIndex> transitionRoutine;
  std::optional<ModuleIndex> transitionCell;

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
  /// always been written as.
  Container container = Container::RAW_IMAGE;

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
};

/// The base a Window shows in a Phase: what the Phase chose, or what a group
/// it needs chose, or the variant's; nothing for a Window with no base at
/// all. See docs/decisions/0056-a-phase-chooses-a-base.md.
std::optional<std::uint32_t> baseIn( Project const& project, PhaseIndex phase, WindowIndex window );

/// Fills in every Module's Residency from the PhaseGraph: the set of Phases
/// whose `needs` name it. The one way a Residency comes to exist, for a read
/// Project and a synthesised one alike.
void deriveResidency( Project& project );

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
