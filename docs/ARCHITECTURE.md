# Architecture

## Top-level separation

```text
UniverseCore (portable C++)
├── deterministic generation
├── simulation clock
├── events/history
├── population
├── economy
├── knowledge/technology
├── ecology
├── physical abstractions
├── simulation LOD
└── persistence

        ↓ adapter boundary

Presentation Runtime (future: Unreal Engine)
├── rendering
├── animation
├── input
├── local physics
├── audio
└── UI

        ↕ optional service boundary

AI / Cloud Services (future)
├── expensive cognition
├── large batch simulation
├── model inference
└── online/multiplayer services
```

UniverseCore must compile and run independently.

Current implementation status:

- headless canonical runtime via `universe_sim`
- optional local inspector runtime via `universe_ui` (SDL3 + Dear ImGui)
- explicit UI adapter boundary via `universe::ui::SimulationController`
- UI issues explicit simulation commands (advance/reset/save/load) and consumes read-only query views
- canonical simulation logic and state remain in `universe_core`

## Deterministic hierarchy

World objects should derive seeds hierarchically rather than sharing a mutable global RNG stream whenever order independence matters.

Conceptually:

```text
UniverseSeed
  └── GalaxySeed
       └── SystemSeed
            └── PlanetSeed
                 └── RegionSeed
                      └── SettlementSeed
                           └── HouseholdSeed
                                └── PersonSeed
```

A future stable hashing API should derive child seeds from parent seed + typed identifier. This prevents unrelated generation order from changing existing content.

Current implementation status:

- strongly typed canonical seed (`SimulationSeed`)
- explicit domain-tagged derivation (`SeedDomain`)
- deterministic `derive_seed(parent, domain, stable_id)` using fixed-width integer mixing
- scoped randomness decisions (for example person/fertility/year and person/mortality/year)

Invariant:

- canonical outcomes must depend on parent seed + domain + stable identifier/time
- canonical outcomes must not depend on unrelated mutable RNG consumption order

## Canonical state

Canonical reality consists of:

```text
Procedural base state
+ canonized generated details
+ ordered consequential events
+ persistent deltas
= current world state
```

Generated detail becomes canon once another persistent fact depends upon it. Canon must not silently regenerate differently after code changes. This implies generator versioning.

## Events

Major state transitions should eventually be representable as explicit events:

```text
Birth
Death
Migration
Marriage/partnership
ResourceExtracted
BuildingConstructed
KnowledgeLearned
KnowledgeForgotten
TechnologyDemonstrated
InstitutionFounded
ConflictStarted
ConflictEnded
PlayerModifiedTerrain
```

Not every frame-level physical interaction belongs in the historical event log. Store only information needed to reproduce canonical consequences or explain history.

Current implementation status:

- append-only canonical `event_log` in `WorldState`
- stable deterministic `EventId` assignment
- strongly typed event payload model (no unstructured optional-field bag)
- canonical event emission currently includes `Birth`, `Death`, `SettlementFounded`, `SettlementExtinct`, `DiscoveryMade`, and `InstitutionFounded`
- canonical event metadata includes deterministic ID + canonical simulation timestamp (`SimulationTime`)
- deterministic ordering derived from deterministic system update order
- canonical per-person agent memory state stores direct observations with deterministic `MemoryId`, owner, source `EventId`, acquisition time, and confidence
- objective event journal remains world truth; person memory capture is event-driven and observer-scoped (no automatic omniscience)

## Simulation LOD

Initial target levels:

```cpp
enum class SimulationResolution {
    Statistical,
    Aggregate,
    Entity,
    Detailed,
    Physical
};
```

Promotion:

```text
cohort → household → person → detailed agent → physical actor
```

Demotion:

```text
physical actor
  ↓ extract consequential state
structured agent
  ↓ aggregate nonessential detail
household/cohort
```

Every subsystem should define what information is conserved across promotion/demotion.

## Time model

Use integer simulation ticks, not floating-point wall-clock time, for canonical state.

