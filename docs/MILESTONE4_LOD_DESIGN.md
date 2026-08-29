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

## Aggregate execution model (M4B/M4C/M4E as built)

### Execution separation

Aggregate execution is a distinct code path, not a data mirror of detailed simulation:

- `process_aggregate_food_period` runs per settlement, never per person.
- `process_aggregate_demography_period` runs per cohort, never per person.
- Detailed systems (`process_food_period`, `process_mortality`, `process_births`) skip any
  household whose region is not `Detailed`.

Two non-canonical counters (`person_daily_update_count`, `person_demography_update_count`)
are incremented at the canonical per-person processing sites so tests can assert that
aggregate regions reach them exactly zero times. They are never serialized and never
influence outcomes.

### Aggregate demography

Cohort rates are band averages of the canonical per-age tables, so aggregate execution
reproduces detailed demography in the mean:

| Band | Ages | Mortality /10k | Fertility /10k |
| --- | --- | --- | --- |
| Child | 0-17 | 40 | 0 |
| YoungAdult | 18-29 | 20 | 2,450 |
| Adult | 30-59 | 33 | 797 |
| Senior | 60+ | 790 | 0 |

Fractional outcomes use a seeded carry (`deterministic_share`), so results are deterministic
and free of rounding bias. Food scarcity raises mortality and suppresses fertility through a
per-year `required_food_year` / `unmet_food_year` window, preserving the M3 economic feedback.

Cohorts are merged each year by canonical key, with the lowest id surviving. This keeps cohort
count bounded by the bucket space rather than growing with simulated years.

### Canonization

`collect_canonized_person_ids` protects entities carrying durable provenance -- institution
founders and members, record authors, and discoverers. These keep exact detailed identity
across demotion; only anonymous population folds into cohorts.

### Conservation across transitions

Demotion moves household food, materials and wealth into the settlement aggregate ledger;
promotion hands them back and clears the aggregate, so stored food is counted exactly once.
Aggregate births and deaths are tracked separately (`aggregate_births_total` /
`aggregate_deaths_total`) because anonymous mass transitions deliberately emit no per-person
events; the event journal reconciles against the detailed portion of the canonical totals.

### Time acceleration

Aggregate settlements advance in 30-day chunks (`kAggregateFoodPeriodDays`); detailed regions
keep the daily cadence. Per-day flows are scaled by the chunk length so totals stay exact, and
the chunk phase derives from the absolute day counter so save/load mid-chunk is safe.

## M4 closeout verification

Canonical verification (Podman `./scripts/container-test.sh`): fast, integration and long
suites pass.

M4D mixed-LOD causality/equivalence coverage:

- aggregate-only execution proven free of per-person loops via instrumentation counters
- demotion of a natively-detailed region proven to stop per-person execution
- conservation of population, stored food, wood and stone across demote/promote
- canonized provenance anchors retained in exact detailed form
- cohort count bounded across 50 simulated years
- deterministic aggregate repeatability at 100k represented population
- promotion/demotion save/load identity reactivation without rerolling
- mixed LOD transition equivalence across save/load segmentation, including mid-chunk saves

M4E benchmark evidence (`universe_lod_benchmark`, 100 simulation years, seed 42, Release):

| Scenario | elapsed_ms | start pop | end pop | person_daily_updates |
| --- | --- | --- | --- | --- |
| `detailed_200` | 8,451 | 200 | 433 | 12,987,795 |
| `aggregate_10000` | 37 | 10,000 | 104,839 | 0 |
| `aggregate_100000` | 105 | 100,000 | 762,874 | 0 |

Cost per represented person: ~42,255,000 ns detailed vs ~1,050 ns aggregate.

The mixed-LOD lifecycle demonstration in the same executable advances ~200 detailed people
alongside ~10,000 and ~100,000 aggregate population, then promotes a region to detailed
(3,019 individuals realized), demotes it, saves, loads, returns 10 years later, and reactivates
3,018 of the 3,019 prior identities (the difference is one intervening death), with a
byte-identical deterministic replay.

### Known limitations

- Aggregate population is Malthusian: a region whose carrying capacity cannot support its
  represented population collapses toward capacity. Benchmark scenarios size regions
  accordingly.
- Detailed 200-person runs at `base_region_area_units = 140` still go extinct near year 100.
  This is the pre-existing Milestone 1 deferred item (long-horizon viability across broader
  seed distributions), not an LOD regression; benchmarks use area 1,000 where detailed runs
  are stable.
- World-level labour allocation totals (`total_labour_capacity`, `total_labour_demand`,
  `total_labour_allocated`) describe detailed execution only. Aggregate settlements advance on
  their own chunk cadence, so their labour figures live in the per-settlement
  `settlement_aggregates` records; folding them into the world totals would desync between
  yearly refreshes. Cohort workforce composition is still reported in the world worker counts.
- `RegionalCohort` remains a declared resolution level; only `Detailed` and
  `SettlementAggregate` have execution paths.
