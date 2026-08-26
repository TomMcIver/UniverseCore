#include "universe/simulation.hpp"
#include "universe/snapshot.hpp"
#include "universe/seed.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace {

void require(const bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

universe::Simulation load_from_snapshot_or_fail(const std::vector<std::byte>& bytes, const char* context) {
    universe::Simulation loaded{universe::SimulationConfig{.seed = 999U, .initial_population = 10U}};
    const auto load_result = universe::Simulation::load_snapshot(bytes, loaded);
    if (!load_result.ok) {
        std::cerr << "Snapshot load failed (" << static_cast<unsigned>(load_result.code) << "): " << load_result.message << '\n';
    }
    require(load_result.ok, context);
    return loaded;
}

const universe::SimulationEvent* find_event_by_id(const universe::WorldState& state, const universe::EventId id) {
    for (const auto& event : state.event_log) {
        if (event.id == id) {
            return &event;
        }
    }
    return nullptr;
}

const universe::Person* find_person_by_id(const universe::WorldState& state, const universe::PersonId id) {
    for (const auto& person : state.people) {
        if (person.id == id) {
            return &person;
        }
    }
    return nullptr;
}

const universe::AgentMemory* find_memory_by_id(const universe::WorldState& state, const universe::MemoryId id) {
    for (const auto& memory : state.agent_memories) {
        if (memory.id == id) {
            return &memory;
        }
    }
    return nullptr;
}

const universe::KnowledgeTransmission* find_transmission_by_recipient_memory(
    const universe::WorldState& state,
    const universe::MemoryId memory_id) {
    for (const auto& transmission : state.knowledge_transmissions) {
        if (transmission.recipient_memory_id == memory_id) {
            return &transmission;
        }
    }
    return nullptr;
}

const universe::KnowledgeRecordAccess* find_record_access_by_memory(
    const universe::WorldState& state,
    const universe::MemoryId memory_id) {
    for (const auto& access : state.knowledge_record_accesses) {
        if (access.reader_memory_id == memory_id) {
            return &access;
        }
    }
    return nullptr;
}

const universe::KnowledgeRecord* find_record_by_id(
    const universe::WorldState& state,
    const universe::RecordId record_id) {
    for (const auto& record : state.knowledge_records) {
        if (record.id == record_id) {
            return &record;
        }
    }
    return nullptr;
}

}  // namespace

