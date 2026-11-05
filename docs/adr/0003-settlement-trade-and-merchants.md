# Settlements trade through individual merchants, priced by supply and demand, paid in coin

Settlements today share nothing with each other: `process_local_exchange` only splits goods among the households of one settlement, and a settlement short of wood or stone has no way to get it from one that has plenty (in the demo world, village 1 ends with over a million units of commons food and no woodcutters or quarriers). We decided that goods move between settlements carried by individual merchants, that prices come from each settlement's own supply and need, and that trades are paid in a simple conserved coin. Everything below is decided unless it appears under "To calibrate or confirm".

## Decisions

- **Merchant** is a new `Occupation::Merchant`, alongside the five existing jobs. It shows in the job counts and the economy panel, and needs a snapshot schema bump (v27 or later, whichever is next when it is built).
- **Dynamic numbers.** A settlement trains a merchant when the best price gap it knows of clears a threshold and it has a working-age person to spare. The person is picked by seeded choice from the settlement's eligible adults. Merchants are not a fixed share of the population: trade grows where it pays and fades as prices converge. A safety bound on the count is set when it is built.
- **Price** of each of the five resources (Food, Wood, Stone, Timber, BasicTool) in a settlement is derived from days of cover: current stock divided by that settlement's own daily need (food requirement, construction demand, tool wear). Price rises as cover falls, in integer arithmetic, bounded to a range around a base (initially 0.25x to 4x). It depends only on state, never on wall-clock time or turn order.
- **Coin** is a conserved, fixed-width integer balance held by settlements (commons) and households. It is a currency, not a sixth `ResourceType`, so the closed five-value enum is untouched. This amends map #26, which put a new resource system out of scope; the decision is to start with plain coin and extend it (credit, minted money, other forms) as civilisation grows.
- **Settlement of a trade.** The origin's commons funds the cargo, paying itself in coin at the origin's price. The merchant carries it to the destination and sells at the destination's price, then returns with the coin. The merchant's household keeps a fixed share of the realised profit and the commons gets the rest. A trade that would not profit is not made.
- **Distance.** Straight-line canonical distance (settlement positions, crossing region boundaries freely) divided by travel speed gives travel time. Trade is allowed up to 3 days one way. Travel speed is a function of the settlement's capabilities, walking pace for now. Better capabilities (pack animals, carts, roads, boats, later transport) raise the speed through that one function, so the same 3-day cap reaches further as civilisation advances. No such technology is built by this decision.
- **Cargo** is a base load per trip on foot, multiplied by a capability multiplier through the same hook as travel speed.
- **Risk.** No extra risk in the first version: a merchant faces only normal mortality while away. A travel-risk rule can be added later without changing the trip state.
- **Knowledge.** A merchant knows only the prices it last saw: its home settlement, settlements it has visited, and neighbours within a day's walk. Remembered prices carry the time they were seen and go stale, so arbitrage is real and fades as prices converge. This follows the rule that NPCs reason only from what they have perceived or been told.

## Canonical state

- `MerchantTrip`: merchant person id, origin, destination, cargo (resource and amount), coin outlay, departure time, arrival time, return time, phase. A merchant on a trip is marked away and does not work or appear in its settlement.
- Per-merchant price memory: (settlement, resource, price, time seen).
- Coin balances on settlements and households.
- Two new events, trade departed and trade completed. Both must be deterministic: seed plus an explicit domain plus stable ids, never sequential RNG consumption.
- The client draws a merchant walking between settlements along the tracks; the position is presentation, the trip is canonical.

## Considered options

- **Individual NPC merchants versus an abstract flow between settlements:** abstract flow rejected; the map decided merchants are individuals.
- **A merchant as a trip any craft worker takes:** rejected; the person would vanish from job counts while away.
- **Fixed share of workers as merchants:** rejected for dynamic numbers.
- **Perfect price knowledge within range:** rejected; it would leak engine truth into character knowledge.
- **Fixed exchange rates:** rejected earlier by the map.
- **One-way shipments with a debt ledger, or round-trip barter instead of coin:** coin chosen, because it is the base later money systems can extend.

