# Roadmap

## Milestone 0: Repository and deterministic kernel

Status: **complete**

- [x] C++23/CMake project
- [x] headless executable
- [x] deterministic PRNG
- [x] seed/config handling
- [x] basic demographic smoke test
- [x] deterministic unit test
- [x] hierarchical seed derivation
- [x] canonical simulation clock
- [x] event types and event log
- [x] snapshot serialization

Exit criteria: same seed/config produces byte-identical canonical snapshot across supported platforms.

## Milestone 1: 200-person settlement

Status: **foundation complete (migration deferred)**

- [x] person IDs
- [x] age/sex
- [x] households
- [x] kinship
- [x] fertility by age/context
- [x] mortality by age/context
- [x] food production/consumption
- [x] health/nutrition
- [x] basic simulation UI (optional SDL3 + Dear ImGui inspector with smoke test)
- [x] deterministic land/environment resources
- [x] deterministic settlements and local carrying capacity
- [x] occupations
- [x] resource inventory
- [ ] migration
- [x] settlement population statistics

Exit criteria: 100-year runs remain internally valid and produce explainable demographic histories.

## Milestone 1 stabilization pass (post-1F integration)

Status: **complete (foundation stabilized; migration implementation deferred)**

- [x] deterministic diagnostics for yearly demographic/resource/labour signals
- [x] integrated food-labour-health feedback calibration pass
- [x] deterministic regime regression scenarios (sustainable / famine / constrained)

Deferred follow-up (non-blocking for Milestone 2 start):

- [ ] extend long-horizon viability across broader baseline seed distributions

## Milestone 2A: Material production chains

Status: **complete**

- [x] raw material opportunity integrated into regional environment state
- [x] deterministic wood extraction and stone extraction
- [x] settlement-level material inventories
- [x] deterministic material processing/crafting
- [x] canonical Timber and BasicTool outputs
- [x] snapshot schema v7 for canonical material state persistence/validation

Current M2A modelling scope (intentional abstractions):

- aggregate material quantities, not per-item object simulation
- settlement-level inventory ownership only
- no private ownership/wealth accounting
- no trade or market mechanics
- no building/materialized construction system
- no technology gating of production chains
- primitive generic `BasicTool` output abstraction
- wood treated as renewable stock abstraction
- stone treated as finite non-regenerating abstraction

## Milestone 2B: Deterministic tools durability and productivity

Status: **complete**

- [x] deterministic settlement-level tool wear accumulation and breakage
- [x] deterministic per-settlement tool coverage tracking (0..10,000)
- [x] bounded productivity scaling from tool coverage in food/material output flows
- [x] closed-loop replacement pressure via existing tool deficit/crafting path
- [x] canonical durability/coverage world aggregates for diagnostics
- [x] snapshot schema v8 for tool durability + coverage persistence/validation

## Milestone 2: Persistent history and knowledge

Status: **complete**

- [x] event journal
- [x] agent memory
- [x] knowledge graph/state
- [x] communication/teaching
- [x] records
- [x] knowledge loss
- [x] basic discoveries
- [x] institutions

Exit criteria: a discovery can originate with one person, spread through part of a population, survive or disappear, and be traced historically.

## Milestone 3: Economy and settlement growth

- [x] work allocation
- [x] production chains
- [x] ownership/access
- [x] exchange
- [x] construction
- [x] land/resource pressure
- [x] inequality/household wealth

Status: **complete**

Exit criteria: settlement growth is constrained by real resources and labour rather than scripted population thresholds.

## Milestone 4: Simulation LOD

Status: **complete**

- [x] cohort representation
- [x] individual ↔ cohort promotion/demotion
- [x] household aggregation
- [x] causality preservation tests
- [x] time acceleration policies

Exit criteria: simulate a much larger off-screen population cheaply, resolve a region to individuals, and preserve statistical/history consistency.

Verified by `universe_lod_benchmark`: ~200 detailed people, ~10,000 and ~100,000 aggregate
population advance together; a region promotes to detailed, demotes, survives save/load, and
reactivates its prior identities without rerolling. Aggregate execution costs ~1,050 ns per
represented person against ~42,255,000 ns for detailed simulation.

Scope notes:

- `RegionalCohort` is defined as a resolution level but only `Detailed` and
  `SettlementAggregate` have execution paths.
- Aggregate populations are Malthusian; regions must be sized for the population they carry.

## Milestone 5: First AI cognition

- [ ] structured cognitive state
- [ ] planner interface
- [ ] inference adapter
- [ ] schema validation
- [ ] knowledge-constrained prompts/context
- [ ] deterministic fallback agent

Exit criteria: one significant NPC can make a model-assisted decision without accessing hidden world truth or directly mutating canonical state.

## Milestone 5 progress note

Milestone 5 is not started as a whole (no planner interface, inference adapter or schema validation yet). Groundwork that exists: the cognitive-state and activity/schedule model was decided under #34 and built as a deterministic person daily schedule and activity state (#37), with canonical household positions (#38). This is deterministic behaviour, not yet a planner interface.

## Milestone 6: Presentation clients (was "Unreal bridge")

Status: **demo slice delivered on Godot and a web client; Unreal not started and no longer the planned first client**

