class_name DayNight
extends RefCounted

## Lighting derived from the real simulation tick (ticks are simulation
## seconds; 86400 ticks = 1 simulated day, matching web_protocol.cpp), never
## from wall-clock or frame time.
##
## Ticket #55 (per the #48 decision): daytime uses prototype variant A,
## "clear afternoon" -- warm white sun, blue sky, neutral ambient colour (a
## sky-sourced ambient tinted every green toward cyan and fought the Kenney
## palette). The simulation has no time-of-day convention for tick 0 and
## currently advances in whole days, so tick 0 is anchored to mid-afternoon
## (15:00): every day-aligned snapshot opens on the demo's afternoon light,
## and lighting will follow automatically once the simulation resolves
## time within a day.

const DAY_TICKS := 86400
const HOUR_TICKS := 3600
const ANCHOR_HOUR := 15.0

const NIGHT_SUN_COLOR := Color(0.25, 0.35, 0.55)
const DAY_SUN_COLOR := Color(1.0, 0.95, 0.86)
const DUSK_SUN_COLOR := Color(1.0, 0.72, 0.5)
const MIN_SUN_ENERGY := 0.08
const MAX_SUN_ENERGY := 1.15
const MIN_AMBIENT_ENERGY := 0.18
const MAX_AMBIENT_ENERGY := 0.7
const DAY_AMBIENT_COLOR := Color(0.88, 0.88, 0.86)
const NIGHT_AMBIENT_COLOR := Color(0.35, 0.42, 0.6)
const DAY_SKY_TOP := Color(0.28, 0.5, 0.82)
const DAY_SKY_HORIZON := Color(0.7, 0.82, 0.92)
const NIGHT_SKY_TOP := Color(0.03, 0.05, 0.12)
const NIGHT_SKY_HORIZON := Color(0.12, 0.16, 0.26)
## Below the horizon the sky shows a distant-grass tone, never a brown void.
const DAY_SKY_GROUND := Color(0.42, 0.52, 0.38)
const NIGHT_SKY_GROUND := Color(0.08, 0.1, 0.1)
const DAY_FOG_COLOR := Color(0.72, 0.8, 0.88)
const NIGHT_FOG_COLOR := Color(0.1, 0.13, 0.2)
const SUN_AZIMUTH_DEGREES := -35.0
const MAX_SUN_ELEVATION_DEGREES := 62.0

## Hour of day (0..24) at `tick`.
static func hour_of_day(tick: int) -> float:
	var wrapped: int = ((tick % DAY_TICKS) + DAY_TICKS) % DAY_TICKS
	return fmod(ANCHOR_HOUR + float(wrapped) / float(HOUR_TICKS), 24.0)

## Ticks, from tick 0, until the given hour of the first day.
static func tick_at_hour(hour: float) -> int:
	return int(round(fmod(hour - ANCHOR_HOUR + 24.0, 24.0) * float(HOUR_TICKS)))

## 0.0 at midnight, 1.0 at midday, smoothly curved.
static func daylight_amount(tick: int) -> float:
	var phase := (hour_of_day(tick) - 12.0) / 24.0
	return clamp((cos(phase * TAU) + 1.0) * 0.5, 0.0, 1.0)

static func sample(tick: int) -> Dictionary:
	var daylight := daylight_amount(tick)
	var hour := hour_of_day(tick)
	var low_sun := 1.0 - smoothstep(0.35, 0.75, daylight)
	var sun_color := NIGHT_SUN_COLOR.lerp(DAY_SUN_COLOR, smoothstep(0.1, 0.5, daylight)).lerp(DUSK_SUN_COLOR, low_sun * smoothstep(0.1, 0.3, daylight))
	var elevation: float = lerp(-10.0, MAX_SUN_ELEVATION_DEGREES, daylight)
	var azimuth := SUN_AZIMUTH_DEGREES + (hour - 12.0) * 12.0
	return {
		"sun_color": sun_color,
		"sun_energy": lerp(MIN_SUN_ENERGY, MAX_SUN_ENERGY, smoothstep(0.1, 0.6, daylight)),
		"sun_rotation_degrees": Vector3(-elevation, azimuth, 0.0),
		"ambient_color": NIGHT_AMBIENT_COLOR.lerp(DAY_AMBIENT_COLOR, daylight),
		"ambient_energy": lerp(MIN_AMBIENT_ENERGY, MAX_AMBIENT_ENERGY, daylight),
		"sky_top": NIGHT_SKY_TOP.lerp(DAY_SKY_TOP, daylight),
		"sky_horizon": NIGHT_SKY_HORIZON.lerp(DAY_SKY_HORIZON, daylight).lerp(DUSK_SUN_COLOR, low_sun * daylight * 0.4),
		"sky_ground": NIGHT_SKY_GROUND.lerp(DAY_SKY_GROUND, daylight),
		"fog_color": NIGHT_FOG_COLOR.lerp(DAY_FOG_COLOR, daylight),
	}
