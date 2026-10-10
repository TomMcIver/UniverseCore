# UniverseCore

A deterministic, hierarchical simulation of a world that begins in a Bronze/Iron Age settlement, plus the clients that present it.

## Language

### Simulation state

**Canonical state**:
World state the simulation owns, serializes into snapshots, and reproduces deterministically from seed, configuration and event stream.
_Avoid_: engine truth, real state

**Presentation state**:
State derived deterministically from canonical state purely to display it; never serialized and never read back by the simulation.
_Avoid_: UI state (when meaning derived world positions), fake state

**Presentation position**:
Where a person appears in the world at a given moment, derived from their canonical activity and assigned buildings. It is presentation state, not canonical state.
_Avoid_: agent position, NPC location

**Building placement**:
The canonical location of a building within its settlement, fixed once decided.
_Avoid_: building position (when meaning a presentation-computed value)

**Life event**:
A recorded simulation event that names a specific person as a participant, such as their birth, death, or a change in their occupation or household.
_Avoid_: history item, log entry

**Life history**:
The ordered set of life events and memories recorded for one person. It contains only what the simulation recorded.
_Avoid_: backstory, biography

### Settlements

**Viable size**:
The smallest population a settlement can hold and still be treated as sustainable on its own. Settlements below it are candidates to be absorbed by a neighbour, and a split never leaves either side below it.
_Avoid_: minimum size, threshold (unqualified)

**Hamlet**:
A settlement below the viable size. It is a descriptive term only and is not a settlement tier.
_Avoid_: small village, outpost

### Trade

**Coin**:
A conserved integer balance held by settlement commons and households. It is created once at world creation and only ever moves; it is a currency, not a resource type.
_Avoid_: money (unqualified), gold, currency resource

**Price**:
What one unit of a resource costs in a settlement, in coin. It follows the settlement's days of cover: it rises as stock falls against need and stays between a quarter and four times its base. Derived from state, never stored.
_Avoid_: rate, value (value means the household wealth points base prices are built from)

**Days of cover**:
How many days a settlement's stock of a resource lasts at its own daily need.
_Avoid_: runway, buffer (the commons reserve is a separate food rule)

**Merchant**:
A person whose occupation is carrying goods between settlements to sell at a profit. Settlements train merchants only when trading pays.
_Avoid_: trader, caravan, NPC vendor

**Trade**:
Goods carried by a merchant from one settlement to another, paid for in coin at each settlement's own prices.
_Avoid_: exchange (which already means sharing goods within one settlement), barter

**Trip**:
One merchant's journey from their home settlement to another and back, with a cargo, taking real simulated days.
_Avoid_: route (which means the path a villager walks), run

### Presentation

**Command view**:
The overhead camera over a settlement or region, used to inspect and advance the world.
_Avoid_: map view, god view, RTS view

**Ground view**:
The third-person view in which the presenter walks the player avatar through the same settlement.
_Avoid_: first-person mode, walk mode

**Player avatar**:
The presenter-controlled character in ground view. It is presentation-only: the simulation neither knows it exists nor reacts to it.
_Avoid_: player, player character (reserved for the future canonical player entity)

**View model**:
A typed, display-ready projection of canonical state for one purpose, keyed by stable entity ID.
_Avoid_: raw snapshot, DTO

**Panel**:
A UI surface that renders one or more view models and may offer actions. Mechanics register panels without modifying the main screen.
_Avoid_: window, widget (when meaning a registered mechanic surface)

**Demo scenario**:
A committed seed and configuration from which the demo world is regenerated deterministically at runtime.
_Avoid_: save file, demo save

**Demo sequence**:
The canonical scripted path the presenter follows through the demo scenario.
_Avoid_: walkthrough, tour
