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
- `Birth` and `Death` event emission from simulation state transitions
- event payload includes canonical simulation timestamp (`SimulationTime`) and affected entity IDs
- deterministic ordering derived from deterministic system update order

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
- annual mortality by age band
- annual fertility by age band with household-context father sampling
- parent links on newborn creation

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
