// Seed sweep for trade calibration (ADR 0003, ticket #76). Slow by design: it is
// an executable, not a test. Prints one row per seed and a distribution summary.
#include "universe/trade_calibration.hpp"

#include <algorithm>
#include <charconv>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

std::int64_t percentile(std::vector<std::int64_t> values, const std::size_t pct) {
    std::sort(values.begin(), values.end());
    return values[std::min(values.size() - 1, (values.size() * pct) / 100U)];
}

template <typename Getter>
void print_dist(const char* name, const std::vector<universe::TradeSeedMetrics>& rows, Getter get) {
    std::vector<std::int64_t> values;
    for (const auto& row : rows) {
        values.push_back(static_cast<std::int64_t>(get(row)));
    }
    std::cout << "  " << name << " min=" << percentile(values, 0) << " p25=" << percentile(values, 25)
              << " median=" << percentile(values, 50) << " p75=" << percentile(values, 75)
              << " p90=" << percentile(values, 90) << " max=" << *std::max_element(values.begin(), values.end()) << '\n';
}

bool parse_u64(const std::string_view text, std::uint64_t& out) {
    const auto r = std::from_chars(text.data(), text.data() + text.size(), out);
    return r.ec == std::errc{} && r.ptr == text.data() + text.size();
}

int usage() {
    std::cerr << "Usage: universe_trade_calibration [--seed-start N] [--seed-count N] [--years N] [--layout single|default]\n";
    return 2;
}

void print_dists(const std::vector<universe::TradeSeedMetrics>& rows) {
    print_dist("final_population", rows, [](const auto& r) { return r.final_population; });
    print_dist("final_settlements", rows, [](const auto& r) { return r.final_settlements; });
    print_dist("peak_merchants", rows, [](const auto& r) { return r.peak_merchants; });
    print_dist("merchant_years", rows, [](const auto& r) { return r.merchant_years; });
    print_dist("max_margin_permille", rows, [](const auto& r) { return r.max_margin_permille; });
    print_dist("max_price_spread_permille", rows, [](const auto& r) { return r.max_price_spread_permille; });
    print_dist("exchange_volume", rows, [](const auto& r) { return r.total_exchange_volume; });
}

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t seed_start = 1;
    std::uint64_t seed_count = 64;
    std::uint64_t years = 20;
    bool both_layouts = true;
    auto only = universe::CalibrationLayout::SingleSettlement;
    if (argc % 2 == 0) {
        return usage();
    }
    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string_view arg{argv[i]};
        const std::string_view value{argv[i + 1]};
        if (arg == "--layout") {
            both_layouts = false;
            only = value == "default" ? universe::CalibrationLayout::DefaultLayout : universe::CalibrationLayout::SingleSettlement;
        } else if (arg == "--seed-start" && parse_u64(value, seed_start)) {
        } else if (arg == "--seed-count" && parse_u64(value, seed_count) && seed_count > 0) {
        } else if (arg == "--years" && parse_u64(value, years)) {
        } else {
            return usage();
        }
    }

    int failures = 0;
    for (const auto layout : {universe::CalibrationLayout::SingleSettlement, universe::CalibrationLayout::DefaultLayout}) {
        if (!both_layouts && layout != only) {
            continue;
        }
        const char* name = layout == universe::CalibrationLayout::SingleSettlement ? "single" : "default";
        std::vector<universe::TradeSeedMetrics> rows;
        std::cout << "layout=" << name << " years=" << years << '\n';
        for (std::uint64_t seed = seed_start; seed < seed_start + seed_count; ++seed) {
            const auto m = universe::run_trade_seed(layout, seed, static_cast<int>(years));
            rows.push_back(m);
            failures += (m.coin_conserved && m.prices_in_range) ? 0 : 1;
            std::cout << "seed=" << m.seed << " pop=" << m.final_population << " settlements=" << m.final_settlements
                      << " merchants_final=" << m.final_merchants << " merchants_peak=" << m.peak_merchants
                      << " pays_years=" << m.trade_pays_settlement_years << "/" << m.settlement_years
                      << " max_margin=" << m.max_margin_permille << " max_spread=" << m.max_price_spread_permille
                      << " price_range=" << m.min_price_millicoin << "-" << m.max_price_millicoin
                      << " coin_ok=" << m.coin_conserved << " prices_ok=" << m.prices_in_range << '\n';
        }
        std::uint64_t with_merchants = 0;
        std::uint64_t pays = 0;
        std::uint64_t settlement_years = 0;
        for (const auto& m : rows) {
            with_merchants += m.peak_merchants > 0 ? 1U : 0U;
            pays += m.trade_pays_settlement_years;
            settlement_years += m.settlement_years;
        }
        std::cout << "summary layout=" << name << " seeds=" << rows.size() << " seeds_with_merchants=" << with_merchants
                  << " trade_pays_settlement_years=" << pays << "/" << settlement_years << '\n';
        print_dists(rows);
    }
    std::cout << (failures == 0 ? "invariants held on every seed\n" : "INVARIANT FAILURES\n");
    return failures == 0 ? 0 : 1;
}
