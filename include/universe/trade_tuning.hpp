// Defaults for the tunable trade constants (ADR 0003, ticket #76).
//
// Each value can be overridden at compile time with a -D flag so the calibration
// sweep (scripts/trade-parameter-sweep.sh) can measure alternatives without a
// runtime global. The shipped values are the defaults below; no production build
// sets these macros.
#pragma once

#ifndef UNIVERSE_TRADE_MARGIN_THRESHOLD_PERMILLE
#define UNIVERSE_TRADE_MARGIN_THRESHOLD_PERMILLE 500
#endif
#ifndef UNIVERSE_TRADE_MAX_MERCHANTS_PER_SETTLEMENT
#define UNIVERSE_TRADE_MAX_MERCHANTS_PER_SETTLEMENT 2
#endif
#ifndef UNIVERSE_TRADE_BASE_CARGO_UNITS
#define UNIVERSE_TRADE_BASE_CARGO_UNITS 50
#endif
#ifndef UNIVERSE_TRADE_STOCK_CAP_PERCENT
#define UNIVERSE_TRADE_STOCK_CAP_PERCENT 25
#endif
#ifndef UNIVERSE_TRADE_PROFIT_SHARE_PERMILLE
#define UNIVERSE_TRADE_PROFIT_SHARE_PERMILLE 200
#endif
#ifndef UNIVERSE_TRADE_STALE_DAYS
#define UNIVERSE_TRADE_STALE_DAYS 500
#endif
