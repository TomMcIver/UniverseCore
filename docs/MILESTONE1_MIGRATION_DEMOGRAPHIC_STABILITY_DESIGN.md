# Milestone 1 Migration + Demographic Stability Design (Implementation-Ready)

Status: design-only specification. No implementation in this revision.

Scope boundary (explicit):
- Migration is not currently justified as the primary solution to world-level demographic extinction under present fertility semantics.
- Father fallback remains world-global today.
- Migration remains intended for later settlement realism, resource/demographic locality, and eventual geographically local reproduction semantics.
- The v7 snapshot fields, cooldown mechanics, and HouseholdMigrated event shape below are provisional design targets until migration is actually scheduled for implementation.

Anchors:
- Baseline behavior targeted: verified state around a648ded.
- Code semantics traced from include/universe/simulation.hpp, src/simulation.cpp, src/snapshot.cpp, tests/simulation_tests.cpp.
- Baseline viability evidence: build/viability/baseline_p200_s1_50/summary.txt and related 50-seed analyses.

Proposed architectural decisions (for future implementation scheduling):
- migration unit = Household
- scope = intra-region settlement-to-settlement
- snapshot target = v7
- canonical event = HouseholdMigrated

## 1) Critical fertility assumption (reconfirmed)

Current fertility/birth semantics in src/simulation.cpp:
1. Candidate mothers: alive females with age in [18,49] and nonzero effective fertility rate.
2. Effective fertility rate is affected by:
   - age schedule
   - household alive-count saturation term (household size proxy)
   - person nutrition/health fertility scale
3. A birth is attempted per candidate mother using deterministic seed chance.
4. Father selection order:
   - first: alive adult male in same household (sample_father_in_household)
   - fallback: alive adult male from entire world state (sample_father_global)

Implication for migration scope:
- Father pool is not settlement-local or region-local after fallback; it is world-global.
- Settlement membership currently does not directly gate access to fathers if any global adult male exists.
- Moving a household between settlements does not directly change the reproductive candidate pool under current semantics.

Therefore, with current fertility implementation, migration can improve survival indirectly through pressure/resource rebalancing and settlement viability dynamics, but cannot by itself introduce new father connectivity constraints because global fallback already provides connectivity.

## 2) Exact canonical yearly ordering

Existing yearly demography order is:
1. mortality
2. births
3. settlement restructuring (extinction -> founding -> extinction)
4. occupation reassignment

Migration insertion for Milestone 1 is locked to:
1. mortality
2. births
3. settlement restructuring (existing behavior unchanged)
4. migration (new yearly pass)
5. occupation reassignment
6. yearly aggregate refreshes (existing post-pass refreshes)

Daily systems remain unchanged and continue to run by the existing scheduler between year boundaries.

Rationale:
- Births before migration preserves current fertility causality and avoids introducing mid-year parent-pool side effects.
- Keeping restructuring before migration preserves current founding/extinction decision semantics.
- Migration before occupation reassignment preserves a single canonical place where labour roles adapt to final yearly settlement placement.
- Determinism is maintained by fixed pass order, sorted stable IDs, and seed-scoped randomness only.

## 3) Demographic viability definition (explicit)

Demographic viability is represented by interpretable settlement-level integer conditions, not a synthetic weighted survival score.

Definitions per active settlement s:
- L_s: alive population in settlement
- F_s: reproductive-age females count (alive females age 18..49)
- M_s: adult male count (alive males age >=18)
- H_s: fertile households count (households in s containing at least one reproductive-age female and at least one adult male)
- D_s: dependent count (alive age <18 or age >=60)

Demographic stress classes:
- severe_demographic_isolation(s) is true if any:
  - F_s == 0 and L_s > 0
  - M_s == 0 and L_s > 0
  - H_s == 0 and L_s >= kMinPopulationForHouseholdFertilityCheck
- moderate_demographic_decline(s) is true if all:
  - L_s >= kMinPopulationForDeclineCheck
  - H_s < kMinFertileHouseholds
  - D_s * 100 >= kDependentPressurePercentThreshold * L_s

