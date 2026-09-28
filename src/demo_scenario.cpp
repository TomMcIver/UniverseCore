#include "universe/demo_scenario.hpp"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <sstream>
#include <unordered_map>
#include <utility>

namespace universe {
namespace {

constexpr std::int64_t kPointsPerBuilding = 10;
constexpr std::uint64_t kScoredBuildingCap = 20;
constexpr std::int64_t kGrowthPercentCap = 100;
constexpr std::int64_t kDeclinePenaltyMultiplier = 2;
constexpr std::int64_t kPointsPerSettlement = 50;
constexpr std::uint64_t kScoredSettlementCap = 2;
constexpr std::int64_t kPointsPerDiscovery = 30;
constexpr std::uint64_t kScoredDiscoveryCap = 3;
constexpr std::int64_t kPointsPerInstitution = 20;
constexpr std::uint64_t kScoredInstitutionCap = 2;
constexpr std::int64_t kPointsPerTierPromotion = 60;
constexpr std::uint64_t kScoredTierPromotionCap = 2;
constexpr std::int64_t kPointsPerMerge = 40;
constexpr std::uint64_t kScoredMergeCap = 2;
constexpr std::size_t kMaxScenarioBytes = 64U * 1024U;

std::int64_t capped_points(const std::uint64_t count, const std::uint64_t cap, const std::int64_t points_each) {
    return static_cast<std::int64_t>(std::min(count, cap)) * points_each;
}

std::int64_t population_change_points(const std::uint64_t initial, const std::uint64_t final_population) {
    if (initial == 0U) {
        return 0;
    }
    const auto delta = static_cast<std::int64_t>(final_population) - static_cast<std::int64_t>(initial);
    const auto percent = (delta * 100) / static_cast<std::int64_t>(initial);
    if (percent >= 0) {
        return std::min(percent, kGrowthPercentCap);
    }
    return percent * kDeclinePenaltyMultiplier;
}

std::size_t count_occurrences(const std::string_view text, const std::string_view needle) {
    std::size_t count = 0;
    for (auto pos = text.find(needle); pos != std::string_view::npos; pos = text.find(needle, pos + needle.size())) {
        ++count;
    }
    return count;
}

std::optional<std::uint64_t> read_unsigned_field(const std::string_view json, const std::string_view key, std::string& error) {
    const auto quoted = std::string{"\""} + std::string{key} + "\"";
    if (count_occurrences(json, quoted) != 1U) {
        error = "scenario must contain exactly one \"" + std::string{key} + "\" field";
        return std::nullopt;
    }

    auto pos = json.find(quoted) + quoted.size();
    const auto skip_space = [&] {
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\n' || json[pos] == '\r')) {
            ++pos;
        }
    };
    skip_space();
    if (pos >= json.size() || json[pos] != ':') {
        error = "expected ':' after \"" + std::string{key} + "\"";
        return std::nullopt;
    }
    ++pos;
    skip_space();

    std::uint64_t value = 0;
    const auto* begin = json.data() + pos;
    const auto* end = json.data() + json.size();
    const auto result = std::from_chars(begin, end, value);
    if (result.ec != std::errc{} || result.ptr == begin) {
        error = "\"" + std::string{key} + "\" must be a non-negative integer";
        return std::nullopt;
    }
    return value;
}

}  // namespace

DemoOutcome measure_demo_outcome(const WorldState& initial_state, const WorldState& final_state) {
    const auto window_start = initial_state.time;
    DemoOutcome outcome{
        .initial_population = initial_state.population,
        .final_population = final_state.population,
    };

    std::unordered_map<SettlementId, SettlementTier> initial_tiers;
    initial_tiers.reserve(initial_state.settlements.size());
    for (const auto& settlement : initial_state.settlements) {
        initial_tiers.emplace(settlement.id, settlement.tier);
    }
    for (const auto& settlement : final_state.settlements) {
        if (!settlement.active) {
            continue;
        }
        const auto it = initial_tiers.find(settlement.id);
        const auto start_tier = it == initial_tiers.end() ? SettlementTier::Village : it->second;
        if (static_cast<std::uint8_t>(settlement.tier) > static_cast<std::uint8_t>(start_tier)) {
            ++outcome.tier_promotions;
        }
    }

    for (const auto& building : final_state.buildings) {
        if (building.state == ConstructionState::Complete && building.completed_at.has_value() &&
            *building.completed_at > window_start) {
            ++outcome.buildings_completed;
        }
    }

    for (const auto& event : final_state.event_log) {
        if (event.timestamp <= window_start) {
            continue;
        }
        switch (event.type) {
            case EventType::SettlementFounded:
                ++outcome.settlements_founded;
                break;
            case EventType::DiscoveryMade:
                ++outcome.discoveries_made;
                break;
            case EventType::InstitutionFounded:
                ++outcome.institutions_founded;
                break;
            case EventType::SettlementMerged:
                ++outcome.settlements_merged;
                break;
            default:
                break;
        }
    }

    return outcome;
}

