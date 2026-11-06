# Proposal: people migrating between settlements (scoping)

Status: scoping proposal for the owner's design session. No code is changed by this document.
Vocabulary follows `CONTEXT.md`. Nothing here was built or measured except where marked "checked in code".

## 1. What the Milestone 1 design said

`docs/MILESTONE1_MIGRATION_DEMOGRAPHIC_STABILITY_DESIGN.md` (531 lines, design only, "provisional until scheduled") specified:

- **Unit and scope.** A household moves; only between settlements of the same region. Snapshot field and event shape assigned at implementation time
  (`next_migration_eligible_year` per household, `HouseholdMigrated` event).
- **Yearly slot.** After births and settlement restructuring, before occupation reassignment, then an immediate extinction check (section 2, 17).
- **Push.** Lexicographic classes: severe demographic isolation, then pressure at or above `kHighPressureThresholdPer10k`, then moderate decline (section 5).
- **Pull.** Hard filters (same region, pressure at least 800 per 10k lower, projected destination pressure at most 120%), ranked by pressure gain,
  spare capacity, lower pressure, lower id. "No bonus for raw settlement size" (section 6).
- **Guards.** Source keeps at least 10 people (16 to start), destination intake caps (2 out per source, 3 in per destination per year, 25% of households),
  an 8-year cooldown, one proposal pass from a captured pre-state with deterministic reservation order (sections 7-13).
- **Honest scope note (section 26).** Migration was *not* justified as a fix for extinction, because father selection falls back to the whole world
  (`sample_father_global`, still true: `simulation.cpp:7094-7096`), so moving a household does not change who can be a father.
  It was kept for "locality and realism" and "eventual geographic reproduction semantics".

## 2. What has changed since

| Then | Now |
|---|---|
| "No settlement-owned canonical food pool; settlement food is derived from households" | Settlements own `commons_food`, `materials`, `coin`, `access_share_points`, buildings and tool state (`simulation.hpp:242-285`). A moving household takes only its own `owned_materials`, `coin` and food; the settlement keeps the commons. |
| Households only change settlement through founding | Fission moves a planned set of households (`process_settlement_founding`, `simulation.cpp:5790`) and absorption moves all of them (`merge_settlements`, `simulation.cpp:6034`). Both already emit `HouseholdRelocated` (event 8; `simulation.cpp:5980`, `6128`). The shared primitive `move_household_to_settlement` the old design proposed was never extracted. |
| Settlements are static | ADR 0002: fission at 120 people (or 40 under 730 days of strain), absorption after 5 years below the viable size of 20 (`simulation.hpp:232-240`), same region and within 2,000 units. The system now manufactures hamlets and removes them. |
| Pressure is the only signal | Settlements also have prices (food multiplier is a derived cover signal), coin, `days_under_food_strain`, dwelling capacity bonus, and a tier. |
| Settlement location is implicit | Settlements and regions have canonical x/z positions (`Settlement::position_x`, `RegionEnvironmentState::position_x`). Distance rules (merchants: 3 days one way, `trade_trip.hpp`) already exist and work across regions. |
| LOD was only planned | Demoted regions hold people as `PopulationCohort` with a `settlement_id`, and `merge_settlements` already moves the aggregate ledger. A migration that ignores demoted regions will strand or duplicate people. |
| Merchants did not exist | Merchants already carry information (remembered prices) between settlements, the only existing model of "who knows what about a place". |
| Extinction was a generic failure | ADR 0002: on the single-region start, 10 of 64 seeds go extinct (all 7 wetland, 3 of 10 marginal) because the region cannot feed 200 people; 8 of those 10 never split. These worlds have no other settlement to move to. |

Still true: father fallback is world-global, births ignore settlement, and occupation reassignment runs after the demography bracket.

## 3. What migration would mean now

Two different goals hide behind the word; they need different designs, so the first question is which one is wanted.

- **Demographic balancing (what the old doc scoped).** Move people from strained settlements to ones with slack, inside a region. Mostly a pressure
  smoother. Probably small effect: fission already relieves pressure by moving whole groups, absorption already consolidates, and the single-start extinctions
  are not rescued because there is nowhere to go.
- **Social and economic mobility (GAME_VISION "migration emerges from wealth, opportunity, transport, housing").** People or households move because a place
  offers work, safety, family or prices. Cross-region, with travel time, knowledge of the destination, and consequences for both ends (labour supply,
  merchants, settlement tier). This is the vision's "one region shrinks while the world grows".

## 4. Options

### A. Household moves inside a region (the old design, updated)
- Push: strain days (`days_under_food_strain`), pressure, hamlet status. Pull: lower pressure and spare capacity as before, plus the new commons food cover.
- Needs: extract `move_household_to_settlement` (from founding and merge), fix what travels (household coin and materials yes, commons no), a cooldown field
  on `Household`, a yearly pass before occupation reassignment, a new event or reuse `HouseholdRelocated` with a reason field.
- Smallest. Honest expectation: marginal demographic effect, but it makes hamlets and absorption interact sensibly (below).

### B. Household moves across regions with travel
- Households walk for N days (like a trip) or teleport at year boundary. Pull uses what the household could know: its settlement's merchants' remembered prices,
  kin in the destination, or only neighbours within a day's walk (reuse `kDaysWalkKnownNeighbours`).
