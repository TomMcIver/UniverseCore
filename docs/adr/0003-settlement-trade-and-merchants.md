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
- **Household coin on promotion.** Demotion folds a household's coin into its settlement commons, and promotion does not hand it back. Total coin is conserved and trade is unaffected, because only a commons funds cargo. A household's coin matters only as a share of realised profit, so this is revisited when households start to spend.
