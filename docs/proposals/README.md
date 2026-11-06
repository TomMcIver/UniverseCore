# Proposals index

Design proposals prepared for the owner's design sessions. They are documents only: nothing here is decided, and no code was changed.
Numbers marked "measured" came from `universe_trade_calibration` runs described in each document.

| Proposal | Question it answers | Key finding |
|---|---|---|
| [trade-volume-91.md](trade-volume-91.md) | Why is trade volume tiny, and what would make it matter (#91)? | Wood, stone, timber and tools are not made because food-first labour allocation ignores food stock and prices (`simulation.cpp:6407`). In a 16-seed experiment, freeing that labour plus un-locking household coin raised units moved about 90-125x; price convergence still only -1% to -8%. |
| [migration.md](migration.md) | What would "people migrate between settlements" mean now? | The Milestone 1 design is outdated in useful ways (settlements own goods and coin, households already relocate in fission and absorption), and "migration" hides two different goals that need different designs. |

## The roadmap candidates compared

| Candidate | What it is | Depends on | Unlocks for `docs/GAME_VISION.md` | Main risk |
|---|---|---|---|---|
| Finish trade (#91, #68) | Make merchants move enough goods and coin to change prices and growth | Allocator and household-coin changes (see proposal); no new resources | "Persistent history" and "emergent civilization": regions that depend on each other, prices that mean something, a reason for merchants and later institutions, roads and transport | Changes every seed's world; demo seed re-pick |
| Migration | People and households move between settlements | Settlement fission and absorption (ADR 0002, done); ideally trade finished, because prices and wealth are the pull signals | The population pillar: "fertility, mortality and migration emerge from nutrition, wealth, housing, opportunity, transport"; shrinking regions inside a growing world | Value is small while father fallback is world-global and the single-region demo has nowhere to go |
| Milestone 5: first AI cognition | Structured cognitive state, planner interface, inference adapter, deterministic fallback for one significant NPC | Existing agent memory, knowledge and merchant price memory (these are the seeds of "knows only what it perceived"); no dependency on trade or migration, but richer worlds give it more to decide about | "NPC cognition" pillar and principles 5 and 6 (no fake global knowledge, LLMs are not NPC state); the first model-assisted decision | Needs a validated structured-output boundary and a deterministic fallback; exit criterion is about hidden-truth access, which must be testable |
| Player continuity | The player persists across death (rebirth into a household), is not invulnerable, has no magic bank account | A canonical player entity (the player avatar is presentation-only today, `CONTEXT.md`), households and kinship (done), economics that can carry inheritance (trade and coin, ideally institutions) | "Player continuity" and "time acceleration is not free power" pillars; rebirth into a continuing civilization | Not on the roadmap as a milestone; touches canonical state, LOD (the player's neighbourhood must stay detailed) and time acceleration policy |

## Dependencies between them (as I read the code and docs)

- Trade (finished) -> migration: prices, coin and food cover are the natural pull factors. Migration before trade would use pressure only.
- Trade (finished) -> player continuity: "economic continuity should not be an unexplained bank account" needs coin that circulates, households that spend,
  and eventually institutions; today 80% of coin is idle in households (see the trade proposal).
- Migration -> player continuity: rebirth "into a different household" is a household-level relocation of an identity; the relocation primitive and cooldown rules overlap.
- Milestone 5 is the most independent. It benefits from trade (merchants are the only agents that make decisions from remembered, aging information) but needs neither.
- Anything that changes world outcomes forces a demo seed re-pick (`universe_demo_seed_search`, `scenarios/demo.json`), so batching world-changing work reduces rework.

## Recommendation (mine; the order is the owner's call)

1. Finish the cheap, high-evidence part of trade first (labour floor and household coin circuit, section 4 of the trade proposal), because it changes the world
   and it feeds the others, and the experiments are already encouraging.
2. Then either migration (option A, small) or Milestone 5, whichever the owner wants for the next demo. Milestone 5 is the bigger step toward the vision's
   differentiator; migration is the smaller, safer one.
3. Defer player continuity until there is a canonical player entity design and a decision on how much of the player's neighbourhood stays detailed under LOD.

What I did not verify: the Godot client's needs for any of these, and effort estimates beyond "small / medium / large".