This design intentionally uses only integer comparisons and current canonical person/household state.

## 4) Threshold defaults (anchored to current robustness evidence)

Observed guidance from baseline analyses:
- population <10 is high-risk (about 87.2% extinction among runs crossing this range)
- reproductive bottleneck is a dominant failure mode in baseline analyses

Defaults (conservative, small parameter set):

1. kMinSourcePopulationForNormalMigration = 16
- Meaning: source settlement must have at least 16 alive people for non-terminal outbound moves.
- Sensitivity: lower value increases churn and source collapse risk; higher value may block useful adaptation.

2. kMinPopulationAfterMove = 10
- Meaning: post-move source floor for normal migration.
- Sensitivity: <10 materially increases extinction risk based on observed corridor; >12 may over-constrain movement.

3. kMinPopulationForHouseholdFertilityCheck = 8
- Meaning: require H_s > 0 when settlement is not tiny; avoids overreacting to very small remnants.
- Sensitivity: lower value triggers more moves in tiny settlements; higher value delays intervention.

4. kMinFertileHouseholds = 2
- Meaning: below 2 fertile households contributes to moderate demographic decline.
- Sensitivity: raising to 3 increases migration frequency in fragmented settlement maps.

5. kMigrationMinPressureDeltaPer10k = 800
- Meaning: destination pressure must be at least 8 percentage points lower by required/capacity ratio scale.
- Sensitivity: lower value can cause oscillation; higher value suppresses valid moves.

6. kMigrationMaxDestinationProjectedPressurePer10k = 12000
- Meaning: projected destination pressure after accepting move must stay <=120% required/capacity.
- Sensitivity: lower value rejects too many moves; higher value risks systematic overload.

7. kMigrationCooldownYears = 8
- Meaning: moved household cannot migrate again until eligible year.
- Sensitivity: short cooldown causes churn; long cooldown can trap households in deteriorating conditions.

8. kMaxOutboundHouseholdsPerSourcePerYear = 2
- Meaning: flow limiter to prevent one-year evacuation by normal migration.
- Sensitivity: 1 may be too rigid; >2 can destabilize medium settlements.

9. kMaxInboundHouseholdsPerDestinationPerYear = 3
- Meaning: destination intake limiter for conflict resolution.
- Sensitivity: too low blocks adaptation; too high concentrates pressure.

10. kMaxOutboundFractionPercent = 25
- Meaning: per source, moved households per year <= floor(active_households*25/100), with minimum 1 and cap by kMaxOutboundHouseholdsPerSourcePerYear.
- Sensitivity: higher fraction increases relocation shock; lower fraction slows adaptation.

## 5) Push model (lexicographic, deterministic)

Push uses lexicographic classes to avoid mixing incomparable units:

push_class(source settlement) priority:
1. class 3: severe_demographic_isolation(source) true
2. class 2: settlement_pressure_per_10k(source) >= kHighPressureThresholdPer10k (existing constant) OR source household stress streak indicator triggered
3. class 1: moderate_demographic_decline(source) true
4. class 0: otherwise (not eligible)

Household eligibility requires push_class >=1 plus household-level checks from section 9.

Rationale: demographic isolation remains primary; resource pressure is secondary but still strong; ordinary decline is weakest driver.

## 6) Pull model (lexicographic, deterministic)

Destination must pass hard filters:
- active
- same region as source
- different settlement id
- pressure_delta = source_pressure - destination_pressure >= kMigrationMinPressureDeltaPer10k
- projected_destination_pressure_after_move <= kMigrationMaxDestinationProjectedPressurePer10k

Destination ranking key (higher is better):
1. larger pressure_delta
2. larger projected spare capacity margin (kMigrationMaxDestinationProjectedPressurePer10k - projected_pressure)
3. lower destination pressure
4. lower destination settlement id (tie-break)

No bonus for raw settlement size; attractiveness is pressure/capacity-based only.

## 7) Source-viability protection

