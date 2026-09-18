# Asset Credits

Per [ticket #19](https://github.com/TomMcIver/UniverseCore/issues/19)'s requirement to record asset licenses actually used, even where attribution isn't legally required.

## Kenney — Fantasy Town Kit (2.0)

- Source: <https://kenney.nl/assets/fantasy-town-kit>
- License: **CC0 1.0 Universal** (Public Domain) — <http://creativecommons.org/publicdomain/zero/1.0/>
- Attribution not required. Credited anyway: "Kenney" / "www.kenney.nl".
- Files used (`assets/buildings/`): `wall-wood-door.glb` + `roof-gable.glb` (dwelling assembly), `windmill.glb` (Workshop buildings), `stall-red.glb` (FoodStore buildings), `fountain-round.glb` (settlement marker), plus the shared `Textures/colormap.png`. `roof-gable-end.glb` was copied in alongside the others but is not currently referenced by `building_assets.gd` -- left in place for a future variety pass rather than removed.
- License verified by direct fetch of the license file bundled in the downloaded pack (`License.txt`), not just the research writeup's summary.

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
