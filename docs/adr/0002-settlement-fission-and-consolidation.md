# Settlements split under pressure and small ones are absorbed

Starting from about 200 people the simulation produced 13-27 settlements of 8-15 people: initial placement already made about 8, and fission then split any settlement with spare access share and two viable years until pieces reached about three households. Merging required both sides to have 50 or more people, so the pieces fission produced could never merge, and the two rules contradicted each other. We decided that settlements split only under pressure, that those below a viable size are absorbed by a neighbour, and that both rules share one **viable size** of 20 people so they can no longer contradict each other. The target outcome is one dominant village of about 70-120 people plus 1-4 satellite hamlets of 10-30 after 20 years.

- **Fission** fires when a settlement reaches 120 people, or reaches 40 people and has been under food strain for 730 consecutive days. The daughter takes at least 20 people and the source keeps at least 40, so a strain-driven split needs at least 60 people in practice. A split resets the source's strain count.
- **Absorption** is asymmetric: the survivor must be viable, keep its 365-day viability streak, and be within the existing distance limit; the absorbed settlement qualifies once it has stayed below the viable size for 5 consecutive years, with no streak or 50-person floor on its side. The larger settlement survives. This amends the symmetric merge trigger decided in the settlement merging design.
- **Tier is unchanged** (Village/Town/City). "Hamlet" is a descriptive term for a settlement below the viable size, not a new tier.
- **New saved state:** two per-settlement counters in snapshot schema v26, consecutive days below the viable size and consecutive days under food strain. Both are zero for settlements loaded from v24 or v25 snapshots. A day is strained when at least a third of a settlement's households went short of food that day. The existing economic pressure streak was first assumed to be the strain signal, but it stays at 0 even in worlds that starve outright, so it cannot be used.

## Considered Options

- Keep opportunistic fission and only raise its minimums: rejected because a healthy settlement would still split whenever it has spare share, and a community splits because land or food gets tight.
- Change the simulation's default starting layout: rejected for now because it changes every existing seed's world and every test baseline; the demo scenario instead starts with one region and one settlement.
- Symmetric merging with a lower population floor: rejected because it would still stall on a pair of small settlements that each fail the viability streak.

## Consequences

World outcomes change for every seed, so tests asserting specific counts, hashes or settlement numbers are updated deliberately in the same change, each justified as an intended consequence. Determinism and conservation assertions are never loosened.

The strain trigger is currently unreachable in practice. In this simulation a world is either stable (no strain at all) or collapses within a year, so a settlement never stays strained for 730 days while still holding the 60 people a split needs. It is kept as decided and is covered at the counter level; it should be revisited, or removed, once resource pressure becomes gradual.

Measured over seeds 1-64 from the single-settlement start (200 people, one region of 480 land units, 20 years): 52 seeds end with 2-5 settlements and a largest of 70 or more, 10 die out entirely and 2 fall to about 75 people. The same start loses 13 seeds with fission switched off, so the collapses come from the single-region start, not from fission. The default four-region layout is left unchanged and does not splinter further.

Making fission and absorption fire exposed three defects in the existing merge, which had almost never run: it did not move a settlement's commons food; it left record accesses and the settlement-keyed record index pointing at the absorbed settlement, which made the world unloadable or let a live run drift from a reloaded one; and it did not move the aggregate ledger of a settlement in a demoted region, stranding its stock. All three are fixed. A snapshot round-trip and continuation test runs right after every absorption, including in a demoted region, and the ledger test was confirmed to fail without the fix.

Worlds stay healthier under the new rules, so long simulations now hold more people and the long test suite runs noticeably slower.

## Measured distribution for #66 (single-settlement start, seeds 1-64, 200 people, 20 years)

Settlements remaining: 0 in 10 seeds, 1 in 2, 2 in 0, 3 in 3, 4 in 20, 5 in 29. Among the 54 worlds that did not go extinct the largest settlement ranges 75 to 118 (median 97).

The outcome is set by the land class the seed gives the one region, not by the fission thresholds:

| Land class | Seeds | Meet 2-5 settlements, largest of 70+ | Extinct | End as one settlement |
|---|---|---|---|---|
| ProductiveArable | 31 | 31 | 0 | 0 |
| Woodland | 16 | 14 | 0 | 2 |
| Marginal | 10 | 7 | 3 | 0 |
| WaterWetland | 7 | 0 | 7 | 0 |

A marginal or wetland region yields about 2,200 productive units a day against about 6,800 for arable land, and the 200-person start starves on it. Eight of the ten extinct seeds never split (the world loses over half its people in year one, before the first year-boundary split can run) and the other two peak at two settlements. On woodland the first split works, then the source settlement shrinks to what the land carries and is absorbed, leaving one village of about 75. A split divides land without creating food, and the collapse is faster than the 730-day strain trigger. Raising initial household food from 80 to 200 still leaves 8 seeds extinct, and raising food production to 150% or 200% leaves 11, so this is not a shortage of starting food.

No fission or strain threshold was changed. #66 asked for 2-5 settlements and a largest of 70 or more "across seeds 1-64". Calibration cannot reach that on land that cannot feed the start, so the scenario test asserts the target strictly on every seed whose region is arable (31 of 31, no seed skipped, with a floor of 25 arable seeds so the filter cannot empty the test) and asserts only the structural invariants on the rest: household, population and land accounting, and the absorption rule below. The demo seed search (#51) should choose arable starts.

## Absorption timing fix found by #66

"No settlement stays below the viable size for more than five years" holds only when a neighbour could absorb it: absorption needs a viable neighbour with a 365-day streak in the same region within the merge distance, so a lone survivor or an isolated hamlet legitimately stays. The test asserts exactly that, at every year boundary, for both layouts over seeds 1-64.

That assertion found a real defect. Restructuring judged the survivor on `Settlement::population`, which is refreshed daily and so lacks the year's births. A neighbour that crossed the viable size at the boundary (for example 19 to 21 in default seed 5) was passed over, and the hamlet waited a further year, or for ever when the neighbour hovered near 20. Absorption now judges the survivor on its live population. The single-settlement distribution above is unchanged by this; the default layout is affected only where a small neighbour sits at the threshold. The absorption constants (5 years, 365-day streak, 2,000-unit distance) moved to `simulation.hpp` so the tests share them with the engine.