Normal migration move must satisfy post-move constraints:
- source_alive_after_move >= kMinPopulationAfterMove
- source_active_households_after_move >= 1
- if source_fertile_households_before > 1 then source_fertile_households_after >= 1

Terminal evacuation:
- out of scope for Milestone 1 migration routine.
- full abandonment can still occur through existing mortality and restructuring dynamics.

This prevents accidental source destruction while still allowing gradual decline.

## 8) Destination capacity rule

Use existing 1C/1E pressure semantics (no new synthetic population capacity):

For destination d:
- capacity_d = settlement_accessible_food_capacity_per_day(d)
- required_before_d = sum(food_requirement_for_person(alive person in d))
- required_move_h = sum(food_requirement_for_person(alive person in moving household h))
- required_after_d = required_before_d + required_move_h
- projected_pressure_after =
  - if capacity_d <= 0 and required_after_d > 0: 100000
  - else floor(required_after_d * 10000 / capacity_d)

Accept destination only if projected_pressure_after <= kMigrationMaxDestinationProjectedPressurePer10k.

## 9) Household eligibility and ranking

Household eligibility for proposal generation:
- household has >=1 alive member
- household belongs to active source settlement
- current demographic year >= household.next_migration_eligible_year
- source push_class >=1

Special household types:
- empty households: ineligible
- dependent-only households: eligible if source push class >=2
- elderly-only households: eligible if source push class >=2
- reproductive-household status is not used to engineer sex balance; only for viability constraints and source protection
- severe-health households are not forced; they are handled indirectly through stress and capacity metrics

Per-source household ranking (proposal attempt order):
1. higher source push_class
2. higher household_nutritional_stress_points
3. larger alive member count
4. lower household id

No scoped randomness required for ordering in Milestone 1.

## 10) Anti-thrashing cooldown semantics

Canonical household field (v7):
- next_migration_eligible_year: uint64

Semantics:
- unit: demographic year
- initial value on world generation and legacy migration-disabled worlds: 0
- after accepted move at year y: next_migration_eligible_year = y + kMigrationCooldownYears
- eligibility rule: current_year >= next_migration_eligible_year
- overflow behavior: saturate at uint64 max

Validation:
- field must be present in v7 household records
- value may be any uint64
- no additional derived counters persisted

Cooldown is combined with pressure deadband so cooldown is not the only anti-thrashing mechanism.

## 11) Simultaneous proposal algorithm (yearly)

Algorithm uses a single captured pre-migration view for fairness and determinism.

Pseudocode:

1. Build pre-state indexes:
   - active settlements by region (sorted by settlement id)
   - alive persons by household and settlement
   - settlement metrics (L_s, F_s, M_s, H_s, D_s, pressure)
   - household metrics (alive members, required_move_h, stress, cooldown eligible)

2. For each active settlement in ascending settlement id:
   - compute push_class
   - if push_class == 0 continue
   - produce ranked eligible household list

3. For each eligible household in ranked order:
   - enumerate destination settlements in same region
   - apply hard filters (section 6)
   - rank destinations by pull key
   - if non-empty, create proposal record with chosen best destination

4. Collect all proposals globally.

5. Resolve proposals with deterministic reservations (same-year capacity accounting):
   - maintain reservation maps:
     - outbound_count_by_source
     - inbound_count_by_destination
     - reserved_required_increase_by_destination
     - household_already_moved flag
   - sort proposals by conflict order key (section 12)
   - for each proposal in order, re-evaluate accept/reject using pre-state + reservations

6. Apply accepted moves:
   - call shared relocation primitive move_household_to_settlement(household_id, destination_settlement_id)
   - update household settlement id
   - set household.next_migration_eligible_year
   - emit one HouseholdMigrated event

7. Run process_settlement_extinction() immediately after migration pass to prevent phantom active empty settlements for the year.

8. Continue with occupation reassignment and aggregate refreshes.

## 12) Conflict resolution ordering

Global proposal sort key (descending/ascending as noted):
1. higher source push_class
2. larger source pressure
3. larger pressure_delta benefit
4. lower source settlement id
5. lower destination settlement id
6. lower household id

