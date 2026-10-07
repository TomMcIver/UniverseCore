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

- **Where coin comes from.** Proposed default: a seeded endowment at world creation proportional to each settlement's starting stock value, conserved afterwards (no minting), so total coin never changes. This needs the owner's confirmation.
- The profit-share fraction, the base cargo load, the profit threshold for training a merchant, the merchant count bound, and the price range. These get a measured sweep when built, the same way the settlement size distribution was calibrated, and any change is recorded here.
- The capability function for speed and load: for now it returns the walking baseline and a multiplier of one.

## Consequences

A canonical state change, so snapshot schema impact must be evaluated and versioned, with migration from the current version. Every world-critical piece needs deterministic tests: pricing, merchant selection, trip scheduling, coin conservation (total coin unchanged by any trade) and save/load continuation mid-trip. Time acceleration must not let trips resolve for free: a trip takes its real simulated days.