int main() {
    {
        require(universe::seconds(1).ticks == 1, "seconds conversion must be exact");
        require(universe::minutes(1).ticks == 60, "minutes conversion must be exact");
        require(universe::hours(1).ticks == 3600, "hours conversion must be exact");
        require(universe::days(1).ticks == 86400, "days conversion must be exact");

        universe::SimulationTime sum{};
        require(
            universe::checked_add_time(universe::SimulationTime{10}, universe::SimulationDuration{15}, sum),
            "checked_add_time should succeed for in-range additions");
        require(sum.ticks == 25, "checked_add_time must compute exact sums");
    }

    {
        const universe::SimulationSeed root{42};
        const auto a = universe::derive_seed(root, universe::SeedDomain::Person, 7U);
        const auto b = universe::derive_seed(root, universe::SeedDomain::Person, 7U);
        require(a.value == b.value, "same parent/domain/id must derive same seed");

        const auto person_seed = universe::derive_seed(root, universe::SeedDomain::Person, 7U);
        const auto household_seed = universe::derive_seed(root, universe::SeedDomain::Household, 7U);
        require(person_seed.value != household_seed.value, "different domains should derive different seeds for same id");
    }

    const universe::SimulationConfig config{
        .seed = 42,
        .initial_population = 200,
        .enable_unrelated_random_probe = false,
        .initial_household_food_units = 80,
        .food_production_scale_percent = 100,
        .food_system_enabled = true,
    };

    {
        universe::Simulation day_by_day{config};
        for (std::uint64_t i = 0; i < 365U; ++i) {
            day_by_day.advance(universe::days(1));
        }

        universe::Simulation one_chunk{config};
        one_chunk.advance(universe::days(365));
        require(day_by_day.save_snapshot() == one_chunk.save_snapshot(), "365 daily advances must equal one 365-day advance");
    }

    {
        universe::Simulation initial{config};
        initial.run_years(10);
        const auto bytes = initial.save_snapshot();
        auto loaded = load_from_snapshot_or_fail(bytes, "10-year snapshot must load");
        require(initial.save_snapshot() == loaded.save_snapshot(), "10-year snapshot round-trip must preserve state");
        require(bytes == loaded.save_snapshot(), "snapshot bytes must be stable across round-trip");
    }

    {
        universe::Simulation a{config};
        universe::Simulation b{config};
        a.run_years(20);
        b.run_years(20);
        require(a.save_snapshot() == b.save_snapshot(), "same seed must produce identical 20-year state");

        auto different_seed = config;
        different_seed.seed = 43;
        universe::Simulation c{different_seed};
        c.run_years(20);
        require(a.save_snapshot() != c.save_snapshot(), "different seeds should produce different canonical snapshot bytes");
    }

    {
        universe::Simulation events{config};
        events.run_years(30);
        for (const auto& event : events.state().event_log) {
            if (event.type == universe::EventType::Birth) {
                require(std::holds_alternative<universe::BirthEventPayload>(event.payload), "birth payload must match event type");
            } else if (event.type == universe::EventType::Death) {
                require(std::holds_alternative<universe::DeathEventPayload>(event.payload), "death payload must match event type");
            } else if (event.type == universe::EventType::SettlementFounded) {
                require(std::holds_alternative<universe::SettlementFoundedEventPayload>(event.payload), "founded payload must match event type");
            } else if (event.type == universe::EventType::SettlementExtinct) {
                require(std::holds_alternative<universe::SettlementExtinctEventPayload>(event.payload), "extinct payload must match event type");
            }
        }
    }

    {
        universe::Simulation memories{config};
        memories.run_years(40);
        const auto& state = memories.state();

        std::unordered_set<universe::MemoryId> memory_ids;
        std::unordered_map<universe::PersonId, std::unordered_set<universe::EventId>> direct_pairs;
        for (const auto& memory : state.agent_memories) {
            require(memory.id > 0U, "memory id must be non-zero");
            require(memory.owner_id > 0U, "memory owner id must be non-zero");
            require(memory.source_event_id > 0U, "memory event id must be non-zero");
            require(
                memory.source == universe::MemorySource::DirectObservation ||
                    memory.source == universe::MemorySource::Communication ||
                    memory.source == universe::MemorySource::Teaching ||
                    memory.source == universe::MemorySource::Record,
                "memory source must be recognized");
            require(memory.confidence_per_10k <= 10'000U, "memory confidence must be bounded");
            require(memory_ids.insert(memory.id).second, "memory ids must be unique");

            if (memory.source == universe::MemorySource::DirectObservation) {
                require(memory.confidence_per_10k == 10'000U, "direct observations start at full confidence");
                require(
                    direct_pairs[memory.owner_id].insert(memory.source_event_id).second,
                    "direct observation memory must be unique per owner/event");
            }

            const auto* owner = find_person_by_id(state, memory.owner_id);
            require(owner != nullptr, "memory owner must reference existing person");
            const auto* event = find_event_by_id(state, memory.source_event_id);
            require(event != nullptr, "memory source event must reference existing event");
            if (memory.source == universe::MemorySource::DirectObservation) {
                require(event->timestamp == memory.acquired_at, "direct memory must align with source event timestamp");
            }

            if (memory.source == universe::MemorySource::Communication || memory.source == universe::MemorySource::Teaching) {
                const auto* transmission = find_transmission_by_recipient_memory(state, memory.id);
                require(transmission != nullptr, "communication memory must have provenance transmission");
                require(transmission->recipient_id == memory.owner_id, "transmission recipient must match memory owner");
                require(transmission->recipient_memory_id == memory.id, "transmission memory reference must match");
                require(transmission->transmitted_at == memory.acquired_at, "transmission time must match memory acquisition");
                require(transmission->confidence_per_10k == memory.confidence_per_10k, "transmission confidence must match memory confidence");

                const auto* speaker_memory = find_memory_by_id(state, transmission->speaker_memory_id);
                require(speaker_memory != nullptr, "transmission speaker memory must exist");
                require(speaker_memory->owner_id == transmission->speaker_id, "transmission speaker must own speaker memory");
                require(speaker_memory->source_event_id == memory.source_event_id, "transmission must preserve source event");
            } else if (memory.source == universe::MemorySource::Record) {
                const auto* access = find_record_access_by_memory(state, memory.id);
                require(access != nullptr, "record memory must have provenance access");
                require(access->reader_id == memory.owner_id, "record access reader must match memory owner");
                require(access->read_at == memory.acquired_at, "record access time must match memory acquisition");
                require(access->confidence_per_10k == memory.confidence_per_10k, "record access confidence must match memory confidence");

                const auto* record = find_record_by_id(state, access->record_id);
                require(record != nullptr, "record access must reference existing record");
                require(record->source_event_id == memory.source_event_id, "record read memory must preserve source event");
                require(record->settlement_id == access->reader_settlement_id, "record access must stay local to settlement");
                require(access->read_at.ticks >= record->recorded_at.ticks, "record access time must not predate record creation");
            }
            require(memories.person_remembers_event(memory.owner_id, memory.source_event_id), "person_remembers_event must match stored memory");
        }

        std::unordered_set<universe::RecordId> record_ids;
        for (const auto& record : state.knowledge_records) {
            require(record.id > 0U, "record id must be non-zero");
            require(record.settlement_id > 0U, "record settlement id must be non-zero");
            require(record.author_id > 0U, "record author id must be non-zero");
            require(record.author_household_id > 0U, "record author household id must be non-zero");
            require(record.source_memory_id > 0U, "record source memory id must be non-zero");
            require(record.source_event_id > 0U, "record source event id must be non-zero");
            require(record.confidence_per_10k <= 10'000U, "record confidence must be bounded");
            require(record_ids.insert(record.id).second, "record ids must be unique");

            const auto* source_memory = find_memory_by_id(state, record.source_memory_id);
            require(source_memory != nullptr, "record source memory must exist");
            require(source_memory->owner_id == record.author_id, "record source memory owner must match author");
            require(source_memory->source_event_id == record.source_event_id, "record source event must match source memory");
        }

        for (const auto& transmission : state.knowledge_transmissions) {
            require(transmission.id > 0U, "transmission id must be non-zero");
            require(transmission.speaker_id > 0U, "transmission speaker id must be non-zero");
            require(transmission.recipient_id > 0U, "transmission recipient id must be non-zero");
            require(transmission.speaker_household_id > 0U, "transmission speaker household id must be non-zero");
            require(transmission.recipient_household_id > 0U, "transmission recipient household id must be non-zero");
            require(transmission.speaker_settlement_id > 0U, "transmission speaker settlement id must be non-zero");
            require(transmission.recipient_settlement_id > 0U, "transmission recipient settlement id must be non-zero");
            require(transmission.speaker_settlement_id == transmission.recipient_settlement_id, "transmission must stay within one settlement");
            require(transmission.speaker_id != transmission.recipient_id, "transmission must involve distinct people");
            require(transmission.speaker_memory_id > 0U, "transmission speaker memory id must be non-zero");
            require(transmission.recipient_memory_id > 0U, "transmission recipient memory id must be non-zero");
            require(transmission.confidence_per_10k <= 10'000U, "transmission confidence must be bounded");
        }

        for (const auto& person : state.people) {
            const auto queried_count = memories.memory_count_for_person(person.id);
            const auto queried_memories = memories.memories_for_person(person.id);
            require(queried_count == queried_memories.size(), "memory_count_for_person must match memories_for_person size");

            const auto queried_knowledge_count = memories.knowledge_count_for_person(person.id);
            const auto queried_knowledge = memories.knowledge_for_person(person.id);
            require(queried_knowledge_count == queried_knowledge.size(), "knowledge_count_for_person must match knowledge_for_person size");
        }

        struct KnowledgeSemanticKey {
            universe::PersonId owner_id{};
            universe::KnowledgeFactKind fact_kind{universe::KnowledgeFactKind::PersonLifeStatus};
            std::uint64_t primary_subject_id{};

            [[nodiscard]] bool operator==(const KnowledgeSemanticKey& other) const noexcept {
                return owner_id == other.owner_id &&
                    fact_kind == other.fact_kind &&
                    primary_subject_id == other.primary_subject_id;
            }
        };

        struct KnowledgeSemanticKeyHash {
            [[nodiscard]] std::size_t operator()(const KnowledgeSemanticKey& key) const noexcept {
                const auto mixed =
                    (key.owner_id * 0x9E3779B97F4A7C15ULL) ^
                    (static_cast<std::uint64_t>(key.fact_kind) * 0xD6E8FEB86659FD93ULL) ^
                    (key.primary_subject_id * 0xA24BAED4963EE407ULL);
                return static_cast<std::size_t>(mixed);
            }
        };

        std::unordered_set<universe::KnowledgeId> knowledge_ids;
        std::unordered_set<KnowledgeSemanticKey, KnowledgeSemanticKeyHash> semantic_keys;
        for (const auto& knowledge : state.agent_knowledge) {
            require(knowledge.id > 0U, "knowledge id must be non-zero");
            require(knowledge.owner_id > 0U, "knowledge owner id must be non-zero");
            require(knowledge.source == universe::KnowledgeSource::MemoryDerived, "knowledge source must be memory-derived");
            require(knowledge.source_memory_id > 0U, "knowledge source memory id must be non-zero");
            require(knowledge.acquired_at.ticks >= 0, "knowledge acquired time must be non-negative");
            require(knowledge.updated_at.ticks >= knowledge.acquired_at.ticks, "knowledge updated time must be >= acquired time");
            require(knowledge.updated_at.ticks <= state.time.ticks, "knowledge updated time must not exceed simulation time");
            require(knowledge.confidence_per_10k <= 10'000U, "knowledge confidence must be bounded");
            require(knowledge_ids.insert(knowledge.id).second, "knowledge ids must be unique");

            const auto* source_memory = find_memory_by_id(state, knowledge.source_memory_id);
            require(source_memory != nullptr, "knowledge source memory must exist");
            require(source_memory->owner_id == knowledge.owner_id, "knowledge and source memory owner must match");
            require(source_memory->acquired_at == knowledge.updated_at, "knowledge updated_at must match source memory acquisition time");
            const auto* source_event = find_event_by_id(state, source_memory->source_event_id);
            require(source_event != nullptr, "knowledge source event must exist");

            if (const auto* fact = std::get_if<universe::PersonLifeStatusKnowledgeFact>(&knowledge.fact); fact != nullptr) {
                const auto status = memories.known_life_status(knowledge.owner_id, fact->person_id);
                require(status.has_value(), "known_life_status query must resolve life knowledge");
                require(*status == fact->status, "known_life_status query must match canonical value");

                if (fact->status == universe::KnownLifeStatus::Alive) {
                    require(source_event->type == universe::EventType::Birth, "alive life fact must derive from birth event");
                    const auto& payload = std::get<universe::BirthEventPayload>(source_event->payload);
                    require(payload.child_id == fact->person_id, "alive life fact subject must match birth child");
                } else {
                    require(source_event->type == universe::EventType::Death, "dead life fact must derive from death event");
                    const auto& payload = std::get<universe::DeathEventPayload>(source_event->payload);
                    require(payload.person_id == fact->person_id, "dead life fact subject must match death person");
                }

                require(
                    semantic_keys.insert(KnowledgeSemanticKey{
                        .owner_id = knowledge.owner_id,
                        .fact_kind = universe::KnowledgeFactKind::PersonLifeStatus,
                        .primary_subject_id = fact->person_id,
                    }).second,
                    "owner+life-status+person semantic key must be unique");
            } else if (const auto* fact = std::get_if<universe::PersonHouseholdKnowledgeFact>(&knowledge.fact); fact != nullptr) {
                const auto household = memories.known_household_for_person(knowledge.owner_id, fact->person_id);
                require(household.has_value(), "known_household_for_person query must resolve household knowledge");
                require(*household == fact->household_id, "known_household_for_person query must match canonical value");

                require(source_event->type == universe::EventType::Birth, "person-household fact must derive from birth event");
                const auto& payload = std::get<universe::BirthEventPayload>(source_event->payload);
                require(payload.child_id == fact->person_id, "person-household fact person must match birth child");
                require(payload.household_id == fact->household_id, "person-household fact household must match birth household");

                require(
                    semantic_keys.insert(KnowledgeSemanticKey{
                        .owner_id = knowledge.owner_id,
                        .fact_kind = universe::KnowledgeFactKind::PersonHousehold,
                        .primary_subject_id = fact->person_id,
                    }).second,
                    "owner+household+person semantic key must be unique");
            } else if (const auto* fact = std::get_if<universe::SettlementStatusKnowledgeFact>(&knowledge.fact); fact != nullptr) {
                const auto settlement = memories.known_settlement_status(knowledge.owner_id, fact->settlement_id);
                require(settlement.has_value(), "known_settlement_status query must resolve settlement knowledge");
                require(settlement->region_id == fact->region_id, "known_settlement_status region must match canonical value");
                require(settlement->active == fact->active, "known_settlement_status active flag must match canonical value");

                if (fact->active) {
                    require(source_event->type == universe::EventType::SettlementFounded, "active settlement fact must derive from founded event");
                    const auto& payload = std::get<universe::SettlementFoundedEventPayload>(source_event->payload);
                    require(payload.settlement_id == fact->settlement_id, "active settlement fact id must match founded event");
                    require(payload.region_id == fact->region_id, "active settlement fact region must match founded event");
                } else {
                    require(source_event->type == universe::EventType::SettlementExtinct, "inactive settlement fact must derive from extinct event");
                    const auto& payload = std::get<universe::SettlementExtinctEventPayload>(source_event->payload);
                    require(payload.settlement_id == fact->settlement_id, "inactive settlement fact id must match extinct event");
                    require(payload.region_id == fact->region_id, "inactive settlement fact region must match extinct event");
                }

                require(
                    semantic_keys.insert(KnowledgeSemanticKey{
                        .owner_id = knowledge.owner_id,
                        .fact_kind = universe::KnowledgeFactKind::SettlementStatus,
                        .primary_subject_id = fact->settlement_id,
                    }).second,
                    "owner+settlement-status+settlement semantic key must be unique");
            } else {
                require(false, "knowledge fact variant must be recognized");
            }
        }
    }

    {
        universe::Simulation no_omniscience{config};
        no_omniscience.run_years(50);

        const auto& state = no_omniscience.state();
        for (const auto& event : state.event_log) {
            if (event.type != universe::EventType::SettlementExtinct) {
                continue;
            }

            std::uint64_t observers = 0;
            for (const auto& memory : state.agent_memories) {
                if (memory.source_event_id == event.id) {
                    ++observers;
                }
            }
            require(observers == 0U, "settlement extinction may have zero direct observers");
        }
    }

    std::cout << "All UniverseCore fast tests passed.\n";
    return 0;
}
