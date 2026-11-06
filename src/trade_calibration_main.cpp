// Seed sweep for trade calibration (ADR 0003, ticket #76). Slow by design: it is
// an executable, not a test. Each seed runs twice, with trade enabled and with
// trade disabled, and the paired outcome is compared. Prints one row per seed
// (unless --brief 1) and a distribution summary per layout.
#include "universe/trade_calibration.hpp"

#include <algorithm>
#include <charconv>
#include <iostream>
#include <string_view>
#include <vector>

namespace {

using universe::TradeSeedMetrics;

std::int64_t percentile(std::vector<std::int64_t> values, const std::size_t pct) {
    std::sort(values.begin(), values.end());
    return values[std::min(values.size() - 1, (values.size() * pct) / 100U)];
}

template <typename Getter>
void print_dist(const char* name, const std::vector<TradeSeedMetrics>& rows, Getter get) {
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
    std::cerr << "Usage: universe_trade_calibration [--seed-start N] [--seed-count N] [--years N] [--layout single|default] [--brief 0|1]\n";
    return 2;
}

void print_dists(const std::vector<TradeSeedMetrics>& rows) {
    print_dist("final_population", rows, [](const auto& r) { return r.final_population; });
    print_dist("final_settlements", rows, [](const auto& r) { return r.final_settlements; });
    print_dist("peak_merchants", rows, [](const auto& r) { return r.peak_merchants; });
    print_dist("merchant_years", rows, [](const auto& r) { return r.merchant_years; });
    print_dist("max_margin_permille", rows, [](const auto& r) { return r.max_margin_permille; });
    print_dist("max_price_spread_permille", rows, [](const auto& r) { return r.max_price_spread_permille; });
    print_dist("trips_departed", rows, [](const auto& r) { return r.trips_departed; });
    print_dist("trips_completed", rows, [](const auto& r) { return r.trips_completed; });
    print_dist("cargo_units_completed", rows, [](const auto& r) { return r.cargo_units_completed; });
    print_dist("realised_profit_completed", rows, [](const auto& r) { return r.realised_profit_completed; });
    print_dist("max_trips_one_merchant", rows, [](const auto& r) { return r.max_trips_one_merchant; });
    print_dist("max_trips_one_settlement", rows, [](const auto& r) { return r.max_trips_one_settlement; });
}

void print_row(const TradeSeedMetrics& m, const TradeSeedMetrics& base) {
    std::cout << "seed=" << m.seed << " pop=" << m.final_population << " (no-trade " << base.final_population
              << ") settlements=" << m.final_settlements << " merchants_peak=" << m.peak_merchants
              << " pays_years=" << m.trade_pays_settlement_years << "/" << m.settlement_years << " trips=" << m.trips_completed
              << "/" << m.trips_departed << " units=" << m.cargo_units_completed << " profit=" << m.realised_profit_completed
              << " stress_days=" << m.stress_person_days << " (no-trade " << base.stress_person_days << ")"
              << " max_spread=" << m.max_price_spread_permille << " ok=" << universe::trade_invariants_hold(m) << '\n';
}

struct PairedSummary {
    std::uint64_t seeds{};
    std::uint64_t seeds_with_trips{};
    std::uint64_t survivable{};  // seeds whose no-trade run keeps people alive
    std::uint64_t below_90pct{};
    std::uint64_t below_75pct{};
    std::int64_t worst_ratio_permille{1'000'000};
    std::int64_t ratio_sum_permille{};
    std::uint64_t trips_departed{};
    std::uint64_t trips_completed{};
    std::uint64_t trips_lost{};
    std::int64_t units{};
    std::int64_t profit{};
    std::int64_t expected_profit{};
    std::int64_t stress_trade{};
    std::int64_t stress_base{};
    std::int64_t gap_trade_permille{};
    std::int64_t gap_base_permille{};
    std::int64_t food_gap_trade_permille{};
    std::int64_t food_gap_base_permille{};
    std::uint64_t pop_trade{};
    std::uint64_t pop_base{};
    std::uint64_t profitable{};
    std::uint64_t sold_nothing{};
    std::int64_t revenue{};
    std::int64_t outlay{};
    std::uint64_t coinless_settlements{};
    std::int64_t richest_share_sum{};
};

PairedSummary summarise(const std::vector<TradeSeedMetrics>& on, const std::vector<TradeSeedMetrics>& off) {
    PairedSummary s;
    std::int64_t gap_on_sum = 0;
    std::uint64_t gap_on_n = 0;
    std::int64_t gap_off_sum = 0;
    std::uint64_t gap_off_n = 0;
    std::int64_t food_on_sum = 0;
    std::uint64_t food_on_n = 0;
    std::int64_t food_off_sum = 0;
    std::uint64_t food_off_n = 0;
    for (std::size_t i = 0; i < on.size(); ++i) {
        ++s.seeds;
        s.seeds_with_trips += on[i].trips_departed > 0 ? 1U : 0U;
        s.trips_departed += on[i].trips_departed;
        s.trips_completed += on[i].trips_completed;
        s.trips_lost += on[i].trips_lost_money;
        s.units += on[i].cargo_units_completed;
        s.profit += on[i].realised_profit_completed;
        s.expected_profit += on[i].expected_profit_departed;
        s.stress_trade += static_cast<std::int64_t>(on[i].stress_person_days);
        s.stress_base += static_cast<std::int64_t>(off[i].stress_person_days);
        s.pop_trade += on[i].final_population;
        s.pop_base += off[i].final_population;
        s.profitable += on[i].trips_profitable;
        s.sold_nothing += on[i].trips_sold_nothing;
        s.revenue += on[i].revenue_completed;
        s.outlay += on[i].outlay_completed;
        s.coinless_settlements += on[i].active_settlements_without_coin_at_end;
        s.richest_share_sum += on[i].richest_settlement_coin_share_permille;
        gap_on_sum += on[i].pair_gap_sum_permille;
        gap_on_n += on[i].pair_gap_count;
        gap_off_sum += off[i].pair_gap_sum_permille;
        gap_off_n += off[i].pair_gap_count;
        food_on_sum += on[i].food_gap_sum_permille;
        food_on_n += on[i].food_gap_count;
        food_off_sum += off[i].food_gap_sum_permille;
        food_off_n += off[i].food_gap_count;
        if (off[i].final_population == 0U) {
            continue;
        }
        ++s.survivable;
        const auto ratio = static_cast<std::int64_t>(on[i].final_population * 1000U / off[i].final_population);
        s.ratio_sum_permille += ratio;
        s.worst_ratio_permille = std::min(s.worst_ratio_permille, ratio);
        s.below_90pct += ratio < 900 ? 1U : 0U;
        s.below_75pct += ratio < 750 ? 1U : 0U;
    }
    s.gap_trade_permille = gap_on_n == 0 ? 0 : gap_on_sum / static_cast<std::int64_t>(gap_on_n);
    s.gap_base_permille = gap_off_n == 0 ? 0 : gap_off_sum / static_cast<std::int64_t>(gap_off_n);
    s.food_gap_trade_permille = food_on_n == 0 ? 0 : food_on_sum / static_cast<std::int64_t>(food_on_n);
    s.food_gap_base_permille = food_off_n == 0 ? 0 : food_off_sum / static_cast<std::int64_t>(food_off_n);
    return s;
}

void print_summary(const char* name, const PairedSummary& s, const bool all_ok) {
    std::cout << "summary layout=" << name << " seeds=" << s.seeds << " seeds_with_trips=" << s.seeds_with_trips
              << " trips_departed=" << s.trips_departed << " trips_completed=" << s.trips_completed
              << " trips_lost_money=" << s.trips_lost << " units=" << s.units << " profit=" << s.profit
              << " expected_profit=" << s.expected_profit << '\n'
              << "observed layout=" << name << " trips_with_profit=" << s.profitable << " sold_nothing=" << s.sold_nothing
              << " revenue=" << s.revenue
              << " outlay=" << s.outlay << " coinless_active_settlements_at_end=" << s.coinless_settlements
              << " mean_richest_coin_share_permille=" << (s.seeds == 0 ? 0 : s.richest_share_sum / static_cast<std::int64_t>(s.seeds))
              << '\n'
              << "paired layout=" << name << " survivable=" << s.survivable
              << " mean_pop_ratio_permille=" << (s.survivable == 0 ? 0 : s.ratio_sum_permille / static_cast<std::int64_t>(s.survivable))
              << " worst_pop_ratio_permille=" << (s.survivable == 0 ? 0 : s.worst_ratio_permille)
              << " seeds_below_90pct=" << s.below_90pct << " seeds_below_75pct=" << s.below_75pct
              << " pop_trade=" << s.pop_trade << " pop_no_trade=" << s.pop_base << " stress_days_trade=" << s.stress_trade
              << " stress_days_no_trade=" << s.stress_base << " mean_pair_gap_trade=" << s.gap_trade_permille
              << " mean_pair_gap_no_trade=" << s.gap_base_permille << " mean_food_gap_trade=" << s.food_gap_trade_permille
              << " mean_food_gap_no_trade=" << s.food_gap_base_permille << " invariants_ok=" << all_ok << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    std::uint64_t seed_start = 1;
    std::uint64_t seed_count = 64;
    std::uint64_t years = 20;
    std::uint64_t brief = 0;
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
        } else if (arg == "--brief" && parse_u64(value, brief)) {
        } else {
            return usage();
        }
    }

    bool every_ok = true;
    for (const auto layout : {universe::CalibrationLayout::SingleSettlement, universe::CalibrationLayout::DefaultLayout}) {
        if (!both_layouts && layout != only) {
            continue;
        }
        const char* name = layout == universe::CalibrationLayout::SingleSettlement ? "single" : "default";
        std::vector<TradeSeedMetrics> on;
        std::vector<TradeSeedMetrics> off;
        bool layout_ok = true;
        std::cout << "layout=" << name << " years=" << years << '\n';
        for (std::uint64_t seed = seed_start; seed < seed_start + seed_count; ++seed) {
            on.push_back(universe::run_trade_seed(layout, seed, static_cast<int>(years), true));
            off.push_back(universe::run_trade_seed(layout, seed, static_cast<int>(years), false));
            // With trade off there must be no trip at all.
            layout_ok = layout_ok && universe::trade_invariants_hold(on.back()) && universe::trade_invariants_hold(off.back()) &&
                off.back().trips_departed == 0U && off.back().peak_merchants == 0U;
            if (brief == 0U) {
                print_row(on.back(), off.back());
            }
        }
        every_ok = every_ok && layout_ok;
        print_summary(name, summarise(on, off), layout_ok);
        if (brief == 0U) {
            print_dists(on);
        }
    }
    std::cout << (every_ok ? "invariants held on every seed\n" : "INVARIANT FAILURES\n");
    return every_ok ? 0 : 1;
}