- Needs: in-transit state if travel takes time (a record like `MerchantTrip`; hidden state must be serialized per AGENTS.md), region adjacency rules,
  possibly a "destination does not exist for demoted region" rule. Larger and touches presentation (people leaving).
- Unlocks rescue of marginal and wetland starts when the default layout is used; does nothing for the single-region demo start.

### C. Individual moves (marriage, work, youth leaving)
- Smaller flows driven by kinship: a person marries into another settlement's household, an adult follows work. No push class needed for household
  transfers, and it breaks the "household belongs to exactly one settlement" invariant only if a household would split across settlements. It would, so
  households would have to split or merge: a large change to households, kinship and `process_household_access_allocation`.
- Realistic and cheap in data, expensive in invariants. Not recommended before household-level moves exist.

### D. Aggregate flows between demoted regions
- Cohort-to-cohort flows (people per age band per year) between settlements in demoted regions, with no identities. Needed so that LOD promotion/demotion
  stays consistent when migration exists. Can be deferred only if migration is disabled for demoted regions, which then becomes an explicit rule and a test.

## 5. Interactions to decide up front

- **Viable size and absorption.** Inflow can keep a hamlet above 20 and stop absorption (`days_below_viable_size` resets); outflow from a hamlet speeds it. Decide
  whether migration targets, or avoids, hamlets. A plain rule that fits ADR 0002: migrants never leave a settlement that would drop below the viable size, and
  hamlets are absorbed rather than reinforced.
- **Fission.** Inflow can push a settlement past 120 and trigger fission, which moves households again. Cooldowns must be shared so a household that just moved
  from a fission is not eligible the same year.
- **Trade.** A merchant's home settlement loses an adult when its household leaves, and a merchant's household moving needs a rule (retire or keep the home). Price
  and coin consequences: a household's coin moves with it, commons coin does not. Immigrants do not change prices directly; they change need.
- **Tier.** Tier is derived from population and wealth with a 365-day hysteresis, so inflow can promote a settlement; no extra state needed.
- **Knowledge of destinations.** AGENTS.md principle 5: a household may only choose a place it could know about. A cheap reading: neighbours within a day's walk plus
  settlements its merchants have seen.
- **Occupation.** The old design (migration does not touch occupations; reassignment follows) still fits, and now interacts with the allocator issue in the trade-volume
  proposal (food-first labour).

## 6. What it needs canonically

- Per-household cooldown (`next_migration_eligible_year`) or a derived rule that needs none. Schema bump to v31 or later (current `kSchemaVersion` is 30,
  `snapshot.hpp:34`); older files migrate with cooldown 0 (the established pattern, v24-v30).
- In-transit records only for option B. A new event reason or event type; event type bytes are rejected in older files by the existing check.
- A migration ON/OFF config like `trade_enabled` so the paired comparison harness (`universe_trade_calibration` pattern) works.
- No new coin and no new resources: household coin, owned materials and food move with the household; commons stay.

## 7. Testing determinism

Reuse the established pattern (see ADR 0002 and 0003):
- Same seed, any advancement schedule, identical canonical state; snapshot round-trip and continuation right after a migration pass, including with a demoted region.
- Conservation checked every simulated day: total coin, total people, food and materials per resource; every household belongs to exactly one active settlement.
- Reservation order with explicit sorts (the old design's conflict key is a good starting point); no dependence on container order; seeded tie-breaks only through `derive_seed`
  with a new `SeedDomain` and (settlement, year).
- A migration-on/off paired sweep over seeds 1-64 for both layouts, reporting: households moved, hamlet lifetimes, settlements at the end, extinctions, people-under-strain days.
  Pass criteria are the invariants and "no seed worse than 90% of its no-migration population", as in trade.
- Existing seed-sensitive guards (`scenarios/demo.json`, `tests/demo_scenario_tests.cpp`, `universe_demo_seed_search`) will need a deliberate re-pick if outcomes shift.

## 8. Recommendation (mine, for discussion)

Do not start migration until the trade work from #91 has settled, and then do option A in its updated form, with the primitive extracted and
`HouseholdRelocated` reused, because (1) trade will change prices, coin and labour, which are exactly the pull signals migration would use,
(2) fission and absorption already exercise the hard parts (moving households, conservation, demoted-region ledgers), and (3) cross-region moves (B) need the
travel and knowledge model that is easier to design once merchants have real volume. Revisit B when a default-layout world needs it for rescue or presentation.

## 9. Questions for the owner

1. Which goal: smooth pressure inside a region, or economically driven moves across regions? (They are different features.)
2. Should migration try to rescue marginal and wetland starts, or is extinction there an acceptable outcome of land that cannot feed 200 people?
3. Household-level only, or are marriage and individual moves wanted for the first version?
4. Are moves instantaneous at the year boundary, or do people walk (and can be seen, and die) on the way?
5. What may a household know when choosing a destination: neighbours only, merchant-reported prices, kin?
6. Should hamlets be reinforced or left to be absorbed (ADR 0002 says absorbed)?
7. Is migration still wanted if father fallback stays world-global? If births become settlement-local later, this feature's value changes a lot.
8. Do merchants' households ever move, and does that retire the merchant?
9. What is the priority against Milestone 5 and player continuity (see `docs/proposals/README.md`)?
