#include "universe/simulation.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <unordered_map>
#include <optional>
#include <unordered_set>
#include <variant>

namespace {

void require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

const universe::Person* find_person(const universe::WorldState& state, const universe::PersonId id) {
    for (const auto& person : state.people) {
        if (person.id == id) {
            return &person;
        }
    }
    return nullptr;
}

const universe::Household* find_household(const universe::WorldState& state, const universe::HouseholdId id) {
    for (const auto& household : state.households) {
        if (household.id == id) {
            return &household;
        }
    }
    return nullptr;
}

const universe::SimulationEvent* find_event(const universe::WorldState& state, const universe::EventId id) {
    for (const auto& event : state.event_log) {
        if (event.id == id) {
            return &event;
        }
    }
    return nullptr;
}

const universe::KnowledgeRecord* find_record_by_id(const universe::WorldState& state, const universe::RecordId id) {
    for (const auto& record : state.knowledge_records) {
        if (record.id == id) {
            return &record;
        }
    }
    return nullptr;
}

const universe::Institution* find_institution(const universe::WorldState& state, const universe::InstitutionId id) {
    for (const auto& institution : state.institutions) {
        if (institution.id == id) {
            return &institution;
        }
    }
    return nullptr;
}

const universe::AgentMemory* find_memory(const universe::WorldState& state, const universe::MemoryId id) {
    for (const auto& memory : state.agent_memories) {
        if (memory.id == id) {
            return &memory;
        }
    }
    return nullptr;
}

std::vector<universe::PersonId> event_observers(const universe::WorldState& state, const universe::EventId event_id) {
    std::vector<universe::PersonId> observers;
    for (const auto& memory : state.agent_memories) {
        if (memory.source_event_id == event_id) {
            observers.push_back(memory.owner_id);
        }
    }
    std::sort(observers.begin(), observers.end());
    observers.erase(std::unique(observers.begin(), observers.end()), observers.end());
    return observers;
}

void verify_resource_invariants(const universe::WorldState& state, const std::int64_t expected_initial_food_units) {
    std::int64_t total_food_stored = 0;
    std::int64_t total_food_produced = 0;
    std::int64_t total_food_consumed = 0;
    std::int64_t total_unmet_food = 0;
    std::int64_t total_commons_food = 0;
    std::uint64_t households_in_shortage = 0;
    std::unordered_map<universe::SettlementId, std::int64_t> owned_wood_by_settlement;
    std::unordered_map<universe::SettlementId, std::int64_t> owned_stone_by_settlement;
    std::unordered_map<universe::SettlementId, std::int64_t> owned_timber_by_settlement;
    std::unordered_map<universe::SettlementId, std::int64_t> owned_tools_by_settlement;

    for (const auto& settlement : state.settlements) {
        require(settlement.commons_food.value >= 0, "settlement commons food must never be negative");
        total_commons_food += settlement.commons_food.value;
    }

    for (const auto& household : state.households) {
        require(household.resources.food_inventory.value >= 0, "household food inventory must never be negative");
        require(household.resources.food_produced_total >= 0, "household food produced total must never be negative");
        require(household.resources.food_consumed_total >= 0, "household food consumed total must never be negative");
        require(household.resources.unmet_food_total >= 0, "household unmet food total must never be negative");

        total_food_stored += household.resources.food_inventory.value;
        total_food_produced += household.resources.food_produced_total;
        total_food_consumed += household.resources.food_consumed_total;
        total_unmet_food += household.resources.unmet_food_total;
        owned_wood_by_settlement[household.settlement_id] += household.owned_materials.wood.value;
        owned_stone_by_settlement[household.settlement_id] += household.owned_materials.stone.value;
        owned_timber_by_settlement[household.settlement_id] += household.owned_materials.timber.value;
        owned_tools_by_settlement[household.settlement_id] += household.owned_materials.basic_tool.value;
        if (household.resources.consecutive_shortage_days > 0U) {
            ++households_in_shortage;
        }
    }

    for (const auto& settlement : state.settlements) {
        require(owned_wood_by_settlement[settlement.id] == settlement.materials.wood.value, "owned wood partition must match settlement inventory");
        require(owned_stone_by_settlement[settlement.id] == settlement.materials.stone.value, "owned stone partition must match settlement inventory");
        require(owned_timber_by_settlement[settlement.id] == settlement.materials.timber.value, "owned timber partition must match settlement inventory");
        require(owned_tools_by_settlement[settlement.id] == settlement.materials.basic_tool.value, "owned tool partition must match settlement inventory");
    }

    const auto expected_initial = state.households.empty()
        ? 0
        : static_cast<std::int64_t>(state.households.size()) * expected_initial_food_units;

    require(total_food_stored + total_commons_food == state.total_food_stored, "world total_food_stored must match household plus commons aggregation");
    require(total_food_produced == state.total_food_produced, "world total_food_produced must match household aggregation");
    require(total_food_consumed == state.total_food_consumed, "world total_food_consumed must match household aggregation");
    require(total_unmet_food == state.total_unmet_food, "world total_unmet_food must match household aggregation");
    require(households_in_shortage == state.households_in_shortage, "households_in_shortage must match household shortage flags");

    const auto inferred_initial_food = state.total_food_stored + state.total_food_consumed - state.total_food_produced;
    require(inferred_initial_food == expected_initial, "food accounting conservation must hold");
}

void verify_memory_invariants(const universe::WorldState& state, const universe::Simulation& simulation) {
    std::unordered_set<universe::MemoryId> ids;
    std::unordered_map<universe::PersonId, std::unordered_set<universe::EventId>> direct_by_owner;
    std::unordered_map<universe::MemoryId, const universe::KnowledgeTransmission*> transmission_by_recipient_memory;
    transmission_by_recipient_memory.reserve(state.knowledge_transmissions.size());
    for (const auto& transmission : state.knowledge_transmissions) {
        transmission_by_recipient_memory.emplace(transmission.recipient_memory_id, &transmission);
    }
    std::unordered_map<universe::MemoryId, const universe::KnowledgeRecordAccess*> record_access_by_memory;
    record_access_by_memory.reserve(state.knowledge_record_accesses.size());
    for (const auto& access : state.knowledge_record_accesses) {
        record_access_by_memory.emplace(access.reader_memory_id, &access);
    }
    std::unordered_map<universe::RecordId, const universe::KnowledgeRecord*> record_by_id;
    record_by_id.reserve(state.knowledge_records.size());
    for (const auto& record : state.knowledge_records) {
        record_by_id.emplace(record.id, &record);
    }

    for (const auto& memory : state.agent_memories) {
        require(memory.id > 0U, "memory ids must be non-zero");
        require(memory.owner_id > 0U, "memory owner ids must be non-zero");
        require(memory.source_event_id > 0U, "memory source event ids must be non-zero");
        require(
            memory.source == universe::MemorySource::DirectObservation ||
                memory.source == universe::MemorySource::Communication ||
                memory.source == universe::MemorySource::Teaching ||
                memory.source == universe::MemorySource::Record,
            "memory source must be recognized");
        require(memory.confidence_per_10k <= 10'000U, "memory confidence must be bounded");
        require(ids.insert(memory.id).second, "memory ids must be unique");

        if (memory.source == universe::MemorySource::DirectObservation) {
            require(memory.confidence_per_10k == 10'000U, "direct observation confidence starts at full confidence");
            require(
                direct_by_owner[memory.owner_id].insert(memory.source_event_id).second,
                "owner+event direct memory must be unique");
        }

        const auto* owner = find_person(state, memory.owner_id);
        require(owner != nullptr, "memory owner must reference existing person");

        const auto* source_event = find_event(state, memory.source_event_id);
        require(source_event != nullptr, "memory source event must reference existing event");
        if (memory.source == universe::MemorySource::DirectObservation) {
            require(source_event->timestamp == memory.acquired_at, "direct memory acquired_at must match source event timestamp");
        }

        if (memory.source == universe::MemorySource::Communication || memory.source == universe::MemorySource::Teaching) {
            const auto transmission_it = transmission_by_recipient_memory.find(memory.id);
            require(transmission_it != transmission_by_recipient_memory.end(), "communication memory must have transmission");

            const auto* transmission = transmission_it->second;
            require(transmission->recipient_id == memory.owner_id, "transmission recipient must match memory owner");
            require(transmission->transmitted_at == memory.acquired_at, "transmission time must match memory acquired_at");
            require(transmission->confidence_per_10k == memory.confidence_per_10k, "transmission confidence must match memory confidence");

            const universe::AgentMemory* speaker_memory = nullptr;
            for (const auto& candidate : state.agent_memories) {
                if (candidate.id == transmission->speaker_memory_id) {
                    speaker_memory = &candidate;
                    break;
                }
            }
            require(speaker_memory != nullptr, "transmission speaker memory must exist");
            require(speaker_memory->owner_id == transmission->speaker_id, "transmission speaker must own speaker memory");
            require(speaker_memory->source_event_id == memory.source_event_id, "transmission chain must preserve event provenance");

            const auto* speaker = find_person(state, transmission->speaker_id);
            require(speaker != nullptr, "transmission speaker must exist");

            require(transmission->speaker_settlement_id > 0U, "transmission speaker settlement id must be valid");
            require(transmission->recipient_settlement_id > 0U, "transmission recipient settlement id must be valid");
            require(
                transmission->speaker_settlement_id == transmission->recipient_settlement_id,
                "transmissions must be local to same settlement at send time");
        } else if (memory.source == universe::MemorySource::Record) {
            const auto access_it = record_access_by_memory.find(memory.id);
            require(access_it != record_access_by_memory.end(), "record memory must have record-access provenance");
            const auto* access = access_it->second;
            require(access->reader_id == memory.owner_id, "record access reader must match memory owner");
            require(access->read_at == memory.acquired_at, "record access read time must match memory acquired_at");
            require(access->confidence_per_10k == memory.confidence_per_10k, "record access confidence must match memory confidence");

            const auto record_it = record_by_id.find(access->record_id);
            require(record_it != record_by_id.end(), "record access must reference existing record");
            const auto* record = record_it->second;
            require(record->source_event_id == memory.source_event_id, "record memory must preserve record source event provenance");
            require(record->settlement_id == access->reader_settlement_id, "record access must remain in record settlement");
            require(access->read_at.ticks >= record->recorded_at.ticks, "record read time must not predate record creation");
        }

        require(
            simulation.person_remembers_event(memory.owner_id, memory.source_event_id),
            "query path person_remembers_event must match canonical memory state");
    }
}

void verify_event_payload_types(const universe::WorldState& state) {
    for (const auto& event : state.event_log) {
        if (event.type == universe::EventType::Birth) {
            require(std::holds_alternative<universe::BirthEventPayload>(event.payload), "birth payload must match event type");
            const auto& payload = std::get<universe::BirthEventPayload>(event.payload);
            require(find_person(state, payload.child_id) != nullptr, "birth child id must exist");
            require(find_person(state, payload.mother_id) != nullptr, "birth mother id must exist");
        } else if (event.type == universe::EventType::Death) {
            require(std::holds_alternative<universe::DeathEventPayload>(event.payload), "death payload must match event type");
        } else if (event.type == universe::EventType::SettlementFounded) {
            require(std::holds_alternative<universe::SettlementFoundedEventPayload>(event.payload), "founded payload must match event type");
        } else if (event.type == universe::EventType::SettlementExtinct) {
            require(std::holds_alternative<universe::SettlementExtinctEventPayload>(event.payload), "extinct payload must match event type");
        } else if (event.type == universe::EventType::DiscoveryMade) {
            require(std::holds_alternative<universe::DiscoveryMadeEventPayload>(event.payload), "discovery payload must match event type");
        } else if (event.type == universe::EventType::InstitutionFounded) {
            require(std::holds_alternative<universe::InstitutionFoundedEventPayload>(event.payload), "institution founded payload must match event type");
        } else if (event.type == universe::EventType::SettlementMerged) {
            require(std::holds_alternative<universe::SettlementMergedEventPayload>(event.payload), "settlement merged payload must match event type");
        }
    }
}

}  // namespace

int main() {
    const universe::SimulationConfig config{
        .seed = 42,
        .initial_population = 200,
        .enable_unrelated_random_probe = false,
        .initial_household_food_units = 80,
        .food_production_scale_percent = 100,
        .food_system_enabled = true,
    };

    {
        universe::Simulation continuous{config};
        continuous.run_years(500);
        const auto continuous_bytes = continuous.save_snapshot();

        universe::Simulation with_boundary{config};
        with_boundary.run_years(250);
        const auto mid_bytes = with_boundary.save_snapshot();

        universe::Simulation resumed{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
        const auto load_result = universe::Simulation::load_snapshot(mid_bytes, resumed);
        require(load_result.ok, "midpoint snapshot must load");
        resumed.run_years(250);

        require(
            continuous_bytes == resumed.save_snapshot(),
            "500 years continuous and 250+save/load+250 must serialize identically");
    }

    {
        universe::Simulation deterministic_a{config};
        universe::Simulation deterministic_b{config};
        deterministic_a.run_years(120);
        deterministic_b.run_years(120);
        require(
            deterministic_a.save_snapshot() == deterministic_b.save_snapshot(),
            "same seed 120-year integration run must be deterministic");

        verify_resource_invariants(deterministic_a.state(), config.initial_household_food_units);
        verify_event_payload_types(deterministic_a.state());
        verify_memory_invariants(deterministic_a.state(), deterministic_a);
        require(
            deterministic_a.state().active_buildings + deterministic_a.state().active_projects > 0U,
            "construction system should produce active projects or completed buildings");

        std::uint64_t checked_births = 0;
        std::uint64_t checked_deaths = 0;
        std::uint64_t extinction_events = 0;
        for (const auto& event : deterministic_a.state().event_log) {
            if (event.type == universe::EventType::Birth) {
                const auto& payload = std::get<universe::BirthEventPayload>(event.payload);
                require(
                    deterministic_a.person_remembers_event(payload.mother_id, event.id),
                    "mother must remember birth through direct observation");
                bool child_has_direct_birth_memory = false;
                for (const auto& memory : deterministic_a.state().agent_memories) {
                    if (memory.owner_id == payload.child_id &&
                        memory.source_event_id == event.id &&
                        memory.source == universe::MemorySource::DirectObservation) {
                        child_has_direct_birth_memory = true;
                        break;
                    }
                }
                require(!child_has_direct_birth_memory, "newborn must not directly observe own birth");
                ++checked_births;
            } else if (event.type == universe::EventType::Death) {
                const auto& payload = std::get<universe::DeathEventPayload>(event.payload);
                require(
                    !deterministic_a.person_remembers_event(payload.person_id, event.id),
                    "person must not gain a memory of their own death");
                ++checked_deaths;
            } else if (event.type == universe::EventType::SettlementExtinct) {
                std::uint64_t observer_count = 0;
                for (const auto& memory : deterministic_a.state().agent_memories) {
                    if (memory.source_event_id == event.id && memory.source == universe::MemorySource::DirectObservation) {
                        ++observer_count;
                    }
                }
                require(observer_count == 0U, "settlement extinction event may have zero direct observers");
                ++extinction_events;
            }
        }
        require(checked_births > 0U, "integration run should include birth events");
        require(checked_deaths > 0U, "integration run should include death events");
        require(extinction_events > 0U, "integration run should include settlement extinction events");
    }

    {
        auto verify_founding_observers = [](const universe::Simulation& run) {
            std::uint64_t founded_events = 0;
            for (const auto& event : run.state().event_log) {
                if (event.type != universe::EventType::SettlementFounded) {
                    continue;
                }

                ++founded_events;
                std::uint64_t observer_count = 0;
                for (const auto& memory : run.state().agent_memories) {
                    if (memory.source_event_id == event.id) {
                        ++observer_count;
                    }
                }

                require(observer_count > 0U, "settlement founded event should have direct observers from moved residents");
                require(
                    observer_count < run.state().people.size(),
                    "settlement founded event should not automatically be remembered by everyone");
            }
            return founded_events;
        };

        auto founder = config;
        founder.initial_population = 220;
        founder.initial_region_count = 1;
        founder.base_region_area_units = 220;
        founder.initial_household_food_units = 200;
        founder.food_production_scale_percent = 140;
        founder.wood_resource_scale_percent = 130;
        founder.stone_resource_scale_percent = 130;
        founder.material_crafting_scale_percent = 130;

        universe::Simulation founding_run{founder};
        founding_run.run_years(120);

        auto founded_events = verify_founding_observers(founding_run);
        if (founded_events == 0U) {
            auto fallback = founder;
            fallback.seed = 93;
            fallback.initial_population = 260;
            fallback.base_region_area_units = 260;
            fallback.initial_household_food_units = 220;
            fallback.food_production_scale_percent = 150;
            fallback.wood_resource_scale_percent = 150;
            fallback.stone_resource_scale_percent = 150;
            fallback.material_crafting_scale_percent = 150;

            universe::Simulation fallback_run{fallback};
            fallback_run.run_years(160);
            founded_events = verify_founding_observers(fallback_run);
        }

        require(founded_events > 0U, "founding scenario should produce settlement founded events");
    }

    {
        universe::Simulation continuous{config};
        continuous.run_years(120);

        universe::Simulation segmented{config};
        segmented.run_years(40);
        auto boundary_1 = segmented.save_snapshot();
        segmented = universe::Simulation{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
        auto load_result = universe::Simulation::load_snapshot(boundary_1, segmented);
        require(load_result.ok, "first repeated snapshot boundary must load");

        segmented.run_years(40);
        auto boundary_2 = segmented.save_snapshot();
        segmented = universe::Simulation{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
        load_result = universe::Simulation::load_snapshot(boundary_2, segmented);
        require(load_result.ok, "second repeated snapshot boundary must load");

        segmented.run_years(40);

        require(
            continuous.save_snapshot() == segmented.save_snapshot(),
            "repeated save/load continuation must preserve canonical memory/event state without duplication");
    }

    {
        universe::Simulation knowledge_run{config};
        knowledge_run.run_years(160);
        const auto& state = knowledge_run.state();

        bool birth_case_verified = false;
        bool stale_case_verified = false;
        bool no_omniscience_verified = false;
        bool settlement_extinction_stale_verified = false;

        std::unordered_map<universe::PersonId, const universe::SimulationEvent*> birth_event_by_child;
        for (const auto& event : state.event_log) {
            if (event.type != universe::EventType::Birth) {
                continue;
            }
            const auto& payload = std::get<universe::BirthEventPayload>(event.payload);
            birth_event_by_child.emplace(payload.child_id, &event);
        }

        for (const auto& event : state.event_log) {
            if (event.type == universe::EventType::Birth && !birth_case_verified) {
                const auto& payload = std::get<universe::BirthEventPayload>(event.payload);
                const auto* child = find_person(state, payload.child_id);
                if (child == nullptr || !child->alive) {
                    continue;
                }
                const auto observers = event_observers(state, event.id);
                if (observers.empty()) {
                    continue;
                }

                const auto observer_id = observers.front();
                universe::PersonId unrelated_id = 0;
                for (const auto& person : state.people) {
                    if (person.id == observer_id || person.id == payload.child_id) {
                        continue;
                    }
                    if (!knowledge_run.person_remembers_event(person.id, event.id)) {
                        unrelated_id = person.id;
                        break;
                    }
                }
                if (unrelated_id == 0U) {
                    continue;
                }

                const auto observer_status = knowledge_run.known_life_status(observer_id, payload.child_id);
                const auto observer_household = knowledge_run.known_household_for_person(observer_id, payload.child_id);
                require(observer_status.has_value(), "birth observer should know newborn is alive");
                require(*observer_status == universe::KnownLifeStatus::Alive, "birth observer life-status knowledge must be alive");
                require(observer_household.has_value(), "birth observer should know newborn household");
                require(*observer_household == payload.household_id, "birth observer household knowledge must match birth household");

                bool newborn_direct_birth_memory = false;
                for (const auto& memory : state.agent_memories) {
                    if (memory.owner_id == payload.child_id &&
                        memory.source_event_id == event.id &&
                        memory.source == universe::MemorySource::DirectObservation) {
                        newborn_direct_birth_memory = true;
                        break;
                    }
                }
                require(!newborn_direct_birth_memory, "newborn must not directly observe own birth event");
                require(
                    !knowledge_run.known_life_status(unrelated_id, payload.child_id).has_value(),
                    "unrelated non-observer must not gain birth life-status knowledge");
                require(
                    !knowledge_run.known_household_for_person(unrelated_id, payload.child_id).has_value(),
                    "unrelated non-observer must not gain birth household knowledge");

                birth_case_verified = true;
            }

            if (event.type == universe::EventType::SettlementFounded && !no_omniscience_verified) {
                const auto& payload = std::get<universe::SettlementFoundedEventPayload>(event.payload);
                const auto observers = event_observers(state, event.id);
                if (observers.empty()) {
                    continue;
                }

                universe::PersonId unrelated_id = 0;
                for (const auto& person : state.people) {
                    if (std::find(observers.begin(), observers.end(), person.id) == observers.end() &&
                        !knowledge_run.person_remembers_event(person.id, event.id)) {
                        unrelated_id = person.id;
                        break;
                    }
                }
                if (unrelated_id == 0U) {
                    continue;
                }

                require(
                    !knowledge_run.known_settlement_status(unrelated_id, payload.settlement_id).has_value(),
                    "non-observer must not gain settlement knowledge from journal omniscience");
                no_omniscience_verified = true;
            }
        }

        if (!no_omniscience_verified) {
            auto no_omniscience_config = config;
            no_omniscience_config.initial_population = 300;
            no_omniscience_config.initial_region_count = 1;
            // Founding is pressure-driven (ADR 0002), so this needs one settlement that
            // starts over the population threshold, on land that survives its first
            // split; the old starved-out world (60 land units) founds nothing now.
            no_omniscience_config.initial_settlements_per_region = 1;
            no_omniscience_config.base_region_area_units = 480;

            universe::Simulation no_omniscience_run{no_omniscience_config};
            no_omniscience_run.run_years(220);
            const auto& no_omniscience_state = no_omniscience_run.state();
            for (const auto& event : no_omniscience_state.event_log) {
                if (event.type != universe::EventType::SettlementFounded) {
                    continue;
                }

                const auto& payload = std::get<universe::SettlementFoundedEventPayload>(event.payload);
                const auto observers = event_observers(no_omniscience_state, event.id);
                if (observers.empty()) {
                    continue;
                }

                universe::PersonId unrelated_id = 0;
                for (const auto& person : no_omniscience_state.people) {
                    if (std::find(observers.begin(), observers.end(), person.id) == observers.end() &&
                        !no_omniscience_run.person_remembers_event(person.id, event.id)) {
                        unrelated_id = person.id;
                        break;
                    }
                }
                if (unrelated_id == 0U) {
                    continue;
                }

                require(
                    !no_omniscience_run.known_settlement_status(unrelated_id, payload.settlement_id).has_value(),
                    "non-observer must not gain settlement knowledge from journal omniscience");
                no_omniscience_verified = true;
                break;
            }
        }

        struct StaleKnowledgeCase {
            universe::PersonId child_id{};
            universe::PersonId stale_observer_id{};
            universe::PersonId death_observer_id{};
            universe::EventId birth_event_id{};
            universe::EventId death_event_id{};
        } stale_case{};

        bool found_stale_case = false;
        for (const auto& event : state.event_log) {
            if (event.type != universe::EventType::Death) {
                continue;
            }
            const auto& death_payload = std::get<universe::DeathEventPayload>(event.payload);
            const auto birth_it = birth_event_by_child.find(death_payload.person_id);
            if (birth_it == birth_event_by_child.end()) {
                continue;
            }

            const auto* birth_event = birth_it->second;
            const auto birth_observers = event_observers(state, birth_event->id);
            const auto death_observers = event_observers(state, event.id);
            if (birth_observers.empty() || death_observers.empty()) {
                continue;
            }

            universe::PersonId stale_observer_id = 0;
            for (const auto observer_id : birth_observers) {
                if (std::find(death_observers.begin(), death_observers.end(), observer_id) == death_observers.end()) {
                    stale_observer_id = observer_id;
                    break;
                }
            }
            if (stale_observer_id == 0U) {
                continue;
            }

            stale_case = StaleKnowledgeCase{
                .child_id = death_payload.person_id,
                .stale_observer_id = stale_observer_id,
                .death_observer_id = death_observers.front(),
                .birth_event_id = birth_event->id,
                .death_event_id = event.id,
            };
            found_stale_case = true;
            break;
        }

        require(found_stale_case, "integration run should contain stale-knowledge scenario");
        const auto* dead_person = find_person(state, stale_case.child_id);
        require(dead_person != nullptr, "stale-knowledge dead person must exist");
        require(!dead_person->alive, "stale-knowledge target must be dead in world truth");

        const auto stale_status = knowledge_run.known_life_status(stale_case.stale_observer_id, stale_case.child_id);
        require(stale_status.has_value(), "stale observer should retain life-status knowledge");
        require(*stale_status == universe::KnownLifeStatus::Alive, "stale observer should still believe target is alive");

        const auto death_observer_status = knowledge_run.known_life_status(stale_case.death_observer_id, stale_case.child_id);
        require(death_observer_status.has_value(), "death observer should have life-status knowledge");
        require(*death_observer_status == universe::KnownLifeStatus::Dead, "death observer should know target is dead");
        stale_case_verified = true;

        std::uint64_t life_fact_count = 0;
        universe::KnowledgeId life_fact_id = 0;
        for (const auto& knowledge : knowledge_run.knowledge_for_person(stale_case.death_observer_id)) {
            const auto* life = std::get_if<universe::PersonLifeStatusKnowledgeFact>(&knowledge.fact);
            if (life == nullptr || life->person_id != stale_case.child_id) {
                continue;
            }
            ++life_fact_count;
            life_fact_id = knowledge.id;
            require(life->status == universe::KnownLifeStatus::Dead, "life status for death observer must be dead after update");
        }
        require(life_fact_count == 1U, "life-status upsert must keep one current entry per owner/subject");
        require(life_fact_id > 0U, "life-status knowledge id must be stable and non-zero");

        const auto verify_settlement_stale_case = [](const universe::Simulation& run, const universe::WorldState& run_state) {
            for (const auto& event : run_state.event_log) {
                if (event.type != universe::EventType::SettlementExtinct) {
                    continue;
                }
                const auto& extinct_payload = std::get<universe::SettlementExtinctEventPayload>(event.payload);

                for (const auto& prior_event : run_state.event_log) {
                    if (prior_event.id >= event.id || prior_event.type != universe::EventType::SettlementFounded) {
                        continue;
                    }
                    const auto& founded_payload = std::get<universe::SettlementFoundedEventPayload>(prior_event.payload);
                    if (founded_payload.settlement_id != extinct_payload.settlement_id) {
                        continue;
                    }

                    const auto founded_observers = event_observers(run_state, prior_event.id);
                    if (founded_observers.empty()) {
                        continue;
                    }

                    // A settlement now lives for decades before it goes extinct (founding is
                    // pressure-driven, ADR 0002), so the first observer may have died or
                    // lost the knowledge by then. The case needs an observer who kept it.
                    std::optional<universe::PersonId> retained_observer;
                    for (const auto candidate_id : founded_observers) {
                        if (run.known_settlement_status(candidate_id, founded_payload.settlement_id).has_value()) {
                            retained_observer = candidate_id;
                            break;
                        }
                    }
                    if (!retained_observer.has_value()) {
                        continue;
                    }
                    const auto observer_id = *retained_observer;
                    require(
                        !run.person_remembers_event(observer_id, event.id),
                        "settlement extinction currently has zero direct observer memories");

                    const auto settlement_status = run.known_settlement_status(observer_id, founded_payload.settlement_id);
                    require(settlement_status.has_value(), "founded settlement observer should have settlement knowledge");
                    require(settlement_status->region_id == founded_payload.region_id, "settlement knowledge region must match founded event");
                    require(settlement_status->active, "settlement knowledge should remain active without extinction memory");

                    return true;
                }
            }
            return false;
        };

        bool found_settlement_stale_case = verify_settlement_stale_case(knowledge_run, state);
        if (!found_settlement_stale_case) {
            auto stale_settlement_config = config;
            stale_settlement_config.initial_population = 300;
            stale_settlement_config.initial_region_count = 1;
            // Founding is pressure-driven (ADR 0002), so the founded-then-extinct
            // case needs one settlement that starts over the population
            // threshold, on enough land to survive its first split. The old
            // starved-out stress world (60 land units) now loses everyone before
            // anything is founded, so it no longer produces the case.
            stale_settlement_config.initial_settlements_per_region = 1;
            stale_settlement_config.base_region_area_units = 480;

            universe::Simulation stale_settlement_run{stale_settlement_config};
            stale_settlement_run.run_years(220);
            found_settlement_stale_case = verify_settlement_stale_case(stale_settlement_run, stale_settlement_run.state());
        }

        require(found_settlement_stale_case, "integration run should include founded-then-extinct stale settlement knowledge case");
        require(birth_case_verified, "birth-derived knowledge behavior should be verified");
        require(stale_case_verified, "stale life-status knowledge behavior should be verified");
        require(no_omniscience_verified, "no-omniscience behavior should be verified");
        settlement_extinction_stale_verified = found_settlement_stale_case;
        require(settlement_extinction_stale_verified, "settlement extinction stale-knowledge behavior should be verified");

        const auto birth_event = find_event(state, stale_case.birth_event_id);
        const auto death_event = find_event(state, stale_case.death_event_id);
        require(birth_event != nullptr && death_event != nullptr, "stale case events must exist");
        const auto birth_year = universe::DemographicCalendarAdapter::demographic_year(birth_event->timestamp);
        const auto death_year = universe::DemographicCalendarAdapter::demographic_year(death_event->timestamp);
        require(death_year > birth_year, "death used for update-after-load test must occur after birth");

        universe::Simulation pre_load{config};
        pre_load.run_years(birth_year);
        const auto pre_status = pre_load.known_life_status(stale_case.death_observer_id, stale_case.child_id);
        require(pre_status.has_value() && *pre_status == universe::KnownLifeStatus::Alive, "pre-load knowledge should be alive after birth observation");

        universe::KnowledgeId pre_knowledge_id = 0;
        for (const auto& knowledge : pre_load.knowledge_for_person(stale_case.death_observer_id)) {
            const auto* fact = std::get_if<universe::PersonLifeStatusKnowledgeFact>(&knowledge.fact);
            if (fact != nullptr && fact->person_id == stale_case.child_id) {
                pre_knowledge_id = knowledge.id;
                break;
            }
        }
        require(pre_knowledge_id > 0U, "pre-load life-status knowledge id must exist");

        const auto boundary = pre_load.save_snapshot();
        universe::Simulation resumed{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
        const auto load_result = universe::Simulation::load_snapshot(boundary, resumed);
        require(load_result.ok, "knowledge boundary snapshot must load");
        resumed.run_years(death_year - birth_year);

        const auto post_status = resumed.known_life_status(stale_case.death_observer_id, stale_case.child_id);
        require(post_status.has_value() && *post_status == universe::KnownLifeStatus::Dead, "post-load life-status should update to dead after observed death");

        universe::KnowledgeId post_knowledge_id = 0;
        for (const auto& knowledge : resumed.knowledge_for_person(stale_case.death_observer_id)) {
            const auto* fact = std::get_if<universe::PersonLifeStatusKnowledgeFact>(&knowledge.fact);
            if (fact != nullptr && fact->person_id == stale_case.child_id) {
                post_knowledge_id = knowledge.id;
                break;
            }
        }
        require(post_knowledge_id == pre_knowledge_id, "knowledge id must remain stable across load and later upsert update");
    }

    {
        universe::Simulation communication_run{config};
        communication_run.run_years(220);
        const auto& state = communication_run.state();

        std::uint64_t same_household_transmissions = 0;
        std::uint64_t same_settlement_cross_household_transmissions = 0;
        std::uint64_t cross_settlement_transmissions = 0;

        for (const auto& transmission : state.knowledge_transmissions) {
            if (transmission.speaker_settlement_id != transmission.recipient_settlement_id) {
                ++cross_settlement_transmissions;
                continue;
            }

            if (transmission.speaker_household_id == transmission.recipient_household_id) {
                ++same_household_transmissions;
            } else {
                ++same_settlement_cross_household_transmissions;
            }
        }

        require(cross_settlement_transmissions == 0U, "communication must never cross settlements");
        require(same_household_transmissions > 0U, "communication run should include same-household transfer");
        require(
            same_settlement_cross_household_transmissions > 0U,
            "communication run should include same-settlement cross-household transfer");

        bool stale_propagation_verified = false;
        std::unordered_map<universe::PersonId, const universe::SimulationEvent*> death_event_by_person;
        for (const auto& event : state.event_log) {
            if (event.type != universe::EventType::Death) {
                continue;
            }
            const auto& payload = std::get<universe::DeathEventPayload>(event.payload);
            death_event_by_person.emplace(payload.person_id, &event);
        }

        for (const auto& transmission : state.knowledge_transmissions) {
            const auto* recipient_memory = find_memory(state, transmission.recipient_memory_id);
            if (recipient_memory == nullptr) {
                continue;
            }

            const auto* source_event = find_event(state, recipient_memory->source_event_id);
            if (source_event == nullptr || source_event->type != universe::EventType::Birth) {
                continue;
            }

            const auto& birth_payload = std::get<universe::BirthEventPayload>(source_event->payload);
            const auto death_it = death_event_by_person.find(birth_payload.child_id);
            if (death_it == death_event_by_person.end()) {
                continue;
            }
            const auto* death_event = death_it->second;
            if (death_event == nullptr || death_event->timestamp >= transmission.transmitted_at) {
                continue;
            }

            if (communication_run.person_remembers_event(transmission.speaker_id, death_event->id)) {
                continue;
            }

            const auto speaker_view = communication_run.known_life_status(transmission.speaker_id, birth_payload.child_id);
            const auto recipient_view = communication_run.known_life_status(transmission.recipient_id, birth_payload.child_id);
            if (!speaker_view.has_value() || !recipient_view.has_value()) {
                continue;
            }

            if (*speaker_view == universe::KnownLifeStatus::Alive && *recipient_view == universe::KnownLifeStatus::Alive) {
                const auto* world_person = find_person(state, birth_payload.child_id);
                require(world_person != nullptr && !world_person->alive, "stale propagation scenario requires world-truth dead target");
                stale_propagation_verified = true;
                break;
            }
        }

        require(stale_propagation_verified, "stale subjective knowledge must be able to propagate by communication");
    }

    {
        universe::Simulation record_run{config};
        record_run.run_years(260);
        const auto& state = record_run.state();

        require(!state.knowledge_records.empty(), "records milestone run should produce persistent records");
        require(!state.knowledge_record_accesses.empty(), "records milestone run should produce record reads");

        std::unordered_map<universe::RecordId, const universe::KnowledgeRecord*> record_by_id;
        record_by_id.reserve(state.knowledge_records.size());
        for (const auto& record : state.knowledge_records) {
            record_by_id.emplace(record.id, &record);
            require(record.id > 0U, "record id must be non-zero");
            require(record.settlement_id > 0U, "record settlement id must be non-zero");
            require(record.author_id > 0U, "record author id must be non-zero");
            require(record.source_memory_id > 0U, "record source memory id must be non-zero");
            require(record.source_event_id > 0U, "record source event id must be non-zero");
            require(record.recorded_at.ticks <= state.time.ticks, "recorded time must be within simulation timeline");
        }

        bool delayed_read_verified = false;
        for (const auto& access : state.knowledge_record_accesses) {
            const auto record_it = record_by_id.find(access.record_id);
            require(record_it != record_by_id.end(), "record access must reference known record");
            const auto* record = record_it->second;
            require(record->settlement_id == access.reader_settlement_id, "record access must remain in record settlement");
            require(access.read_at.ticks <= state.time.ticks, "record access time must be in timeline");
            if (access.read_at > record->recorded_at) {
                delayed_read_verified = true;
            }
        }

        require(delayed_read_verified, "records should be read in later periods after creation");
    }

    {
        universe::Simulation loss_run{config};
        loss_run.run_years(260);

        const auto baseline_state = loss_run.state();
        std::unordered_map<universe::KnowledgeId, std::uint32_t> old_knowledge_confidence;
        for (const auto& knowledge : baseline_state.agent_knowledge) {
            const auto stale_ticks = baseline_state.time.ticks - knowledge.updated_at.ticks;
            const auto stale_years = static_cast<std::uint64_t>(
                stale_ticks / universe::DemographicCalendarAdapter::year_duration().ticks);
            if (stale_years >= 185U) {
                old_knowledge_confidence.emplace(knowledge.id, knowledge.confidence_per_10k);
            }
        }

        require(!old_knowledge_confidence.empty(), "knowledge-loss scenario requires stale knowledge candidates");

        loss_run.run_years(120);
        const auto& decayed_state = loss_run.state();

        std::unordered_map<universe::KnowledgeId, std::uint32_t> confidence_after;
        confidence_after.reserve(decayed_state.agent_knowledge.size());
        for (const auto& knowledge : decayed_state.agent_knowledge) {
            confidence_after.emplace(knowledge.id, knowledge.confidence_per_10k);
        }

        bool decay_or_forget_observed = false;
        for (const auto& [knowledge_id, old_confidence] : old_knowledge_confidence) {
            const auto it = confidence_after.find(knowledge_id);
            if (it == confidence_after.end()) {
                decay_or_forget_observed = true;
                break;
            }
            if (it->second < old_confidence) {
                decay_or_forget_observed = true;
                break;
            }
        }

        require(decay_or_forget_observed, "stale knowledge should decay or be forgotten over long horizons");
    }

    {
        universe::Simulation discovery_run{config};
        discovery_run.run_years(260);

        const auto& state = discovery_run.state();
        std::unordered_set<std::uint32_t> discovered_keys;
        std::uint64_t discovery_event_count = 0;
        bool settlement_reference_verified = false;

        for (const auto& event : state.event_log) {
            if (event.type != universe::EventType::DiscoveryMade) {
                continue;
            }

            ++discovery_event_count;
            const auto& payload = std::get<universe::DiscoveryMadeEventPayload>(event.payload);
            require(payload.discovery_key > 0U, "discovery event key must be non-zero");
            require(payload.discoverer_id > 0U, "discovery discoverer id must be non-zero");
            require(payload.settlement_id > 0U, "discovery settlement id must be non-zero");
            require(discovered_keys.insert(payload.discovery_key).second, "discovery keys must be globally unique");

            const auto* discoverer = find_person(state, payload.discoverer_id);
            require(discoverer != nullptr, "discovery discoverer must exist");
            const bool settlement_exists = std::any_of(
                state.settlements.begin(),
                state.settlements.end(),
                [&](const universe::Settlement& settlement) { return settlement.id == payload.settlement_id; });
            require(settlement_exists, "discovery settlement must exist");
            const auto* household = find_household(state, discoverer->household_id);
            require(household != nullptr, "discovery discoverer household must exist");

            require(
                discovery_run.person_remembers_event(payload.discoverer_id, event.id),
                "discoverer must directly remember own discovery event");

            settlement_reference_verified = true;
        }

        require(discovery_event_count > 0U, "basic discoveries milestone should produce at least one discovery event");
        require(settlement_reference_verified, "discovery events must reference a valid settlement");
    }

    {
        universe::Simulation institutions_a{config};
        universe::Simulation institutions_b{config};
        institutions_a.run_years(320);
        institutions_b.run_years(320);

        const auto& a = institutions_a.state();
        const auto& b = institutions_b.state();
        require(!a.institutions.empty(), "institutions milestone should produce at least one institution");
        require(a.institutions == b.institutions, "same-seed institution state must be deterministic");

        std::unordered_map<universe::InstitutionId, const universe::InstitutionFoundedEventPayload*> founded_event_by_institution;
        for (const auto& event : a.event_log) {
            if (event.type != universe::EventType::InstitutionFounded) {
                continue;
            }
            const auto& payload = std::get<universe::InstitutionFoundedEventPayload>(event.payload);
            founded_event_by_institution.emplace(payload.institution_id, &payload);
        }

        bool nonlocal_membership_verified = false;
        bool dead_member_pruned_verified = false;
        bool stale_knowledge_transfer_verified = false;

        for (const auto& institution : a.institutions) {
            require(institution.id > 0U, "institution id must be non-zero");
            require(institution.kind == universe::InstitutionKind::KnowledgeKeeper, "institution kind must be knowledge keeper");
            require(institution.settlement_id > 0U, "institution settlement id must be non-zero");
            require(institution.founder_id > 0U, "institution founder must be non-zero");
            require(institution.founded_at.ticks <= a.time.ticks, "institution founded time must be in timeline");

            const auto founded_it = founded_event_by_institution.find(institution.id);
            require(founded_it != founded_event_by_institution.end(), "institution must have founded event");
            require(founded_it->second->settlement_id == institution.settlement_id, "institution founded event settlement must match institution");
            require(founded_it->second->founder_id == institution.founder_id, "institution founded event founder must match institution");

            std::unordered_set<universe::PersonId> member_ids;
            for (const auto member_id : institution.member_ids) {
                require(member_ids.insert(member_id).second, "institution members must be unique");
                const auto* member = find_person(a, member_id);
                require(member != nullptr, "institution member must exist");
                require(member->alive, "institution active members must be alive");
                const auto* household = find_household(a, member->household_id);
                require(household != nullptr, "institution member household must exist");
                require(household->settlement_id == institution.settlement_id, "institution members must remain local to institution settlement");
                nonlocal_membership_verified = true;
            }

            if (institution.active) {
                require(institution.member_ids.size() >= 3U, "active institution should keep minimum membership");
            }

            if (!institution.preserved_record_ids.empty()) {
                for (const auto record_id : institution.preserved_record_ids) {
                    const auto* record = find_record_by_id(a, record_id);
                    require(record != nullptr, "institution preserved record must exist");
                    require(record->settlement_id == institution.settlement_id, "institution preserved record must be local to settlement");
                }
            }

            for (const auto discovery_key : institution.preserved_discovery_keys) {
                require(discovery_key > 0U, "institution preserved discovery key must be non-zero");

                bool has_member_provenance = false;
                for (const auto member_id : institution.member_ids) {
                    if (institutions_a.knows_basic_discovery(member_id, discovery_key)) {
                        has_member_provenance = true;
                        break;
                    }
                }

                bool has_record_provenance = false;
                for (const auto record_id : institution.preserved_record_ids) {
                    const auto* record = find_record_by_id(a, record_id);
                    if (record == nullptr) {
                        continue;
                    }
                    const auto* source_event = find_event(a, record->source_event_id);
                    if (source_event == nullptr || source_event->type != universe::EventType::DiscoveryMade) {
                        continue;
                    }
                    const auto& payload = std::get<universe::DiscoveryMadeEventPayload>(source_event->payload);
                    if (payload.discovery_key == discovery_key) {
                        has_record_provenance = true;
                        break;
                    }
                }

                require(
                    has_member_provenance || has_record_provenance,
                    "institution preserved discoveries must come from local member knowledge or preserved records, not world omniscience");
            }
        }

        std::unordered_set<universe::PersonId> dead_people;
        for (const auto& person : a.people) {
            if (!person.alive) {
                dead_people.insert(person.id);
            }
        }
        for (const auto& institution : a.institutions) {
            for (const auto member_id : institution.member_ids) {
                require(!dead_people.contains(member_id), "dead people must not remain active institution members");
                dead_member_pruned_verified = true;
            }
        }

        for (const auto& transmission : a.knowledge_transmissions) {
            if (transmission.method != universe::CommunicationMethod::Teaching) {
                continue;
            }

            const auto* speaker = find_person(a, transmission.speaker_id);
            const auto* recipient = find_person(a, transmission.recipient_id);
            if (speaker == nullptr || recipient == nullptr) {
                continue;
            }

            const auto* speaker_household = find_household(a, speaker->household_id);
            const auto* recipient_household = find_household(a, recipient->household_id);
            if (speaker_household == nullptr || recipient_household == nullptr) {
                continue;
            }

            if (speaker_household->settlement_id != recipient_household->settlement_id) {
                continue;
            }

            bool institution_member_speaker = false;
            for (const auto& institution : a.institutions) {
                if (!institution.active || institution.settlement_id != speaker_household->settlement_id) {
                    continue;
                }
                if (std::find(institution.member_ids.begin(), institution.member_ids.end(), speaker->id) != institution.member_ids.end()) {
                    institution_member_speaker = true;
                    break;
                }
            }
            if (!institution_member_speaker) {
                continue;
            }

            const auto* recipient_memory = find_memory(a, transmission.recipient_memory_id);
            const auto* source_event = recipient_memory == nullptr ? nullptr : find_event(a, recipient_memory->source_event_id);
            if (source_event == nullptr || source_event->type != universe::EventType::Birth) {
                continue;
            }

            const auto& birth_payload = std::get<universe::BirthEventPayload>(source_event->payload);
            const universe::SimulationEvent* death_event = nullptr;
            for (const auto& event : a.event_log) {
                if (event.type != universe::EventType::Death) {
                    continue;
                }
                const auto& death_payload = std::get<universe::DeathEventPayload>(event.payload);
                if (death_payload.person_id == birth_payload.child_id) {
                    death_event = &event;
                    break;
                }
            }
            if (death_event == nullptr || death_event->timestamp >= transmission.transmitted_at) {
                continue;
            }

            if (institutions_a.person_remembers_event(transmission.speaker_id, death_event->id)) {
                continue;
            }

            const auto speaker_view = institutions_a.known_life_status(transmission.speaker_id, birth_payload.child_id);
            const auto recipient_view = institutions_a.known_life_status(transmission.recipient_id, birth_payload.child_id);
            if (speaker_view.has_value() && recipient_view.has_value() &&
                *speaker_view == universe::KnownLifeStatus::Alive &&
                *recipient_view == universe::KnownLifeStatus::Alive) {
                stale_knowledge_transfer_verified = true;
                break;
            }
        }

        require(nonlocal_membership_verified, "institution membership locality should be verified");
        require(dead_member_pruned_verified, "institution membership pruning on death should be verified");
        require(stale_knowledge_transfer_verified, "institution teaching should be able to preserve/transfer stale subjective knowledge");

        const auto snapshot = institutions_a.save_snapshot();
        universe::Simulation resumed{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
        const auto load_result = universe::Simulation::load_snapshot(snapshot, resumed);
        require(load_result.ok, "institution snapshot should load");
        resumed.run_years(40);

        universe::Simulation continuous{config};
        continuous.run_years(360);
        require(
            resumed.save_snapshot() == continuous.save_snapshot(),
            "institution save/load continuation must remain deterministic and free of duplicate canonical artifacts");
    }

    {
        auto durability_config = config;
        durability_config.initial_population = 280;
        durability_config.initial_region_count = 2;
        durability_config.base_region_area_units = 160;
        durability_config.food_production_scale_percent = 130;
        durability_config.wood_resource_scale_percent = 220;
        durability_config.stone_resource_scale_percent = 220;
        durability_config.material_crafting_scale_percent = 220;

        universe::Simulation durability_run{durability_config};
        durability_run.run_years(30);

        require(
            durability_run.state().total_basic_tool_produced > 0,
            "durability scenario should produce tools");
        require(
            durability_run.state().total_basic_tool_broken > 0,
            "durability scenario should consume and break tools over time");
        require(
            durability_run.state().total_basic_tool_produced >= durability_run.state().total_basic_tool_broken,
            "broken tools should never exceed total produced tools");
        require(
            durability_run.state().average_tool_coverage_per_10k <= 10'000U,
            "tool coverage must remain bounded");
    }

    {
        auto m3_success = config;
        m3_success.seed = 93;
        m3_success.initial_population = 260;
        m3_success.initial_region_count = 2;
        // Founding is pressure-driven (ADR 0002): each region starts as one settlement
        // of about 130 people, over the split threshold, instead of about four of 30.
        m3_success.initial_settlements_per_region = 1;
        m3_success.base_region_area_units = 130;
        m3_success.initial_household_food_units = 90;
        m3_success.food_production_scale_percent = 105;
        m3_success.wood_resource_scale_percent = 150;
        m3_success.stone_resource_scale_percent = 150;
        m3_success.material_crafting_scale_percent = 150;

        universe::Simulation success_run{m3_success};
        success_run.run_years(180);
        const auto& state = success_run.state();

        bool labour_divergence_verified = false;
        bool production_divergence_verified = false;
        bool assets_diverged_verified = false;
        for (std::size_t i = 0; i < state.households.size(); ++i) {
            for (std::size_t j = i + 1; j < state.households.size(); ++j) {
                const auto& a = state.households[i];
                const auto& b = state.households[j];
                if (a.settlement_id != b.settlement_id) {
                    continue;
                }

                std::uint64_t a_active_workers = 0;
                std::uint64_t b_active_workers = 0;
                for (const auto member_id : a.member_ids) {
                    const auto* person = find_person(state, member_id);
                    if (person != nullptr && person->alive && person->occupation != universe::Occupation::None) {
                        ++a_active_workers;
                    }
                }
                for (const auto member_id : b.member_ids) {
                    const auto* person = find_person(state, member_id);
                    if (person != nullptr && person->alive && person->occupation != universe::Occupation::None) {
                        ++b_active_workers;
                    }
                }

                if (a_active_workers != b_active_workers) {
                    labour_divergence_verified = true;
                }
                if (a.resources.food_produced_total != b.resources.food_produced_total) {
                    production_divergence_verified = true;
                }
                if (a.derived_wealth_points != b.derived_wealth_points) {
                    assets_diverged_verified = true;
                }
            }
        }

        require(labour_divergence_verified, "success scenario must show household labour allocation divergence");
        require(production_divergence_verified, "success scenario must show household production divergence");
        require(assets_diverged_verified, "success scenario must show household wealth divergence");
        require(
            state.total_exchange_volume > 0 || state.total_transfer_volume > 0,
            "success scenario must include local exchange or transfer activity");

        bool transfer_activity = false;
        for (const auto& household : state.households) {
            if (household.cumulative_food_transferred_in > 0 || household.cumulative_food_transferred_out > 0 ||
                household.cumulative_material_transferred_in > 0 || household.cumulative_material_transferred_out > 0) {
                transfer_activity = true;
                break;
            }
        }
        require(transfer_activity, "success scenario must include transfer/exchange movement between owners");
        require(state.total_timber_inventory + state.total_stone_inventory + state.total_basic_tool_inventory > 0, "success scenario must accumulate real materials");
        require(state.active_projects + state.active_buildings > 0U, "success scenario must trigger construction activity");
        require(state.active_buildings > 0U, "success scenario must complete at least one building");

        bool building_effect_observed = false;
        bool pressure_or_viability_observed = false;
        for (const auto& settlement : state.settlements) {
            if (settlement.dwelling_capacity_bonus > 0 || settlement.food_store_capacity_bonus > 0 || settlement.workshop_capacity_bonus > 0) {
                building_effect_observed = true;
            }
            if (settlement.economic_pressure_streak > 0U || settlement.economic_viability_streak > 0U) {
                pressure_or_viability_observed = true;
            }
        }
        require(building_effect_observed, "success scenario must apply bounded building effects");
        require(pressure_or_viability_observed, "success scenario must update settlement pressure/viability signals");

        std::uint64_t founded_events = 0;
        for (const auto& event : state.event_log) {
            if (event.type == universe::EventType::SettlementFounded) {
                ++founded_events;
            }
        }
        require(founded_events > 0U, "success scenario must produce at least one economically supported settlement founding");
    }

    {
        auto threshold_failure = config;
        threshold_failure.seed = 22;
        threshold_failure.initial_population = 320;
        threshold_failure.initial_region_count = 1;
        threshold_failure.base_region_area_units = 52;
        threshold_failure.initial_household_food_units = 8;
        threshold_failure.food_production_scale_percent = 72;
        threshold_failure.wood_resource_scale_percent = 45;
        threshold_failure.stone_resource_scale_percent = 45;
        threshold_failure.material_crafting_scale_percent = 45;

        universe::Simulation failure_run{threshold_failure};
        const auto initial_settlement_count = failure_run.state().settlements.size();
        failure_run.run_years(180);
        const auto& state = failure_run.state();

        require(
            state.total_unmet_food > 0 || state.households_in_shortage > 0U || state.material_production_shortfall > 0,
            "threshold-failure scenario must include real economic scarcity signals");

        std::uint64_t founded_events = 0;
        for (const auto& event : state.event_log) {
            if (event.type == universe::EventType::SettlementFounded) {
                ++founded_events;
            }
        }
        require(founded_events == 0U, "insufficient food/material/labour viability must block threshold-only settlement expansion");
        require(
            state.settlements.size() == initial_settlement_count,
            "threshold-failure scenario must not create new settlements");
    }

    {
        const std::array<std::uint64_t, 5> baseline_seeds{11U, 22U, 33U, 44U, 55U};
        std::uint64_t survivors_500 = 0;
        std::uint64_t extinctions_500 = 0;
        for (const auto seed : baseline_seeds) {
            universe::Simulation baseline{universe::SimulationConfig{.seed = seed, .initial_population = 200}};
            baseline.run_years(500);
            verify_resource_invariants(baseline.state(), 80);
            if (baseline.state().population > 0) {
                ++survivors_500;
            } else {
                ++extinctions_500;
            }
        }

        require(survivors_500 >= 3U, "baseline 500-year seeds should usually remain viable");
        require(
            survivors_500 + extinctions_500 == baseline_seeds.size(),
            "baseline 500-year seed accounting must remain consistent");
    }

    std::cout << "All UniverseCore integration tests passed.\n";
    return 0;
}
