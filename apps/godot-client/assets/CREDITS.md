# Asset Credits

Per [ticket #19](https://github.com/TomMcIver/UniverseCore/issues/19)'s requirement to record asset licenses actually used, even where attribution isn't legally required.

## Kenney — Fantasy Town Kit (2.0)

- Source: <https://kenney.nl/assets/fantasy-town-kit>
- License: **CC0 1.0 Universal** (Public Domain) — <http://creativecommons.org/publicdomain/zero/1.0/>
- Attribution not required. Credited anyway: "Kenney" / "www.kenney.nl".
- Files used (`assets/buildings/`): `wall-wood.glb` x3 + `wall-wood-door.glb` x1 (rotated into a real enclosed four-wall box, not a single facade) + `roof-gable.glb` for Dwelling and Workshop buildings; `watermill.glb` additionally mounted on Workshop's side wall as a mill wheel; `stall-red.glb` for FoodStore buildings; `fountain-round.glb` for the settlement marker; plus the shared `Textures/colormap.png`. `roof-gable-end.glb` was copied in alongside the others but is not currently referenced by `building_assets.gd` -- left in place for a future variety pass rather than removed. `windmill.glb` was tried and removed: it's just the rotating blade/sail mechanism with no supporting structure of its own, and doesn't read as a building by itself -- caught by rendering it in isolation (`scripts/tests/debug_building.gd`), not assumed from the filename. `watermill.glb` has the same "no supporting structure" characteristic (and the same center-pivot quirk, corrected in code) but works as a wheel mounted on a real wall, unlike the windmill blades.
- License verified by direct fetch of the license file bundled in the downloaded pack (`License.txt`), not just the research writeup's summary.
- **Ticket #55 (demo village look):** the live client now uses only the Bronze/Iron Age-plausible pieces:
  - `wall-wood.glb`, `wall-wood-door.glb` and `wall-wood-window-shutters.glb` (wooden shutters, no glazing);
  - `roof-gable.glb` and `roof-high-gable.glb`;
  - `cart.glb`.
  `wall-wood-window-shutters.glb`, `roof-high-gable.glb` and `cart.glb` came from the same pack and licence, via the #48 prototype. `watermill.glb`, `stall-red.glb` and `fountain-round.glb` are no longer referenced by the live client, and the settlement centre is a firepit built from Nature Kit rocks and stumps. None of these models ship collision; the live client adds simplified box collision in `building_assets.gd`.

## Kenney — Nature Kit (2.1)

- Source: <https://kenney.nl/assets/nature-kit>
- License: **CC0 1.0 Universal** (Public Domain), verified against the bundled `License.txt` when the #48 prototype downloaded the pack.
- Files used (`assets/nature/`, from `Models/GLTF format/`, self-contained materials, no textures):
  - trees: `tree_oak.glb`, `tree_default.glb`, `tree_fat.glb`, `tree_pineRoundA.glb`, `tree_pineTallA.glb`;
  - ground cover: `plant_bushLarge.glb`, `grass.glb`, `flower_redA.glb`, `flower_yellowA.glb`, `flower_purpleA.glb`;
  - rocks and stumps: `rock_largeA.glb`, `rock_smallA.glb`, `stump_round.glb`;
  - fields: `crops_wheatStageB.glb`, `fence_simple.glb`.
- Deliberately not used: `crops_cornStageB.glb` (maize is a New World crop, wrong for a Bronze/Iron Age setting).
- Runtime adjustments in `scripts/world/foliage.gd`; the source files are unedited:
  - mint/teal greens are hue-shifted toward the Town Kit's leaf green;
  - wheat is tinted golden;
  - foliage normals are replaced with straight up, which fixes blades rendering black.
- None ship collision. The vegetation scatter adds cylinder collision to standing trees only.

## Kenney — Castle Kit (2.0)

- Source: <https://kenney.nl/assets/castle-kit>
- License: **CC0 1.0 Universal** (Public Domain) — same terms as above.
- Downloaded and verified this session but **not currently used** by any script — kept available (`Models/GLB format/` under the original download, not copied into `assets/`) for a future pass if more building/wall variety is wanted. Only the Fantasy Town Kit pieces above are actually wired into `building_assets.gd`.

## Kenney — Blocky Characters (2.0)

- Source: <https://kenney.nl/assets/blocky-characters>
- License: **CC0 1.0 Universal** (Public Domain) — same terms as above.
- Files used (`assets/characters/`): `character-a.glb` (agents where `sex == "Female"`), `character-b.glb` (all other agents), plus `Textures/texture-a.png` and `Textures/texture-b.png`.
- Explicitly accepted as v1-quality per spec #14 — a stylized blocky humanoid, not a Bannerlord/Manor-Lords-tier character model. The character-rig/fidelity gap the asset-survey research (docs/research/free-medieval-assets-procedural-terrain.md) flagged for Sketchfab-sourced options is sidestepped entirely by using Kenney's own CC0 characters instead, at the cost of a more cartoonish "blocky" look than a photorealistic peasant would be.

## Not used this ticket

- **Quaternius Medieval Village MegaKit** (the asset survey's primary recommendation) — its actual host is `quaternius.itch.io`, which failed with a TLS/schannel negotiation error from this environment on every attempt (`SEC_E_INVALID_TOKEN`), the same class of fetch failure the original research hit with OpenGameArt.org. Kenney's Fantasy Town Kit and Blocky Characters (both directly fetchable, both CC0) were used instead. Revisit if a different network path to itch.io becomes available.