Rules:
- one household can be accepted at most once per year
- destination acceptance checks include reservation-adjusted projected pressure and inbound caps
- source acceptance checks include reservation-adjusted post-move viability and outbound caps

No vector traversal dependence: all collections are explicitly sorted.

## 13) Annual flow limits (conservative)

Per source settlement per year:
- outbound_limit = min(
    kMaxOutboundHouseholdsPerSourcePerYear,
    max(1, floor(active_households * kMaxOutboundFractionPercent / 100))
  )

Per destination settlement per year:
- inbound_limit = kMaxInboundHouseholdsPerDestinationPerYear

These limits are required to keep migration as a decades-scale adjustment mechanism and avoid one-year relocation shocks.

## 14) Resource-state movement semantics

On accepted household move, household-owned state moves unchanged with household:
- resources.food_inventory.value
- resources.food_produced_total
- resources.food_consumed_total
- resources.unmet_food_total
- resources.nutritional_stress_points
- resources.consecutive_shortage_days

No settlement-owned canonical food pool exists today; settlement food figures are derived aggregates from member households.

Region/environment state remains unchanged:
- productive_capacity_per_day
- wild_food_* fields

This preserves conservation and avoids rewriting historical counters.

## 15) Occupation semantics

Lock behavior:
- migration pass does not directly modify person occupation.
- occupations are reconciled in the existing yearly occupation reassignment pass after migration.

This remains consistent with current architecture and avoids duplicated assignment logic.

## 16) Founding interaction and relocation primitive

Introduce one shared primitive for location mutation and invariant-safe household transfer:
- move_household_to_settlement(household_id, destination_settlement_id)

Use this primitive from:
- settlement founding household split logic
- migration move application logic

Decision logic remains separate for founding vs migration.

## 17) Settlement extinction timing

To avoid phantom active empty settlements after migration, settlement extinction is evaluated immediately after migration via process_settlement_extinction() in the same yearly demography cycle.

This keeps downstream occupation and aggregates consistent in that year.

## 18) Snapshot v7 specification (exact additions)

Schema:
- bump kSchemaVersion from 6 to 7

SimulationConfig additions (canonical, persisted):
- migration_enabled: bool

Household additions (canonical, persisted):
- next_migration_eligible_year: uint64

Event additions:
- EventType::HouseholdMigrated = 2 (uint8 enum)
- SimulationEvent additions:
  - source_settlement_id: optional uint64
  - destination_settlement_id: optional uint64
  - region_id: optional uint64

Event subject adjustment for compatibility:
- subject_person_id remains present but is interpreted by event type:
  - Birth/Death: required valid PersonId
  - HouseholdMigrated: must be 0 (non-person event)

Serialization ordering:
- preserve existing deterministic id-sorted record ordering
- append new v7 fields in fixed order after existing v6 fields within each record type

Validation additions:
- migration_enabled must be decoded and preserved
- next_migration_eligible_year decoded for each household
- HouseholdMigrated must include household_id, source_settlement_id, destination_settlement_id, region_id
- source/destination settlements must exist, be distinct, same region, and reference event.region_id
- household_id must exist

No migration counters or derived metrics are stored canonically.

## 19) HouseholdMigrated canonical event shape

One accepted household move emits exactly one event:
- id: contiguous EventId
- timestamp: current canonical simulation time
- type: HouseholdMigrated
- household_id: required
- source_settlement_id: required
- destination_settlement_id: required
- region_id: required
- subject_person_id: 0 for this event type
- mother_id/father_id: absent

No per-person migration events.

## 20) Event determinism requirements

Determinism requirement:
- same canonical pre-state + same config + same ordered systems
  -> same accepted move set
  -> same event emission order
  -> same event ids

Save/load boundaries must preserve event order and ids exactly.

## 21) Migration ON/OFF control

Add canonical config toggle:
- migration_enabled: bool (default false for initial rollout benchmarking)

Behavior:
- false: migration pass is skipped
- true: migration pass executes using this spec