Long-term design:

```text
Universe time: integer canonical ticks
Local high-resolution physics: engine timestep
Civilization simulation: scheduled coarse events
Population: daily/monthly/yearly processes according to resolution
```

Time acceleration changes simulation granularity. It does not alter physical law.

Current implementation status:

- strongly typed fixed-width canonical elapsed time (`SimulationTime`, `SimulationDuration`)
- canonical simulation advancement via `advance(duration)`
- yearly demography scheduled against canonical time boundaries, not ad-hoc year increments
- derived demographic calendar year via `DemographicCalendarAdapter` for age/fertility/mortality systems

## Population architecture

Prototype progression:

### Phase A
Individual people for a 200-person village.

### Phase B
Households + individual lifecycle, fertility, mortality, kinship and migration.

Current implementation status:

- deterministic person ID allocation
- person birth year, sex and alive/dead state
- household membership with stable household IDs
- settlement membership with stable settlement IDs
- annual mortality by age band
- annual fertility by age band with household-context father sampling
- parent links on newborn creation

Current canonical social hierarchy:

```text
Region
  └── Settlement
          └── Household
                  └── Person
```

## Resource architecture

Current implementation status (Milestone 1C):

- generic resource foundation with fixed-width integer quantities
- `Food` as the first canonical resource type
- household-level canonical resource state:
        - stored food inventory
        - cumulative produced food
        - cumulative consumed food
        - cumulative unmet food demand
        - bounded nutritional stress and shortage streak state
- deterministic region environment state with stable region IDs and household-to-region assignment
- finite per-region productive capacity and finite renewable wild-food stock/capacity
- deterministic per-period regional variation and region-scoped wild harvest limits
- bounded wild-stock depletion and deterministic regeneration with cumulative accounting
- world-level environment aggregates derived from canonical region state
- daily resource scheduler tied to canonical simulation time
- yearly demography remains scheduled separately, but now consumes food-stress signals

Current implementation status (Milestone 1E):

- deterministic settlement generation per region from canonical seed + stable IDs
- deterministic household-to-settlement assignment (order independent)
- multiple settlements per region with independent local outcomes
- finite region capacity allocated to settlements via canonical access shares
- local carrying-capacity pressure (settlement demand vs accessible capacity)
- deterministic yearly settlement restructuring (founding/splitting + extinction)
- extinct settlements retain identity but no active share allocation

Settlement restructuring follows [ADR 0002](adr/0002-settlement-fission-and-consolidation.md):
fission requires population or sustained food strain and preserves minimum source
and daughter sizes; a settlement below the viable size for five consecutive years
can be absorbed by a nearby viable settlement. Absorption transfers household,
material, commons-food and aggregate ledgers and preserves recorded identities.
The optional `initial_settlements_per_region` configuration supports a single
starting settlement without changing the default layout.

Finite resource-allocation hierarchy:

```text
Region environment capacity
        -> Settlement local access share
                        -> Household labour allocation
                                        -> Food production/consumption outcomes
```

Invariants:

- canonical resource quantities use integer units only (no floating point canonical state)
- resource outcomes depend on scoped seeds + stable household identity + canonical period identifier
- equivalent canonical time advancement schedules must produce identical food outcomes and snapshots

Current implementation status (Milestone 2A):

- deterministic raw material opportunity represented in canonical region environment state
- deterministic settlement-specialized labour paths for wood extraction, stone extraction, and crafting
- deterministic settlement-level material inventories
- deterministic processing chain:
        - raw wood/stone -> crafted outputs
        - canonical outputs: `Timber`, `BasicTool`
- canonical world aggregates for material labour, extraction, inventory, and shortfall signals

Current M2A modelling limits (intentional abstractions):

- material quantities are aggregate integers, not per-item physical objects
- inventories are settlement-level, not person/household/private-wealth ledgers
- no ownership/market/trade exchange layer
- no building or construction consumption system yet
- no technology gating over the current extraction/crafting pipeline
- `BasicTool` is currently a primitive generic output type
- wood is modelled as renewable stock abstraction
- stone is modelled as finite non-regenerating abstraction

