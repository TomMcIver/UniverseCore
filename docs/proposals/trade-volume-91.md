# Proposal: make trade volume matter (issue #91)

Status: proposal for the owner's design session. No code is changed by this document.
Vocabulary follows `CONTEXT.md`. Decisions already made are in ADR 0003 and are not re-argued here.

Numbers marked "measured" were produced for this proposal by `universe_trade_calibration` in the `universecore-dev`
container (Release, 16 seeds, 20 years, 200 people, both layouts, each seed paired with `trade_enabled = false`).
The experiments were built from a scratch copy of the tree with temporary compile-time switches; none of that code is in the repo.
16 seeds, not the 64 of ADR 0003, so treat them as direction, not calibration. The baseline reproduces ADR 0003 (section 2).

## 1. Why wood, stone, timber and tools are not traded

**Short answer: a labour allocation problem first, a small-demand problem second, and the price signal is not wired to labour at all. It is not a resource-availability problem.**

1. **"wood_stock = 89,093" is the standing forest of a region, not a settlement inventory.** `universe_sim` prints `Region::wood_stock`
   (`main.cpp:492`). It sits at capacity because nobody cuts it: daily extraction is `min(need, quota, effective labour)`
   (`simulation.cpp:2085-2110`), so the forest stays full whenever need or labour is small. What prices and merchants see is
   `Settlement::materials`, and in the demo that is about zero.
2. **Food takes every worker, whatever the food stock.** The yearly allocation (`process_occupation_reassignment`, `simulation.cpp:6259`)
   fills food first and gives materials only the workers left over (the `while (true)` loop after `6514`). The food target is already stock-aware
   (`compute_daily_food_output_target`, `simulation.cpp:444`: 90% of need once cover is 90 days or more), but `simulation.cpp:6407-6412`
   then raises it back to a fixed 140% of daily need (`kAssignmentRequiredCoveragePercent`, line 288), blind to stock.
   Measured on the demo world (seed 39, single start): village 1 holds 1,159,778 food at year 20, and the whole world has
   139 cultivators and 37 gatherers out of 193 eligible workers, against 1 wood worker, 1 stone worker and 1 craft worker (the remainder are merchants or idle).
3. **Material demand is small and stock-shaped.** Targets scale with population (`material_targets_for_settlement`, `simulation.cpp:392`:
   timber `max(8, pop/2)`, tools `max(4, (workers+2)/3)`) plus construction still owed (28 buildings in 20 years in the demo).
   Tool wear is the only continuous sink (`kToolWearWorkUnitsPerBreak = 24`, line 289).
4. **Prices are not an input to labour.** `settlement_prices` (`simulation.cpp:5275`) is read by merchants only; the allocator never calls it.
   A price is base x 30 days of target cover over actual cover, clamped 0.25x-4x (`trade_pricing.cpp:48-60`), and zero stock against any
   positive need is the ceiling at once. So at year 20 in the demo, wood, stone and tools are at the ceiling (80,000 / 80,000 / 320,000 millicoin)
   in 4 of 5 settlements while food is at the floor (2,500). The spread is real; nothing turns it into labour.
5. **No regional comparative advantage in materials.** Wood and stone capacity are `land_area x 180` and `x 90` with only +-15% jitter
   (`simulation.cpp:4955-4970`); only food capacity depends on environment class. Extraction yields one unit per effective labour unit
   whatever the richness. So even a perfect price-to-labour link makes every settlement an equally good wood producer.
6. **80% of the money supply is locked in households.** Measured, seeds 1-8, per eight worlds: at creation commons hold 98,000 coin and households
   392,000; after 20 years commons hold about 90,000 (single) / 94,000 (default) and households about 400,000 / 396,000. Only the 20% profit share ever
   reaches a household, and only commons fund cargo (`units_affordable`, `simulation_trade.cpp:220`; ADR 0003 "Household coin on promotion").
   The "fixed money supply" in #91 is partly an artefact of households that never spend.

