# Settlements split under pressure and small ones are absorbed

Starting from about 200 people the simulation produced 13-27 settlements of 8-15 people: initial placement already made about 8, and fission then split any settlement with spare access share and two viable years until pieces reached about three households. Merging required both sides to have 50 or more people, so the pieces fission produced could never merge, and the two rules contradicted each other. We decided that settlements split only under pressure, that those below a viable size are absorbed by a neighbour, and that both rules share one **viable size** of 20 people so they can no longer contradict each other. The target outcome is one dominant village of about 70-120 people plus 1-4 satellite hamlets of 10-30 after 20 years.

- **Fission** fires when a settlement reaches 120 people, or reaches 40 people and has been under food or land strain for 2 consecutive years. The daughter takes at least 20 people and the source keeps at least 40.
- **Absorption** is asymmetric: the survivor must be viable, keep its 365-day viability streak, and be within the existing distance limit; the absorbed settlement qualifies once it has stayed below the viable size for 5 consecutive years, with no streak or 50-person floor on its side. The larger settlement survives. This amends the symmetric merge trigger decided in the settlement merging design.
- **Tier is unchanged** (Village/Town/City). "Hamlet" is a descriptive term for a settlement below the viable size, not a new tier.
- **New saved state:** two per-settlement counters (years under strain, years below viable size) that persist across years, so snapshot schema v26 migrates v25 snapshots with both at 0.

## Considered Options

- Keep opportunistic fission and only raise its minimums: rejected because a healthy settlement would still split whenever it has spare share, and a community splits because land or food gets tight.
- Change the simulation's default starting layout: rejected for now because it changes every existing seed's world and every test baseline; the demo scenario instead starts with one region and one settlement.
- Symmetric merging with a lower population floor: rejected because it would still stall on a pair of small settlements that each fail the viability streak.

## Consequences

World outcomes change for every seed, so tests asserting specific counts, hashes or settlement numbers are updated deliberately in the same change, each justified as an intended consequence. Determinism and conservation assertions are never loosened.