Current implementation status (Milestone 3 complete):

- deterministic work allocation prioritizes subsistence before optional material/construction labour
- deterministic production-chain balancing integrates ownership, transfers/exchange, tool coverage, and construction demand
- deterministic settlement founding uses viability/resource constraints rather than threshold-only scripted growth
- deterministic bounded wealth/inequality and pressure/viability metrics are available in headless state and inspector UI

### Phase C
Cohort aggregation for distant populations.

### Phase D
Promotion/demotion between cohort and individual representations.

Large populations should not imply billions of continuously updated C++ objects.

## Knowledge architecture

Separate objective world truth from subjective agent knowledge.

```text
WorldTruth
  ├── physical facts
  ├── actual resources
  └── actual events

AgentKnowledge
  ├── observations
  ├── testimony
  ├── taught concepts
  ├── records
  ├── hypotheses
  └── confidence/provenance
```

Technology becomes possible through combinations of knowledge, material access, tooling, energy, skills and institutions.

Current implementation status:

- objective event journal remains canonical world truth
- direct-observation `AgentMemory` remains canonical subjective experience evidence
- deterministic typed `AgentKnowledge` stores each person's current structured state derived from memory/event provenance
- deterministic local communication/teaching can create non-omniscient transmitted memories with explicit speaker/recipient provenance
- deterministic settlement-local records can persist authored memory-derived claims and later produce record-read memories with explicit record/read provenance
- deterministic long-horizon knowledge loss decays stale confidence and can soft-forget low-confidence facts without mutating objective world truth
- deterministic basic discoveries can originate in-settlement and enter agent memory/knowledge via discovery events with canonical provenance
- deterministic institutions can found settlement-local teaching/preservation groups with deterministic membership and explicit event provenance
- canonical knowledge fact scope currently includes person life status, person birth-household knowledge, settlement status, and basic discovery keys
- knowledge upsert semantics enforce one current fact per semantic key (owner + fact kind + subject)
- no omniscience: world truth never auto-populates agent knowledge without memory evidence (direct, transmitted, or record-derived)

## AI integration boundary

Future model calls should be mediated by a deterministic/validated protocol:

```text
Canonical structured context
        ↓
Inference request
        ↓
Untrusted model proposal
        ↓
Schema validation
        ↓
Rule/physics validation
        ↓
Accepted simulation action/event
```

Never allow raw model text to mutate canonical world state directly.

## Persistence

Persistence will eventually require:

- snapshot format
- append-only events where useful
- content/generator versions
- migrations
- checksums
- deterministic replay tests

Do not choose a database until data access patterns are measured.

Current implementation status:

- canonical binary snapshot codec (`snapshot.hpp` / `snapshot.cpp`) separated from filesystem I/O
- explicit fixed header with magic + schema version + payload size + payload checksum
- fixed-width little-endian encoding for all persisted primitives
- canonical collection ordering during serialization (people/households/events by stable ID, household members by person ID)
- explicit load validation for malformed/truncated data, enum validity, ID uniqueness, references and scheduler invariants
- snapshot schema v15 is the canonical persistence baseline for typed event journal + material/tool/environment state + agent memory + agent knowledge + knowledge transmission + records + institutions provenance
- snapshot schema v25 adds person life events and canonical building placement, and is the first schema that migrates its predecessor (v24) on load instead of rejecting it:
  - `OccupationChanged` is recorded only for a living person's real change of job across the yearly reassignment. People resolved to individual detail (promotion out of a population group, or v24 migration) carry `occupation_history_resync_pending`, so a resolution transition is never recorded as a job change.
  - `HouseholdRelocated` is recorded when a household moves settlement, whether through settlement founding or settlement merging. It carries the living members who moved.
  - A building's placement (`position_x`, `position_z`, `facing_degrees`) is fixed when it is planned (ADR 0001):
    - Household-owned buildings go in the owner's yard, around the household's canonical position.
    - Shared stores and workshops go by the settlement's common.
    - Every other building takes the first free plot along three tracks, whose directions come from the settlement's seed.
    - Placement uses integer geometry only and depends only on settlement geography and lower-id buildings, so v24 saves are migrated by replaying it in id order.
    - Buildings of extinct or absorbed settlements keep their placement and remain obstacles.
  - The v25 section contains placements followed by resync ids.