Not verified: why tool coverage stays near 0 even once labour is freed. Tools are made roughly 1:1 with stone and only 3-6 workers mine stone,
yet 27-42 eligible people were unallocated in the same runs (merchants included, a handful). I did not trace the stone allocation step by step.

## 2. Evidence: two cheap experiments (measured)

- **E1, stock-aware floor.** Skip the 140% floor when food cover is 90 days or more (one condition at `simulation.cpp:6406`).
- **E3, coin circuit (crude).** Households start with 1/5 of their coin; commons get 100% of the pre-division household total (instead of 25%).
  An upper bound on what a household-spending mechanism could free, not a design.
- **E2 = E1 + E3.**

| 16 seeds, 20 y | Units moved single / default | Trips single / default | Mean pair gap vs no-trade, permille, single / default | Pop trade / no-trade single, default |
|---|---|---|---|---|
| baseline | 2,107 / 737 | 197 / 159 | 2,532 vs 2,578 (-1.8%) / 2,493 vs 2,512 (-0.8%) | 3,866/3,866; 4,135/4,135 |
| E1 labour only | 37,385 / 25,435 | 1,241 / 993 | 2,476 vs 2,514 (-1.5%) / 2,757 vs 2,733 (+0.9%) | 3,770/3,827; 4,061/4,061 |
| E3 coin only | 19,875 / 3,087 | 1,741 / 460 | 2,028 vs 2,578 (-21%) / 2,546 vs 2,512 (+1.4%) | 3,899/3,866; 4,135/4,135 |
| E2 both | 192,923 / 91,819 | 8,652 / 3,173 | 2,310 vs 2,514 (-8.1%) / 2,701 vs 2,733 (-1.2%) | 3,827/3,827; 4,061/4,061 |

Departed trips by good (food/wood/stone/timber/tool): baseline single 19/43/37/0/98, default 0/22/46/0/91;
E1 single 758/49/32/0/402, default 557/49/118/17/252. In the baseline, most trips are already tools, not food: they come from the few
settlements that happen to make some. With labour freed, the food hoard and tools both start to move.

Readings:
- Neither lever alone is enough. Together, units rise about 90x (single) and 125x (default), which meets the "order of magnitude" bar of #91.
- "Price gap converges" is **not** met. Best case is -8% single and -1% default, and the widest gap stays 15,000 permille because stone
  and tools stay at the ceiling in some settlement (not diagnosed further). The all-goods mean also averages in ceiling-vs-ceiling pairs.
- E1 alone harmed one seed (single seed 2: 262 vs 319 people, 28,985 vs 0 stress days). Cause not diagnosed. My guess is food leaving a
  commons (758 food trips); unverified. E2 had a worst ratio of 1.000, but 16 seeds cannot show a rare failure.
