# Demo sequence: live-client evidence

Screenshots of the demo sequence (#41, #44) from the live Godot client, taken by the client itself (`scripts/tools/demo_capture.gd` saves only its own viewport). The world is the committed demo scenario (`scenarios/demo.json`: seed 39, 200 people, single-settlement start).

| Beat | Screenshot | What it shows |
| --- | --- | --- |
| Command view above a village | [1_command_view_year0.png](1_command_view_year0.png) | Year 0, 200 people in one village, speed controls and the "Jump 20 years" button |
| Inspect its economy | [2_economy_year0.png](2_economy_year0.png) | Economy panel: 200 people in 63 households, jobs, household wealth 80/80/80, no buildings yet |
| Advance time | [3_command_view_year20.png](3_command_view_year20.png) | After the 20-year jump the camera backs off to frame all 5 settlements: the original village plus hamlets linked by tracks; population 332; the event log lists recorded events |
| Visible change | [4_economy_year20.png](4_economy_year20.png) | Same panel at year 20: 8 buildings complete, wealth 140/560/1,344, inequality 0.66, totals with units |
| Descend to the ground view | [5_ground_view_beside_demo_npc.png](5_ground_view_beside_demo_npc.png) | Third-person view among animated villagers, with the yellow marker over the demo NPC (the living person with the most life events) |
| Inspect an NPC's life history | [6_demo_npc_life_history.png](6_demo_npc_life_history.png) | Villager 136's recorded events (children, who they taught and were taught by) next to their inspector panel; nothing is invented |

## Reproduce

Podman and Godot 4.7 are required. Use a port that is free, and build the bridge first (`scripts/container-build.sh`).

```powershell
$env:UNIVERSE_WS_PORT = "19181"; $env:UNIVERSE_SCENARIO = "scenarios/demo.json"
node apps/web-client/server/ws-bridge.mjs          # terminal 1: the demo world
$env:UNIVERSECORE_DEMO_CAPTURE_DIR = "<existing folder>"
Godot_v4.7.2-stable_win64_console.exe --path apps/godot-client   # terminal 2: runs the sequence, saves the PNGs, quits
```

The capture prints `DEMO_CAPTURE years=0..20 settlements=5 people_shown=332 ...`; those numbers match the headless seed search for seed 39. Restart the bridge between runs, because the world keeps its year.

## Assets

Every asset used, with its licence, is listed in [apps/godot-client/assets/CREDITS.md](../../apps/godot-client/assets/CREDITS.md).

## Known gaps

- The event log shows the most recent events, which at year 20 are mostly annual job changes ("Villager 221 became a farmer"). Births and new settlements scroll past earlier.
- The economy totals are cumulative since the start, not per year.
- Villagers gather around the common in the afternoon, so the village centre is crowded. The spacing is set by `GATHER_SPACING` in `world_view.gd`.