DemoCandidateScore score_demo_outcome(const std::uint64_t seed, const DemoOutcome& outcome) {
    const auto score = capped_points(outcome.buildings_completed, kScoredBuildingCap, kPointsPerBuilding) +
                       population_change_points(outcome.initial_population, outcome.final_population) +
                       capped_points(outcome.settlements_founded, kScoredSettlementCap, kPointsPerSettlement) +
                       capped_points(outcome.discoveries_made, kScoredDiscoveryCap, kPointsPerDiscovery) +
                       capped_points(outcome.institutions_founded, kScoredInstitutionCap, kPointsPerInstitution) +
                       capped_points(outcome.tier_promotions, kScoredTierPromotionCap, kPointsPerTierPromotion) +
                       capped_points(outcome.settlements_merged, kScoredMergeCap, kPointsPerMerge);

    return DemoCandidateScore{
        .seed = seed,
        .outcome = outcome,
        .eligible = outcome.initial_population > 0U && outcome.final_population > 0U && outcome.buildings_completed > 0U,
        .score = score,
    };
}

DemoCandidateScore run_demo_candidate(const std::uint64_t seed,
                                      const std::uint64_t initial_population,
                                      const std::uint64_t years) {
    Simulation simulation{SimulationConfig{.seed = seed, .initial_population = initial_population}};
    const auto initial_state = simulation.state();
    simulation.run_years(years);
    return score_demo_outcome(seed, measure_demo_outcome(initial_state, simulation.state()));
}

std::vector<DemoCandidateScore> rank_demo_candidates(std::vector<DemoCandidateScore> candidates) {
    std::ranges::sort(candidates, [](const DemoCandidateScore& lhs, const DemoCandidateScore& rhs) {
        if (lhs.eligible != rhs.eligible) {
            return lhs.eligible;
        }
        if (lhs.score != rhs.score) {
            return lhs.score > rhs.score;
        }
        return lhs.seed < rhs.seed;
    });
    return candidates;
}

std::optional<DemoScenario> parse_demo_scenario(const std::string_view json, std::string& error) {
    if (json.size() > kMaxScenarioBytes) {
        error = "scenario file is too large";
        return std::nullopt;
    }
    const auto first = json.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos || json[first] != '{') {
        error = "scenario must be a JSON object";
        return std::nullopt;
    }

    const auto seed = read_unsigned_field(json, "seed", error);
    if (!seed.has_value()) {
        return std::nullopt;
    }
    const auto population = read_unsigned_field(json, "initial_population", error);
    if (!population.has_value()) {
        return std::nullopt;
    }
    const auto years = read_unsigned_field(json, "years", error);
    if (!years.has_value()) {
        return std::nullopt;
    }

    if (*population == 0U || *population > kDemoScenarioMaxPopulation) {
        error = "initial_population is out of range";
        return std::nullopt;
    }
    if (*years == 0U || *years > kDemoScenarioMaxYears) {
        error = "years is out of range";
        return std::nullopt;
    }

    return DemoScenario{.seed = *seed, .initial_population = *population, .years = *years};
}

std::string format_demo_scenario_json(const DemoScenario& scenario, const DemoCandidateScore& picked_by) {
    const auto& outcome = picked_by.outcome;
    std::ostringstream out;
    out << "{\n"
        << "  \"seed\": " << scenario.seed << ",\n"
        << "  \"initial_population\": " << scenario.initial_population << ",\n"
        << "  \"years\": " << scenario.years << ",\n"
        << "  \"picked_by\": {\n"
        << "    \"score\": " << picked_by.score << ",\n"
        << "    \"final_population\": " << outcome.final_population << ",\n"
        << "    \"buildings_completed\": " << outcome.buildings_completed << ",\n"
        << "    \"settlements_founded\": " << outcome.settlements_founded << ",\n"
        << "    \"discoveries_made\": " << outcome.discoveries_made << ",\n"
        << "    \"institutions_founded\": " << outcome.institutions_founded << ",\n"
        << "    \"tier_promotions\": " << outcome.tier_promotions << ",\n"
        << "    \"settlements_merged\": " << outcome.settlements_merged << "\n"
        << "  }\n"
        << "}\n";
    return out.str();
}

}  // namespace universe