## To calibrate or confirm

- **Coin supply (confirmed).** Coin comes from a seeded endowment at world creation, proportional to each settlement's starting stock value, and is conserved afterwards: total coin never changes and no settlement can make more. The simulation has no metal or ore, so there is nothing to mint from yet. The consequence to accept is a fixed money supply: as the economy grows, prices in coin drift down, which is harmless over the 20-year demo and would matter over centuries. Making more coin needs a metals resource and an institution that mints, which belongs to a later resource-system map; the coin balance is a plain integer field with a single place where coin is created (world creation), so a mint can be added there later without reworking trade.
- The profit-share fraction, the base cargo load, the profit threshold for training a merchant, the merchant count bound, and the price range. These get a measured sweep when built, the same way the settlement size distribution was calibrated, and any change is recorded here.
- The capability function for speed and load: for now it returns the walking baseline and a multiplier of one.

## Consequences

A canonical state change, so snapshot schema impact must be evaluated and versioned, with migration from the current version. Every world-critical piece needs deterministic tests: pricing, merchant selection, trip scheduling, coin conservation (total coin unchanged by any trade) and save/load continuation mid-trip. Time acceleration must not let trips resolve for free: a trip takes its real simulated days.

## Recorded while building (#70, #71)

- **Prices (#70).** A price is never stored: it is recomputed from state, so it needs no snapshot field. Base prices are the household wealth values times ten coin (food 10, wood 20, stone 20, timber 40, tool 80). The multiplier is thirty days of target cover over actual cover, clamped to 0.25x-4x. A settlement with no need prices at the floor; one with no stock against a need at the ceiling. Stock is the commons: household `owned_materials` are that stock shared out each day, so adding them would count it twice.
- **Recruitment (#71).** Once a year, inside the occupation bracket, right after the labour allocation, each settlement compares its prices with those of neighbours within a day's walk (1000 world units at walking pace), on goods its commons holds. A best margin of 500 permille or more trains one adult by seeded choice (`SeedDomain::MerchantSelection`, settlement and year) among its eligible adults the allocation left unassigned, in ascending id order, so training a merchant never costs production, provided it has at least 12 eligible adults and fewer than 2 merchants. When no margin clears the threshold, its merchants retire. A merger that leaves more merchants than the bound retires the highest ids. A world with fewer than two active settlements has no merchants. All thresholds are first values for the calibration sweep (#76).
- **Retirement returns to ordinary labour.** A retired merchant becomes unassigned and the next labour allocation gives them work. The person's earlier kind of work is not stored, because that would be one more piece of hidden state; revisit if the history of a person's work matters.
- **Merchants are outside the labour pools.** The allocator skips them and they add to no cultivation, gathering, wood, stone or craft total or to construction labour. They count as unallocated workers (not assigned to settlement labour, so eligible = active + unallocated still holds), and the economy view lists them separately as Merchants.
- **Remembered prices (schema v28).** A new merchant remembers the five prices of its home settlement and of each neighbour within a day's walk, with the time they were seen. The rows are a snapshot section, last in the payload after coin, in strictly ascending (merchant, settlement, resource) order. A loaded snapshot is rejected if a row belongs to anyone but a living merchant, names an unknown settlement, falls outside the price range, or is dated in the future. Demoting a region drops the memory of the merchants it folds.
- **Acting on memory (#73, no schema change).** An existing merchant keeps or drops trading from its own remembered prices only: at the yearly review it needs current rows for its home settlement and for a neighbour within a day's walk whose remembered margin on a good the home commons holds clears the threshold. Current derived prices now gate only appointing a new merchant. A remembered row older than 500 days (`kMerchantMemoryStaleDays`, longer than a year so one review still trusts what was seen at the last) is ignored, so a merchant with only stale rows retires; a merchant on trips refreshes the memory of each settlement it reaches, so one that keeps trading keeps current rows, while one that cannot trade lasts about two reviews unless re-trained. `Simulation::merchant_observes_settlement_prices` is the single place memory is written after training (replacing that settlement's five rows with current prices and the time; trips write the same rows through the one shared writer, `remember_prices`). Merchants no longer count as construction labour. No stored field changed, so the schema stays v28. Price convergence from repeated trade is checked under #76.
- **Trade trips (#72, schema v29).** The public surface other tickets build on is small: `include/universe/trade_trip.hpp` (pure arithmetic and the `MerchantTrip` record), `WorldState::merchant_trips`, `Simulation::is_merchant_away` / `trip_for_merchant`, and two events, `EventType::TradeDeparted` (9) and `TradeCompleted` (10), both carrying `TradeEventPayload` (merchant, origin, destination, resource, cargo units, coin profit).
  - **When.** `Simulation::process_trade_period` runs once a day on the whole-day food tick, so a trip resolves on the first daily tick on or after its arrival or return time. It can never finish early, and every equivalent time-advance schedule gives the same world. There is no randomness: an idle merchant picks the best expected profit among the settlements it remembers, ties to the lowest settlement then resource id. With the food system disabled there is no daily tick and so no trade.
  - **Departure.** A merchant at home considers every remembered settlement within reach. Reach is one-way travel of at most 3 days (`trip_within_reach`; distance is the straight line rounded up, travel ticks round up), at the speed from `baseline_trade_capability()` (the single hook for speed and load; walking and a multiplier of one for now). The cargo is the least of the carrying capacity (`kBaseCargoUnits` = 50 times the multiplier), a quarter of the origin's stock of the resource, and what the origin's coin can fund. The trip is scheduled only if expected profit (remembered destination price times cargo, rounded down, minus origin price times cargo, rounded up) is at least one coin. A commons under food strain sends no food. The cargo leaves the origin commons at once.
  - **Coin.** The commons pays itself at the origin price, so no coin moves at departure; `coin_outlay` is the cargo's value at that price and the yardstick for profit, and the commons must be able to fund it. On arrival the destination pays from its commons' coin at its current price for what it can afford; any cargo it cannot pay for stays on the cart. On return the merchant's household receives 20 percent (`kMerchantProfitSharePermille`, floored) of any realised profit and the commons the rest of the coin, plus the unsold goods. A loss goes entirely to the commons. Rounding never creates or destroys coin: total coin is unchanged by any trip, which a test checks every day.
  - **Away.** A merchant on a trip keeps `Occupation::Merchant`, is excluded from construction labour, has `ActivityKind::Travel` every day, and cannot be retired by the yearly recruitment until home. After each leg the merchant remembers the five current prices of the settlement it has just reached.
  - **Abandoned trips.** If a merchant dies, is folded into a cohort by a demotion, is retired by a merger bound, or the trip would start and end in one place after a merger, `resolve_orphaned_trips` ends it immediately: the cargo (outbound) or the carried coin and unsold goods (returning) go back to the origin commons. No event is recorded for an abandoned trip. A merger remaps the absorbed settlement id on a trip to the survivor.
  - **Snapshot v29.** A trailing section after the merchant memory: a count and one 82-byte record per trip in ascending merchant order (see ARCHITECTURE). v24-v28 migrate with no trips; event type bytes 9 and 10 are rejected in files older than v29.
  - **Calibration.** Base load, the quarter-of-stock limit, the 20 percent share and the one-coin profit floor are first values for the sweep in #76. A merchant eats at home and still counts as an unassigned adult in the worker totals while away.
- **Household coin on promotion.** Demotion folds a household's coin into its settlement commons, and promotion does not hand it back. Total coin is conserved and trade is unaffected, because only a commons funds cargo. A household's coin matters only as a share of realised profit, so this is revisited when households start to spend.

## Calibration (#76)

Measured with `universe_trade_calibration` (`scripts/run-trade-calibration.ps1`; about 10 minutes for 64 seeds of both layouts, each seed run twice). 20 years, 200 people, seeds 1-64, from the single-settlement start and the default layout, stepped one day at a time (equivalent to any other schedule). Each seed runs once with trade and once with `SimulationConfig::trade_enabled = false` (no merchant is ever trained), so the comparison is paired. The constants were swept one at a time with `scripts/container-trade-sweep.sh` (each variant is a separate compile with `-DUNIVERSE_TRADE_*`, see `include/universe/trade_tuning.hpp`), 16 seeds of both layouts per variant. No production build sets those macros.

### Result: trade as built moves almost nothing, and no constant can fix that

| Measure (64 seeds, 20 years) | Single-settlement start | Default layout |
|---|---|---|
| Seeds that ever sent a trip | 21 / 64 | 48 / 64 |
| Trips departed / completed (all seeds) | 82,872 / 82,845 | 192,532 / 192,461 |
| Trips per seed, median / p90 / max | 0 / 5,110 / 9,855 | 911 / 9,860 / 14,600 |
| Most trips by one merchant (max over seeds) | 5,110 | 6,205 |
| Completed trips that sold nothing | 82,845 (100%) | 192,247 (99.89%) |
| Completed trips that made a profit | 0 | 154 (0.08%) |
| Coin that realised sales moved (all seeds) | 0 | 113,580 |
| Coin outlay of all trips (all seeds) | 20,997,388 | 47,429,015 |
| Expected profit at departure vs realised (all seeds) | +298,986,169 vs -20,997,388 | +685,017,751 vs -47,315,435 |
| Active settlements holding no coin at the end (summed over seeds) | 184 | 28 |
| Final population with trade / without (sum over seeds) | 14,535 / 14,535 | 15,482 / 15,438 |
| Seeds whose population fell below 90% of the no-trade run | 0 | 0 |
| Worst per-seed population ratio, trade / no-trade | 1.000 | 1.000 |
| Person-days under food stress, trade / no-trade (sum) | 1,627,764 / 1,623,052 | 1,349,303 / 1,365,048 |
| Mean price gap over all settlement pairs and goods, trade / no-trade | 2,516 / 2,566 permille | 2,728 / 2,774 permille |
| Peak merchants per seed, median / p90 / max | 2 / 4 / 5 | 1 / 3 / 5 |
| Widest price gap between two settlements | 15,000 permille on almost every seed | same |
| Final population per seed, median / min / max | 283 / 0 / 337 | 248 / 111 / 332 |

"Realised profit" counts a trip with no sale as a loss of its whole outlay even though the unsold goods return home, so the negative totals are the accounting of failed trips, not destroyed value; the coin and goods do come back. Ten single-settlement seeds end extinct (the non-arable starts of ADR 0002, not a trade effect).

Invariants held on all 128 runs, checked on every simulated day (not only at year ends): total coin (settlements, households and coin carried on trips) never changed; every trip's cargo and unsold goods stayed consistent and no stock went negative; each merchant had at most one trip and was alive and a merchant while away; every price stayed inside 0.25x-4x; trips completed plus trips in flight never exceeded trips departed; with trade disabled there was no merchant and no trip. The test compares repeated runs for determinism. There is no global goods ledger (production and consumption move the same stocks), so goods are checked at the trip level only.

### Why: a destination's coin runs out and the merchant cannot see it

A traced run (default layout, seed 1) shows the loop. A merchant leaves daily on a one-day round trip, carrying 50 wood from a settlement where wood is at the price floor to one where it is at the ceiling. The first trips sell and net coin. The destination had 1,000 coin and pays 960 of it in a few trips, leaving 40. From then on `units_affordable` returns zero, nothing sells, the cargo comes home, and the merchant leaves again the next day on the same remembered price. Remembered prices record what the price was, not whether the buyer could pay, and a failed sale refreshes the memory with the same high price, so the loop never ends. Coin is a conserved fixed supply and only moves where the merchant sells, so the trade drains the buyer once and then idles.

What the numbers show:

- **Trade has no measurable effect on outcomes.** Final population is identical with and without trade on the single-settlement start and within 0.3% on the default layout (it can differ only through the few real sales). No survivable seed is reduced below its no-trade outcome, so that acceptance criterion is met trivially. This is a weakness of the mechanism, not a success: trade is decided on sound prices but is not happening.
- **Price spread does not converge.** The mean pairwise gap with trade is within 2% of the no-trade gap, and the widest gap is the full 15,000 permille everywhere. The expectation that arbitrage fades as prices converge is not met.
- **The profit threshold never filters.** Prices saturate at the 4x ceiling and 0.25x floor, so the best margin is 15,000 permille on almost every seed. Thresholds of 250, 500, 1,000 and 3,000 permille give identical results.
- **Most settlements have no way to pay.** Settlements founded by fission start with no commons coin (coin is created only at world creation), and the single-settlement start ends with 184 coinless active settlements over 64 seeds. A buyer needs coin to buy, so the settlements that most need goods are the least able to take them.
- **Merchant counts are small and uneven**, 0-5 per seed; 43 single-settlement seeds never send a trip.

### Parameter sweep (16 seeds, default layout)

Each row changes one constant; the baseline is the shipped value. "With profit" counts completed trips that earned coin.

| Constant | Value | Trips departed | Trips with profit | Final population vs no-trade |
|---|---|---|---|---|
| (baseline) | 500 permille, 2 merchants, load 50, cap 25%, share 200 permille, stale 500 days | 62,349 | 39 | 4,135 / 4,135 |
| Margin threshold | 250, 1,000, 3,000 permille | 62,349 | 39 | same |
| Merchant bound | 1 | 43,229 | 39 | same |
| Merchant bound | 4 | 70,556 | 39 | same |
| Base cargo load | 25 / 100 / 200 | 62,349 | 41 / 39 / 33 | same |
| Stock cap | 10% | 53,340 | 30 | same |
| Stock cap | 50% | 62,421 | 25 | same |
| Profit share | 0 / 500 permille | 62,349 | 39 | same |
| Staleness | 200 / 1,000 days | 62,349 | 39 | same |

Readings: the threshold, profit share and staleness do nothing here; the merchant bound and the load only scale how much futile travel and cargo go back and forth; a larger stock cap does not help. In the single-settlement layout trips departed are 24,993 at baseline and unchanged by the margin, load, share and 1,000-day settings; the bound moves them to 16,117 (1) and 35,605 (4), the stock cap to 24,933 (10%) and 25,221 (50%), and 200-day staleness to 24,323. Population equals the no-trade population in every variant on both layouts, and the mean pairwise price gap stays between 2,469 and 2,483 permille against 2,512 without trade. Invariants held in every variant.

### Decision

**No constant is changed.** The sweep shows none of them changes outcomes, because the binding problem is structural (a buyer's coin, and a merchant's blindness to it), not a mis-set number. Tuning a constant to hide that would be wrong. The shipped values stay as harmless first values: the bound of two merchants and the 50-unit load cap how much futile travel there is, not how much trade. The demo scenario, its test and the snapshot schema (v29) are unchanged.

Added with the harness, none of which changes an existing world: `SimulationConfig::trade_enabled` (default true, not saved, for the no-trade comparison) and the compile-time overrides in `trade_tuning.hpp` (defaults equal the previous constants).

### Recommended follow-up (not done here)

Make trade actually move goods and coin, then recalibrate with this harness:

1. **Remember the buyer's means.** Record in the merchant's memory whether the destination could pay (or its coin) when last seen, and have `evaluate_trip` cap expected revenue at what the destination can afford. A trip that sold nothing must lower or drop the remembered price rather than refresh it, so the loop ends.
2. **Give settlements founded by fission coin.** A fission should hand the new settlement its share of the founder's commons coin (a conserved transfer) so it can buy. This changes world outcomes, so it needs a fresh look at `scenarios/demo.json`, `tests/demo_scenario_tests.cpp` and `universe_demo_seed_search`.
3. **Let coin flow both ways.** A one-way flow from buyers to sellers ends when buyers run out; sellers should spend, or trips should carry goods in both directions.
4. After 1-3, rerun the 64-seed pair and the sweep. The margin threshold only becomes selective once prices stop sitting at the ceiling and floor, which may also need a wider cover target.
