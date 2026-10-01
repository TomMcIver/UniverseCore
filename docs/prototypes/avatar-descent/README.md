# Player avatar and descent (ticket #57)

Live client on `feat/avatar-descent`, world at year 8, captured by the
env-gated scripted run (`UNIVERSECORE_DESCENT_CAPTURE_DIR`).

1. `1_command_view.png` — command view with the Descend button.
2. `2_mid_descent.png` — halfway through the 1.4 s eased flight (button disabled).
3. `3_ground_walking.png` — avatar walking toward a crowd; only the aimed villager shows its activity label.
4. `4_villager_selected.png` — E selected villager 59 through the panel registry; the inspector shows the same entity ID the command view uses.
5. `5_ascended_to_command_view.png` — back over the avatar's position.

Controls: G or the button descends/ascends; WASD moves relative to the
camera, Shift sprints, right-mouse drag looks, wheel sets arm length; E
selects the villager under the aim (nearest within 3.5 m as fallback).

Buildings are construction sites because nothing completes within the
first 20 years yet (#58).