- snapshot schema v26 adds the initial-settlement-count option and per-settlement
  daily counters for time below viable size and time under food strain:
  - The counters are written in settlement-id order and cannot exceed elapsed whole days.
  - v24 and v25 snapshots migrate with both counters zeroed.
  - The v26 counter section follows the v25 section, before the aggregate demography totals, which remain the final 16 bytes.
- snapshot schema v27 adds coin (ADR 0003): a conserved balance on every settlement commons and every household:
  - The coin section is last in the payload, after the aggregate demography totals: a count and one (id, coin) record per settlement, then the same for households, each in ascending id order. Negative coin is rejected.
  - v24, v25 and v26 snapshots migrate with every balance at zero.
  - Coin is created once, at world creation, in proportion to each household's starting stock value (and each commons in proportion to its households); after that it only moves: demoting a region folds a household's coin into its settlement commons, and absorbing a settlement moves its commons coin to the survivor.

Settlement prices (ADR 0003, `trade_pricing.hpp`, `Simulation::settlement_prices`):

- A price is derived from state on demand and never stored, so it needs no snapshot field and a reload prices identically.
- price = base x multiplier. The base is the household wealth value of the resource times ten coin, in milli-coin per unit (food 10, wood 20, stone 20, timber 40, tool 80 coin). The multiplier is thirty days of target cover divided by actual days of cover, clamped to 0.25x-4x, in integer arithmetic.
- Stock is the commons plus detailed households' holdings, plus the aggregate ledger for food in a demoted region. Daily need is the people's food requirement (detailed members and cohorts), outstanding construction and crafting demand spread over thirty days for wood, stone and timber, and one tool-wear break per twenty-four worker-days for tools.
- No need is the floor price; no stock against a need is the ceiling. A demoted region approximates workers from the aggregate eligible-worker share until level-of-detail trade handling (#74).
- Thirty-day cover, the thirty-day horizon and the 0.25x-4x range are first values, calibrated by #76.

Snapshot invariants:

- snapshots are canonical simulation state, not debug dumps or raw memory images
- snapshot bytes for identical canonical state + schema version should be byte-identical
- no reliance on struct padding, native endianness, pointer values, or compiler ABI layout

Near-term direction:

- versioned schema dispatch (`load_v1`, future `load_v2`, ...)
- periodic snapshots + event segments/checkpoints for very long histories
- keep canonical encoding independent of storage transport (local files, object storage, databases, network blobs)

## Concurrency

Initial implementation should remain single-threaded and deterministic.

Introduce parallelism only after systems and ownership boundaries stabilize. Prefer deterministic task partitioning. Do not let thread scheduling determine world outcomes.

## Unreal integration

Unreal is intentionally deferred.

When introduced, use a narrow adapter layer:

```text
UniverseCore state
  ↓
Unreal adapter
  ↓
Mass entities / actors / terrain / PCG / Chaos
```

Unreal objects are presentation/runtime realizations, not the source of truth for long-lived civilization state.

## Inspector UI boundary (current)

The SDL3/ImGui inspector is a local debug/inspection surface, not canonical simulation state.

Boundary rules:

- UI must only mutate simulation through explicit command methods on `SimulationController`.
- UI frame timing must not drive canonical simulation outcomes.
- UI plots/history caches are derived view state and must remain non-canonical.
- Snapshot save/load behavior in UI uses the same canonical snapshot codec as headless simulation.
