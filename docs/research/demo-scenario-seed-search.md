# Demo scenario seed search

The seed search is `universe_demo_seed_search` (`src/demo_seed_search_main.cpp`). Scoring lives in `src/demo_scenario.cpp` and its tests are in `tests/demo_scenario_tests.cpp`. The measurements below were taken on base `71d5f8f` (schema v24).

## Scoring

Only change after the starting world counts:

| Component | Points | Cap |
| --- | --- | --- |
| Building completed | 10 each | 20 buildings |
| Population growth | 1 per % | 100 |
| Population decline | −2 per % | none |
| Settlement founded | 50 each | 2 |
| Discovery | 30 each | 3 |
| Institution | 20 each | 2 |
| Tier promotion | 60 each | 2 |
| Settlement merge | 40 each | 2 |

A seed is eligible only if at least one building completes and the population survives. Ranking puts eligible seeds first, then higher score, then lower seed.

## Finding: the 20-year demo has no visible building change

| Population | Years | Seeds | Seeds with a completed building | Tier promotions or merges |
| --- | --- | --- | --- | --- |
| 200 | 20 | 1–64 | 0 | 0 |
| 200 | 30 | 1–12 | 0 | 0 |
| 200 | 40 | 1–12 | 1 (seed 10, 1 building) | 0 |
| 200 | 50 | 1–12 | 12 (1–2 buildings each) | 0 |
| 200 | 100 | 51–54 | 4 (3–18 buildings each) | 0 |
| 600 | 20 | 1–4 | 3 (2–4 buildings each), but population falls 33–58% | 0 |

**Likely cause of slow construction (not verified by changing code):** construction labour comes only from people with `Occupation::None` (the labour loop in `src/simulation.cpp`), and occupation allocation almost always leaves `unallocated_workers=0`.

**Why no tier or merge change:** 200 people fragment into 13–27 settlements of about 8–15 people each, far below a Town.

What happens within 20 years: population +10–30%, 13–20 new settlements founded, 3 discoveries.

A demo scenario was not committed. No seed meets the "buildings visibly finish within 20 years" criterion from ticket #44, and committing one that fails its own criterion would misrepresent it.

## After the construction fix (#58)

Branch `fix/construction-labour`. Two changes to construction:
- **Labour:** the annual budget is idle workers' daily capacity plus 0.5% of employed workers' capacity (seasonal slack), times 365 days. It is zero when the settlement is subsistence-constrained.
- **Materials:** builders fell wood, hew timber and quarry stone for their own shortfall, capped by the settlement's regional access share.

Rerun at 200 people, 20 years, seeds 1–64: all 64 complete buildings, and none loses population.

| Rank | Seed | Score | Population | Buildings completed | Settlements founded | Discoveries |
| --- | --- | --- | --- | --- | --- | --- |
| 1 | 51 | 422 | 200→264 | 60 | 18 | 3 |
| 2 | 53 | 417 | 200→254 | 76 | 18 | 3 |
| 3 | 54 | 417 | 200→254 | 63 | 18 | 3 |
| 4 | 20 | 416 | 200→253 | 63 | 18 | 3 |
| 5 | 14 | 411 | 200→242 | 57 | 15 | 3 |
| 6 | 45 | 408 | 200→236 | 69 | 19 | 3 |
| 7 | 22 | 407 | 200→235 | 54 | 13 | 3 |
| 8 | 1 | 406 | 200→233 | 60 | 17 | 3 |
| 9 | 10 | 405 | 200→230 | 66 | 20 | 3 |
| 10 | 36 | 405 | 200→231 | 57 | 19 | 3 |

Completion is gradual rather than front-loaded. Seed 1 completes 6 buildings by year 1, 30 by year 5, 45 by year 10 and 60 by year 20.

Tier promotions and merges are still 0. That depends on the fragmentation question (#59).

## Committed demo scenario (#51)

`scenarios/demo.json` holds the demo scenario: seed 39, 200 people, 20 years, `"layout": "single_settlement"`. The layout is ADR 0002's single-settlement start (one region of 480 land units, one starting settlement), which is how the demo begins; a scenario file without a layout means the simulation's default multi-settlement start. `universe_ui_bridge --scenario scenarios/demo.json` and `scripts/run-demo-bridge.ps1` start the world from it, and the bridge, the seed search and the tests all build their config through `make_demo_simulation_config`, so they run the same world. No `*.ucore` is committed.

The search runs on the single-settlement start by default (`--layout default` searches the old layout). Seeds 1-64, 200 people, 20 years, after the fission and absorption rules (#61-#66) and the construction fix (#58). All 64 seeds that survive complete buildings; extinct seeds are ineligible. Top ten:

| Rank | Seed | Score | Population | Buildings completed | Settlements founded | Discoveries | Institutions |
| --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | 39 | 496 | 200→332 | 28 | 4 | 3 | 2 |
| 2 | 12 | 493 | 200→326 | 27 | 4 | 3 | 3 |
| 3 | 2 | 489 | 200→319 | 24 | 3 | 3 | 2 |
| 4 | 36 | 489 | 200→319 | 29 | 4 | 3 | 2 |
| 5 | 49 | 488 | 200→316 | 26 | 4 | 3 | 2 |
| 6 | 42 | 487 | 200→314 | 29 | 4 | 3 | 2 |
| 7 | 6 | 486 | 200→312 | 22 | 4 | 3 | 3 |
| 8 | 21 | 484 | 200→309 | 27 | 4 | 3 | 2 |
| 9 | 54 | 483 | 200→306 | 27 | 4 | 3 | 2 |
| 10 | 59 | 483 | 200→307 | 27 | 4 | 3 | 3 |

Seed 39 grows from one settlement into five (ADR 0002 target: 2-5 settlements, a largest of 70 or more; at year 20 its largest holds 103), on arable land. Tier promotions and merges are 0 for every seed on this start. The test `test_committed_demo_scenario_still_delivers_visible_change` fails if a later change to the world rules stops this scenario completing a building, keeping its population, and showing a new settlement or discovery; regenerate with `universe_demo_seed_search --write-scenario scenarios/demo.json` and update this table when that happens.
