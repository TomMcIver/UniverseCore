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