- E1 empties the demo hoard (village 1: 1.16M to about 83k food) and changes every seed's world, so `scenarios/demo.json`,
  `tests/demo_scenario_tests.cpp` and `universe_demo_seed_search` need a fresh look (ADR 0003 already accepted this once for #86).

## 3. Options

A and B are independent in code, and the experiments suggest they are complements. For each, measure with
`scripts/run-trade-calibration.ps1` (64 seeds, both layouts, about 10 minutes). First extend the harness with (a) cargo units by good,
(b) coin held by households vs commons over time, (c) idle eligible workers, (d) per-good price gap.

### A. Labour follows need and price (fix the allocator)
- **What changes.** A1: make the 140% floor stock-aware (E1). A2 (larger): feed the allocator a continuous tool demand from wear, and let idle
  capacity tilt toward a good whose price multiplier stays high, so scarcity pulls workers.
- **Canonical state.** A1 none (pure function of food stock and need). A2 none if it reads derived prices; add one per-settlement counter
  (schema v31) only if oscillation shows up and needs hysteresis.
- **Determinism.** Allocation runs once a year in ascending id order inside the existing occupation bracket; prices are derived. Integer permille only.
- **Risks.** Famine if the floor is lowered where food is hidden in households; keep the `food_crisis` gate (`6402`). Every seed's world shifts.
  A2 can oscillate (labour chases price, price chases labour).
- **Size.** A1 small (about a day plus test updates). A2 medium to large and wants its own ADR.

### B. Close the coin circuit (households and commons exchange)
- **What changes.** Move coin between households and commons. Candidates: commons pay households coin for construction and material labour;
  households buy tools and timber from the commons at the settlement price (a real consumer for tools); or a yearly levy to the commons.
  Sellers get a sink, buyers get coin back.
- **Canonical state.** `Household::coin` and `Settlement::coin` already exist (schema v30), so moving balances needs no new field. Add a record only
  if purchases must be remembered. Coin is still created once, at world creation; the daily total-coin invariant keeps checking it.
- **Determinism.** Settle once a day on the food tick or yearly, in ascending household id; integer rounding must never create coin (reuse the
  trade trip remainder rule). Demotion already folds household coin into the commons; keep that conservation test.
- **Risks.** Interacts with household wealth and `process_local_exchange` (`simulation.cpp:8336`); wages invite an inflation story the fixed
  supply cannot support (prices drift down as the economy grows, per ADR 0003).
- **Size.** Medium. Needs an ADR 0003 amendment ("a household's coin matters only as a share of profit" stops being true).

### C. Real regional endowment differences
- **What changes.** Wood and stone capacity and yield depend on environment class (woodland rich in wood, marginal rich in stone, wetland poor),
  and extraction yield per labour unit scales with richness. A poor region then pays more labour per unit, which creates a margin that labour alone cannot close.
- **Canonical state.** None if derived from `RegionEnvironmentState::environment_class` (already saved). Only initial stock changes.
- **Determinism.** Reuse seeds already derived in region creation; no new randomness and no `uniform_*_distribution`.
- **Risks.** Useless in the single-settlement layout (one region; every daughter shares it), so it only helps the default layout until
  settlements differ within a region. Changes the initial world of every seed.
- **Size.** Small to medium. Best as an enabler after A and B, not alone.

### D. A second good that settlements make and want
- **What changes.** A new `ResourceType` (for example salt, cloth or pottery) with its own producer share and household need.
- **Canonical state.** Large: another inventory field, memory rows, price rows, events, and every `std::array<.., 5>` in the trade code. Schema v31 or later.
- **Risks.** Against the map's principle #10 (no new resource machinery) and ADR 0003 ("belongs to a resource-system change"). Also premature: the four existing
  non-food goods are barely produced today.
- **Size.** Large. Not before A and B show what the existing five goods can do.

### Independent: finer trade tick (#75)
Trade runs on the daily food tick. It does not drive volume (trips take days). Do it only if the Returning phase must be observable mid-day.

## 4. Recommendation (mine, for discussion)

1. Do **A1 and B together** as the next slice of #91, measured with the extended harness. Each alone fails the volume bar and together they
   exceed it by two orders of magnitude, so low volume is an allocator-and-circuit fact, not a limit of the design.
2. Treat **price convergence** as a separate, harder target. Expect it to need A2 and probably C, and say so in #91 instead of promising it from A1+B.
3. Defer D.
4. Before building, explain the seed-2 harm in E1 (what leaves the commons). A trade system that starves a seed is worse than none.

## 5. Questions for the owner

1. What is #91 for: volume, price convergence, or visible merchants in the demo? They need different work (A1+B, A2+C, or neither).
2. May food-floor changes (A1) move every seed's world and re-pick the demo seed?
3. Should household coin be spendable (B)? If "households keep a profit share and nothing else" is intended, the usable money supply really is about 20% of the total.
4. Should the commons pay households coin for labour? That makes households consumers of tools and food and links to future institutions.
5. Should regional differences (C) be class-driven, or settlement-local deposits within a region? Single-settlement layouts only benefit from the second.
6. Is the 140% floor a deliberate safety margin or a leftover? ADR 0002 notes the strain trigger is unreachable, which suggests the food model has more slack than the labour model assumes.
7. What population risk is acceptable: any seed below 90% of its no-trade population, or none?
