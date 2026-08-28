# Milestone 4 LOD Design

## Purpose

Milestone 4 introduces deterministic simulation level-of-detail (LOD) so UniverseCore can scale beyond fully detailed individual simulation while preserving canonical causality.

LOD is an execution strategy over one canonical world state, not alternate world branches.

## Resolution levels

UniverseCore uses three canonical execution levels:

1. **Detailed**
   - Existing person/household simulation
   - Full local causality
   - Used for player-local and causally sensitive regions

2. **SettlementAggregate**
   - Settlement-level exact ledgers + household distribution buckets
   - No per-person daily loops for non-canonized anonymous population
   - Preserves economy/buildings/inequality signals

3. **RegionalCohort**
   - Sparse cohort representation for distant regions
   - Aggregate demographic and economic transitions
   - Deterministic promotion possible at any time

## Canonical contracts

### Exact conservation contract

The following remain exact canonical state across LOD transitions:

- population totals
- births/deaths totals
- finite resource stocks and capacities
- food/material inventories
- building stock/state totals
- ownership totals
- settlement identity/liveness
- institutions
- records
- consequential discoveries/events
- canonized entities
- historical provenance anchors

### Deterministic statistical-equivalence contract

The following may be represented statistically (deterministically), with explicit bucket contracts:

- anonymous identity micro-detail
- non-consequential household composition
- low-significance private memory topology
- intra-band variation

Important rule:

- promoted anonymous micro-detail must not be represented as if it had previously existed as exact canonical historical detail before promotion.

## Cohort model

### Cohort key

Minimum canonical cohort key:

- region_id
- settlement_id
- age_band
- sex
- occupation_group
- health_band
- wealth_band
- knowledge_profile_id

### Cohort identity

- Persistent `CohortId` is canonical.
- Split/merge operations use deterministic ordering and explicit stable rules.
- Canonical ordering for processing/serialization is lexicographic by `(region_id, settlement_id, cohort_id)`.

### Buckets

Default demographic/economic bucket sets are coarse integer bands to control dimensional growth.

## Household aggregation model

SettlementAggregate preserves:

- household count by size band
- household labour-capacity distribution
- household wealth distribution
- food-buffer distribution
- productive-access distribution

Settlement totals remain exact and are reconciled against bucket updates each period.

## Canonized entity rules

Entities that cannot be collapsed into anonymous cohorts include:

- player-touched persons
- discoverers
- institution founders/key members
- durable record authors
- consequential event anchors
- consequential building owners/founders

Canonization is bounded by deterministic policy and never removes entities still required by durable provenance.

## Promotion

Promotion from aggregate representations to detailed representation is deterministic and derived from:

- simulation seed
- cohort id
- canonical time/period
- settlement/region ids
- aggregate ledgers and bucket counts
- persistent realization metadata
- canonized anchors

Promotion does not fabricate fake exact history.

## Demotion

Demotion from detailed representation preserves:

- exact conserved totals
- institutions/records/events provenance
- canonized entities

Anonymous non-canonized population folds into cohorts and household buckets.

## Knowledge/history handling

Three layers:

1. exact durable history/provenance (events/records/institutions)
2. canonized individual subjective state
3. aggregate knowledge prevalence profiles for anonymous cohorts

No omniscience is introduced.

## Event strategy

- Keep consequential events exact and typed.
- Represent anonymous mass transitions with compact aggregate demographic events/summaries where needed.
- Avoid event-journal explosion from per-anonymous-person events.

## Scheduler / time acceleration

Resolution-aware cadence:

- Detailed: daily + yearly systems
- SettlementAggregate: monthly + yearly aggregate systems
- RegionalCohort: yearly or multi-year chunk updates

Chunked advancement must preserve exact contracts and deterministic statistical contracts.

## Anti-reroll behavior

Repeated enter/leave transitions cannot reroll canonical outcomes.

Determinism is enforced by:

- immutable seed-domain derivation
- persistent cohort identity
- stable period identifiers
- persisted realization anchors for promoted cohorts

## Snapshot rules

Snapshot schema will include canonical LOD state:

- region/settlement resolution
- cohort records
- household aggregate state
- knowledge prevalence profiles
- canonization/archival anchors
- LOD scheduler metadata
- realization metadata

Derived caches/indexes remain non-canonical and are rebuilt after load.

## Performance targets

Benchmarks for:

- 200 detailed
- 1,000 detailed (if practical)
- 10,000 aggregate
- 100,000 aggregate
- 1,000,000 aggregate stretch

Tracked metrics:

- runtime
- peak RSS
- snapshot size
- snapshot load time
- event/memory/knowledge growth

M4 success requires material scaling improvement for large off-screen populations versus full detailed execution.

## Unreal-facing implications

M4 does not integrate Unreal.

It establishes engine-neutral commands/queries for future adapter use:

- resolve_region
- resolve_settlement
- release_region
- query_visible_people
- query_visible_buildings
- query_settlement_descriptor

Presentation remains non-canonical and cannot directly mutate world state.

## Slice plan

- **M4A**: Cohort representation substrate + snapshot support + deterministic identity rules
- **M4B**: Settlement household/economic aggregation
- **M4C**: Deterministic promotion/demotion with canonization
- **M4D**: Causality/equivalence test framework
- **M4E**: Time acceleration + performance closeout

## Exit criteria

Milestone 4 is complete when UniverseCore can:

- simulate much larger off-screen population cheaply
- resolve selected regions to detailed individuals deterministically
- preserve conservation/history consistency across LOD transitions and save/load
