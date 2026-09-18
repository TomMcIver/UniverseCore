class_name DayNight
extends RefCounted

## Ticket #19's "day/night color grading" requirement: derives lighting
## parameters from the real simulation tick (ticks are simulation seconds;
## 86400 ticks = 1 simulated day, matching the day-boundary math already
## used server-side in web_protocol.cpp) rather than wall-clock time or a
## client-side timer -- the world's lighting should reflect where the
## simulation actually is, not how long the app has been open.

const DAY_TICKS := 86400

const NIGHT_SUN_COLOR := Color(0.25, 0.35, 0.55)
const DAY_SUN_COLOR := Color(1.0, 0.95, 0.85)
const MIN_SUN_ENERGY := 0.15
const MAX_SUN_ENERGY := 1.1
const MIN_AMBIENT_ENERGY := 0.15
const MAX_AMBIENT_ENERGY := 1.0
const NIGHT_FOG_COLOR := Color(0.20, 0.28, 0.42)
const DAY_FOG_COLOR := Color(0.78, 0.80, 0.83)

## 0.0 at midnight, 1.0 at midday, smoothly curved (not a hard on/off
## switch). Tick 0 is anchored to midday (not midnight) so a fresh
## connection -- which always starts paused at tick 0 -- opens on a bright
## scene rather than a dark one; the simulation has no inherent "time of
## day" convention for tick 0, so this is a presentation choice, not a
## data-fidelity one.
static func daylight_amount(tick: int) -> float:
	var wrapped: int = ((tick % DAY_TICKS) + DAY_TICKS) % DAY_TICKS
	var phase := float(wrapped) / float(DAY_TICKS)
	return clamp((cos(phase * TAU) + 1.0) * 0.5, 0.0, 1.0)

static func sample(tick: int) -> Dictionary:
	var daylight := daylight_amount(tick)
	return {
		"sun_color": NIGHT_SUN_COLOR.lerp(DAY_SUN_COLOR, daylight),
		"sun_energy": lerp(MIN_SUN_ENERGY, MAX_SUN_ENERGY, daylight),
		"ambient_energy": lerp(MIN_AMBIENT_ENERGY, MAX_AMBIENT_ENERGY, daylight),
		"fog_color": NIGHT_FOG_COLOR.lerp(DAY_FOG_COLOR, daylight),
	}