Snapshot implication:
- persisted in v7 config block
- load must preserve value exactly

## 22) Benchmark acceptance and required comparisons

Acceptance is directional, not absolute survival.

Baseline reference:
- 1000y survival 9/50
- reproductive-bottleneck-extinction signal materially present in baseline analyses

Required ON/OFF benchmark plan:
- identical seeds 1..50
- horizons: 250, 500, 750, 1000
- both migration_enabled=false and migration_enabled=true

Record per horizon/scenario:
- survivors/runs
- extinction cause breakdown
- median population
- migration event count (ON only)
- moved people total (ON only)
- active settlements
- pressure and food-deficit indicators
- reproductive bottleneck indicators

Directional acceptance target:
- ON has materially fewer reproductive-bottleneck extinctions than OFF
- ON has higher 1000y survival than OFF
- resource collapse remains possible
- no runaway population growth
- no migration explosion/churn signature

Additional scale comparison required (ON/OFF):
- starting populations including at least 20, 50, 100, 200, 500

## 23) Stress tests and invariants

Stress tests:
1. demographic isolation with viable neighbor -> migration occurs
2. demographic isolation with no viable destination -> no migration rescue
3. universal scarcity -> migration does not solve system-wide shortage
4. destination saturation contention -> caps and capacity reservation enforced
5. anti-thrash -> moved household remains ineligible through cooldown
6. long horizon -> no persistent annual oscillation explosion

Implementation-time invariants:
- world population conserved by migration
- PersonId preserved
- HouseholdId preserved
- family graph preserved
- occupation preserved during move step
- each alive household belongs to exactly one settlement
- source and destination share same region
- destination active at acceptance time
- no household migrates twice in same year
- capacity and flow limits respected
- cooldown respected
- event ordering deterministic

## 24) Risks to track explicitly

1. Migration may not materially change fertility outcomes because father fallback is world-global.
2. Migration may shift rather than resolve bottlenecks.
3. Over-concentration into a few settlements.
4. Oscillation despite deadband/cooldown.
5. Destination resource collapse from accepted inflows.
6. Household food/history accounting mistakes on relocation.
7. Extinction ordering drift versus current behavior.
8. Snapshot/event compatibility regressions in v7.
9. Integer arithmetic overflow in projected-pressure calculations.
10. Event-log growth under long horizons.

## 25) Ordered implementation sequence (for later coding pass)

1. Add canonical config/state declarations:
   - migration_enabled
   - household next_migration_eligible_year
2. Implement v7 snapshot schema changes and validation extensions.
3. Add event model extensions for HouseholdMigrated.
4. Add shared move_household_to_settlement relocation primitive.
5. Implement deterministic settlement/household demographic metric builders.
6. Implement migration proposal generation with hard filters.
7. Implement global deterministic conflict resolver with reservations.
8. Integrate migration pass into yearly scheduler order defined above.
9. Add immediate post-migration extinction check.
10. Add deterministic unit/invariant tests and scenario stress tests.
11. Add ON/OFF benchmark tooling path and output fields.
12. Run comparative 50-seed benchmark and analyze directional results.
13. Update design docs/dashboard status only after verified build+tests.
14. Final deterministic replay verification with save/load segmentation.
15. Commit/push only when explicitly requested.

## 26) Remaining unresolved architectural issue

The primary unresolved issue is semantic, not implementation detail:
- Under current fertility semantics, migration does not directly alter father candidate connectivity because of world-global fallback.

This design intentionally does not redesign fertility in Milestone 1.
Migration remains potentially useful for pressure/capacity balancing and settlement viability management, but it is not currently justified as the direct solution to world-level demographic extinction while father fallback is world-global.
Migration implementation remains a later milestone for locality/realism goals and eventual geographic reproduction semantics, and the v7/cooldown/event details in this document remain provisional until that implementation is scheduled.

---

This document is implementation-ready policy specification only. No simulation code, snapshot code, tests, dashboard, roadmap, or git history are changed by this document revision.