The project built presentation clients over a view-model protocol instead of an Unreal bridge. The no-Unreal native client direction is tracked in #8-#14 (engine comparison in `docs/research/bevy-vs-godot-native-client.md`).

- [x] WebSocket bridge: `universe_ui_bridge` speaks a line-delimited protocol on stdio and `apps/web-client/server/ws-bridge.mjs` exposes it on localhost (#2, #7)
- [x] Typed view-model protocol and panel registry (#52)
- [x] PlayCanvas/TypeScript web client (`apps/web-client`, `docs/PLAYCANVAS_WEB_CLIENT.md`)
- [x] Godot 4.7 client (`apps/godot-client`, the current main client): connects, handles reconnects, renders terrain from environment data, assets and lighting (#15-#19)
- [x] NPCs at canonical household positions with navmesh pathing (#39, #40), and route-based villager movement along the village's tracks for the demo (#56)
- [x] Economy, time-advance and life-history panels (#54)
- [x] Player avatar and descent from command view to ground view (#57)
- [x] Demo village look and staged construction visuals (#55, #85)
- [x] Committed demo scenario (`scenarios/demo.json`, seed 39) and seed search (#51); demo sequence evidence in `docs/demo/README.md`
- [x] Merchants drawn travelling between settlements (#78)
- [ ] Construction builders walking to sites (construction is drawn in stages, but villagers do not yet walk to the site)
- [ ] Unreal C++ project and adapter (deferred; superseded for now by the Godot client)

Original exit criterion: player can enter a settlement whose visible state is generated from UniverseCore, leave for twenty simulated years, return, and see the resulting coherent world. The demo sequence (command view, jump 20 years, descend to the ground view, inspect the economy and an NPC's life history) is essentially this, and `tests/demo_scenario_tests.cpp` guards the demo world. It has not been formally declared as a milestone gate.

## Settlement growth, fission and trade (post-Milestone 4 work)

Status: **settlement dynamics delivered; trade built but economically weak**

- [x] Settlement tier (Village/Town/City) computed live, and settlement merging (#30-#33; epic #26 is still open)
- [x] Canonical region/settlement and household positions, life events and building placement (`docs/adr/0001-building-placement-canonical-person-positions-presentation.md`; #23-#25, #35, #38, #45, #46, #53)
- [x] Single-settlement world start for the demo scenario (#63)
- [x] Settlement fission under pressure with minimum sizes, and absorption of settlements that stay below viable size (#61, #64, #65), calibrated across seeds (#66). See `docs/adr/0002-settlement-fission-and-consolidation.md`
- [x] Trade epic #68 (`docs/adr/0003-settlement-trade-and-merchants.md`): conserved coin endowment (#69), prices from days of cover (#70), merchants appear when trade pays (#71), merchant trade trips (#72), remembered prices (#73), safe trip endings (#74), determinism and resume proof (#75), calibration over many seeds (#76), economy panel (#77), drawn merchants (#78), trips that sell (#86)
- [ ] Trade volume and price convergence: after #86 trips sell, but trade volume is small and price convergence is weak (all-goods price gap about 1-2% smaller than with no trade; population within 0.3% with or without trade). Follow-up #91 (seller coin sinks, a second good, a finer trade tick) and epic #68 are open
- [ ] People migrating between settlements (see Milestone 1)

Snapshot schema is v30 (`include/universe/snapshot.hpp`; v24 and later migrate on load). Version history is in `docs/ARCHITECTURE.md`.

## Gap: vision pillars not yet on the roadmap

`docs/GAME_VISION.md` names pillars that have no milestone. They are listed so they are not forgotten. None has a design or a date.

- Player continuity: rebirth into a household of the continuing civilization, with economic continuity explained by world mechanisms and not an unexplained bank account.
- Time model: acceleration must not become tactical power. The vision separates real-time local simulation, coarse simulation while the player is inactive, explicit waiting or stasis, and technology-enabled subjective time. Milestone 4 has time acceleration policies, not this player-facing model.
- Alien worlds generated from constraints (gravity, atmosphere, chemistry, geology, climate, evolution): only touched on under "Later" as procedural geology/climate/biology.
- Multiverse without arbitrary magic: only touched on under "Later" as alternate universes.

## What is next

Honest candidates, in no priority order. The owner decides the order. "Design first" means a design session (a wayfinder map, as used for #20 and #26) should come before tickets.

- Finish trade: #91 (coin sinks, a second good, finer tick), measured with `universe_trade_calibration`. A measured plan exists; a short design check on the second good may be enough.
- People migrating between settlements (Milestone 1 "migration" is still unchecked; earlier design notes in `docs/MILESTONE1_MIGRATION_DEMOGRAPHIC_STABILITY_DESIGN.md`). Design first: it interacts with fission, absorption and household ownership.
- Milestone 5 AI cognition (planner interface, inference adapter, schema validation, deterministic fallback agent). Design first.
- Player continuity (rebirth, no unexplained bank account). Design first.
- Performance work: jump pacing, delta size and hot paths. Measure first; the performance suite (PR pending) is the starting point.
- Construction builders walking to sites (client and activity model).

## Later

- ecology
- procedural geology/climate
- procedural biology
- cities/nations
- advanced technology discovery
- local granular physics
- planets
- orbital mechanics
- interstellar travel
- cloud simulation
- optional multiplayer
- alternate universes
