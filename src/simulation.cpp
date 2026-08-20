#include "universe/simulation.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace universe {

namespace {
constexpr std::uint32_t kRateScale = 10'000;
}

Simulation::DeterministicRng::DeterministicRng(const Seed seed) noexcept
    : state_(seed) {}

std::uint64_t Simulation::DeterministicRng::next_u64() noexcept {
    // SplitMix64: compact, fast, and stable across platforms because we avoid
    // implementation-defined standard-library probability distributions.
    std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ULL);
    z = (z ^ (z >> 30U)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27U)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31U);
}

bool Simulation::DeterministicRng::chance_per_10k(const std::uint32_t threshold) noexcept {
    if (threshold == 0U) {
        return false;
    }
    if (threshold >= kRateScale) {
        return true;
    }

    return (next_u64() % kRateScale) < threshold;
}

Simulation::Simulation(SimulationConfig config)
    : config_(config),
      state_{.seed = config.seed, .year = 0, .population = config.initial_population},
      rng_(config.seed) {
    if (config.birth_rate_per_10k > kRateScale || config.death_rate_per_10k > kRateScale) {
        throw std::invalid_argument("Rates must be between 0 and 10,000 inclusive");
    }
}

const WorldState& Simulation::state() const noexcept {
    return state_;
}

std::uint64_t Simulation::sample_events(const std::uint64_t trials, const std::uint32_t rate_per_10k) {
    std::uint64_t events = 0;
    for (std::uint64_t i = 0; i < trials; ++i) {
        if (rng_.chance_per_10k(rate_per_10k)) {
            ++events;
        }
    }
    return events;
}

void Simulation::step_year() {
    const auto starting_population = state_.population;

    const auto births = sample_events(starting_population, config_.birth_rate_per_10k);
    const auto deaths = std::min(
        sample_events(starting_population, config_.death_rate_per_10k),
        starting_population + births);

    state_.population = starting_population + births - deaths;
    state_.total_births += births;
    state_.total_deaths += deaths;
    ++state_.year;
}

void Simulation::run_years(const std::uint64_t years) {
    for (std::uint64_t i = 0; i < years; ++i) {
        step_year();
    }
}

}  // namespace universe
