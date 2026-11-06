class_name StreamPacer
extends RefCounted

## Shares a small per-frame time budget between the world's background
## streaming jobs (terrain tiles, vegetation chunks, village dressing rebuilds).
##
## Each job is atomic and costs 8-50 ms, and the queues refill whenever the world
## changes (a 20-year jump founds settlements and re-lays fields). Run at full
## speed they stack on top of the message handling and the agents' walking and
## turn every frame of a jump into a 50 ms frame. While the frame rate is healthy
## the pacer never says no, so loading and normal play are unchanged. When frames
## run long it spends a credit of BUDGET_MS per frame: a job may start only while
## credit remains, and its measured cost is charged afterwards, so the average
## streaming cost stays near BUDGET_MS however large a single job is. The jobs
## are only deferred, never dropped, so the world catches up once frames recover.

## Smoothed frame time above which the pacer starts limiting jobs, and the lower
## one at which it stops (hysteresis, so it does not flap around one value).
const PRESSURE_ENTER_MS := 25.0
const PRESSURE_EXIT_MS := 20.0
## Credit gained per frame under pressure, and the most that can be banked.
const BUDGET_MS := 6.0
const MAX_CREDIT_MS := 12.0
const SMOOTHING := 0.3

var _frame_ms := 0.0
var _pressured := false
var _credit_us := 0

## Call once per frame with the frame's delta in seconds, before any job runs.
func begin_frame(delta_seconds: float) -> void:
	_frame_ms = lerpf(_frame_ms, delta_seconds * 1000.0, SMOOTHING)
	if _pressured and _frame_ms < PRESSURE_EXIT_MS:
		_pressured = false
	elif not _pressured and _frame_ms > PRESSURE_ENTER_MS:
		_pressured = true
		_credit_us = 0
	if _pressured:
		_credit_us = mini(_credit_us + int(BUDGET_MS * 1000.0), int(MAX_CREDIT_MS * 1000.0))

func is_under_pressure() -> bool:
	return _pressured

func smoothed_frame_ms() -> float:
	return _frame_ms

## Whether a job may start now.
func can_run() -> bool:
	return not _pressured or _credit_us > 0

## Charges a finished job's cost; only counts while under pressure.
func charge(cost_usec: int) -> void:
	if _pressured:
		_credit_us -= cost_usec
