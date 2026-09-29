#include "universe/ui/life_history.hpp"

#include <algorithm>
#include <array>
#include <unordered_map>
#include <utility>

#include "protocol_text.hpp"

namespace universe::ui {

namespace {

constexpr std::array<std::pair<LifeEntryKind, std::string_view>, 11> kEntryKindLabels{{
    {LifeEntryKind::Born, "Born"},
    {LifeEntryKind::HadChild, "HadChild"},
    {LifeEntryKind::Died, "Died"},
    {LifeEntryKind::OccupationChanged, "OccupationChanged"},
    {LifeEntryKind::HouseholdRelocated, "HouseholdRelocated"},
    {LifeEntryKind::MadeDiscovery, "MadeDiscovery"},
    {LifeEntryKind::FoundedInstitution, "FoundedInstitution"},
    {LifeEntryKind::LearnedFrom, "LearnedFrom"},
    {LifeEntryKind::Taught, "Taught"},
    {LifeEntryKind::AuthoredRecord, "AuthoredRecord"},
    {LifeEntryKind::ReadRecord, "ReadRecord"},
}};

[[nodiscard]] std::string job_name(const Occupation occupation) {
    switch (occupation) {
        case Occupation::None:
            return "no job";
        case Occupation::SubsistenceCultivator:
            return "farmer";
        case Occupation::WildFoodGatherer:
            return "gatherer";
        case Occupation::WoodGatherer:
            return "woodcutter";
        case Occupation::StoneExtractor:
            return "quarrier";
        case Occupation::CraftWorker:
            return "crafter";
    }
    return "unknown job";
}

[[nodiscard]] std::string person_name(const PersonId id) {
    return "Villager " + std::to_string(id);
}

[[nodiscard]] std::string settlement_name(const SimulationController& controller, const SettlementId id) {
    const auto* settlement = detail::find_by_id(controller.settlements(), id);
    const auto tier = settlement != nullptr ? detail::settlement_tier_label(settlement->tier) : std::string{"Settlement"};
    return tier + " " + std::to_string(id);
}

[[nodiscard]] std::string institution_name(const InstitutionKind kind) {
    switch (kind) {
        case InstitutionKind::KnowledgeKeeper:
            return "a knowledge keepers' circle";
    }
    return "an institution";
}

[[nodiscard]] std::string other_parent_text(const BirthEventPayload& birth, const PersonId self) {
    if (birth.mother_id != self) {
        return " with " + person_name(birth.mother_id);
    }
    if (birth.father_id.has_value() && *birth.father_id != self) {
        return " with " + person_name(*birth.father_id);
    }
    return "";
}

// Appends the entries one event contributes to `person_id`'s life; most events contribute none.
void collect_event_entries(const SimulationController& controller, const SimulationEvent& event, const PersonId person_id,
                           std::vector<LifeHistoryEntry>& entries) {
    const auto add = [&](const LifeEntryKind kind, std::optional<PersonId> other, std::string text) {
        entries.push_back(LifeHistoryEntry{
            .at = event.timestamp,
            .kind = kind,
            .source_id = event.id,
            .other_person_id = other,
            .text = std::move(text),
        });
    };

    if (const auto* birth = std::get_if<BirthEventPayload>(&event.payload)) {
        if (birth->child_id == person_id) {
            std::string text = "Born to " + person_name(birth->mother_id);
            if (birth->father_id.has_value()) {
                text += " and " + person_name(*birth->father_id);
            }
            add(LifeEntryKind::Born, birth->mother_id, text + " in household " + std::to_string(birth->household_id));
        } else if (birth->mother_id == person_id || birth->father_id == person_id) {
            add(LifeEntryKind::HadChild, birth->child_id,
                "Had a child, " + person_name(birth->child_id) + other_parent_text(*birth, person_id));
        }
    } else if (const auto* death = std::get_if<DeathEventPayload>(&event.payload)) {
        if (death->person_id == person_id) {
            add(LifeEntryKind::Died, std::nullopt, "Died");
        }
    } else if (const auto* change = std::get_if<OccupationChangedEventPayload>(&event.payload)) {
        if (change->person_id == person_id) {
            add(LifeEntryKind::OccupationChanged, std::nullopt,
                "Changed job: " + job_name(change->from) + " to " + job_name(change->to));
        }
    } else if (const auto* moved = std::get_if<HouseholdRelocatedEventPayload>(&event.payload)) {
        if (std::find(moved->member_ids.begin(), moved->member_ids.end(), person_id) != moved->member_ids.end()) {
            add(LifeEntryKind::HouseholdRelocated, std::nullopt,
                "Household moved from " + settlement_name(controller, moved->from_settlement_id) + " to " +
                    settlement_name(controller, moved->to_settlement_id));
        }
    } else if (const auto* discovery = std::get_if<DiscoveryMadeEventPayload>(&event.payload)) {
        if (discovery->discoverer_id == person_id) {
            add(LifeEntryKind::MadeDiscovery, std::nullopt,
                "Made basic discovery #" + std::to_string(discovery->discovery_key) + " in " +
                    settlement_name(controller, discovery->settlement_id));
        }
    } else if (const auto* founded = std::get_if<InstitutionFoundedEventPayload>(&event.payload)) {
        if (founded->founder_id == person_id) {
            add(LifeEntryKind::FoundedInstitution, std::nullopt,
                "Founded " + institution_name(founded->kind) + " in " + settlement_name(controller, founded->settlement_id));
        }
    }
}

void collect_knowledge_entries(const SimulationController& controller, const PersonId person_id, std::vector<LifeHistoryEntry>& entries) {
    for (const auto& transmission : controller.knowledge_transmissions()) {
        const bool taught = transmission.method == CommunicationMethod::Teaching;
        if (transmission.recipient_id == person_id) {
            entries.push_back(LifeHistoryEntry{
                .at = transmission.transmitted_at,
                .kind = LifeEntryKind::LearnedFrom,
                .source_id = transmission.id,
                .other_person_id = transmission.speaker_id,
                .text = std::string{taught ? "Was taught by " : "Heard news from "} + person_name(transmission.speaker_id),
            });
        } else if (transmission.speaker_id == person_id) {
            entries.push_back(LifeHistoryEntry{
                .at = transmission.transmitted_at,
                .kind = LifeEntryKind::Taught,
                .source_id = transmission.id,
                .other_person_id = transmission.recipient_id,
                .text = std::string{taught ? "Taught " : "Passed news to "} + person_name(transmission.recipient_id),
            });
        }
    }

    for (const auto& record : controller.knowledge_records()) {
        if (record.author_id == person_id) {
            entries.push_back(LifeHistoryEntry{
                .at = record.recorded_at,
                .kind = LifeEntryKind::AuthoredRecord,
                .source_id = record.id,
                .text = "Wrote record #" + std::to_string(record.id) + " in " + settlement_name(controller, record.settlement_id),
            });
        }
    }

    for (const auto& memory : controller.memories_for_person(person_id)) {
        if (memory.source != MemorySource::Record) {
            continue;
        }
        const auto access = controller.record_access_for_memory(memory.id);
        if (!access.has_value() || access->reader_id != person_id) {
            continue;
        }
        entries.push_back(LifeHistoryEntry{
            .at = access->read_at,
            .kind = LifeEntryKind::ReadRecord,
            .source_id = access->record_id,
            .text = "Read record #" + std::to_string(access->record_id),
        });
    }
}

[[nodiscard]] bool counts_as_life_event(const LifeEntryKind kind) noexcept {
    return kind != LifeEntryKind::LearnedFrom && kind != LifeEntryKind::Taught &&
        kind != LifeEntryKind::AuthoredRecord && kind != LifeEntryKind::ReadRecord;
}

}  // namespace

std::string_view life_entry_kind_label(const LifeEntryKind kind) noexcept {
    for (const auto& [candidate, label] : kEntryKindLabels) {
        if (candidate == kind) {
            return label;
        }
    }
    return "Unknown";
}

std::optional<PersonLifeHistoryView> build_person_life_history(
    const SimulationController& controller,
    const PersonId person_id,
    const std::size_t max_entries) {
    const auto* person = detail::find_by_id(controller.people(), person_id);
    if (person == nullptr) {
        return std::nullopt;
    }

    PersonLifeHistoryView view{
        .id = person->id,
        .alive = person->alive,
        .memory_count = controller.memory_count_for_person(person_id),
        .knowledge_count = controller.knowledge_count_for_person(person_id),
    };

    for (const auto& event : controller.events()) {
        collect_event_entries(controller, event, person_id, view.entries);
    }
    collect_knowledge_entries(controller, person_id, view.entries);

    std::stable_sort(view.entries.begin(), view.entries.end(), [](const LifeHistoryEntry& lhs, const LifeHistoryEntry& rhs) {
        if (lhs.at.ticks != rhs.at.ticks) {
            return lhs.at.ticks < rhs.at.ticks;
        }
        if (lhs.kind != rhs.kind) {
            return lhs.kind < rhs.kind;
        }
        return lhs.source_id < rhs.source_id;
    });

    if (view.entries.size() > max_entries) {
        view.omitted_entries = view.entries.size() - max_entries;
        view.entries.erase(view.entries.begin(), view.entries.begin() + static_cast<std::ptrdiff_t>(view.omitted_entries));
    }

    for (const auto& institution : controller.institutions()) {
        if (std::find(institution.member_ids.begin(), institution.member_ids.end(), person_id) != institution.member_ids.end()) {
            view.institution_ids.push_back(institution.id);
        }
    }

    return view;
}

std::optional<DemoFocusView> build_demo_focus(const SimulationController& controller, const SettlementId settlement_id) {
    if (detail::find_by_id(controller.settlements(), settlement_id) == nullptr) {
        return std::nullopt;
    }

    std::unordered_map<HouseholdId, SettlementId> settlement_by_household;
    for (const auto& household : controller.households()) {
        settlement_by_household.emplace(household.id, household.settlement_id);
    }

    // One pass over the log, counted per person, instead of a life history per resident.
    std::unordered_map<PersonId, std::uint64_t> event_counts;
    std::vector<LifeHistoryEntry> scratch;
    for (const auto& person : controller.people()) {
        if (!person.alive) {
            continue;
        }
        const auto it = settlement_by_household.find(person.household_id);
        if (it != settlement_by_household.end() && it->second == settlement_id) {
            event_counts.emplace(person.id, 0U);
        }
    }
    for (const auto& event : controller.events()) {
        for (auto& [person_id, count] : event_counts) {
            scratch.clear();
            collect_event_entries(controller, event, person_id, scratch);
            for (const auto& entry : scratch) {
                if (counts_as_life_event(entry.kind)) {
                    ++count;
                }
            }
        }
    }

    DemoFocusView view{.settlement_id = settlement_id};
    for (const auto& [person_id, count] : event_counts) {
        if (!view.person_id.has_value() || count > view.life_event_count ||
            (count == view.life_event_count && person_id < *view.person_id)) {
            view.person_id = person_id;
            view.life_event_count = count;
        }
    }
    return view;
}

std::string describe_event(const SimulationController& controller, const SimulationEvent& event) {
    if (const auto* birth = std::get_if<BirthEventPayload>(&event.payload)) {
        std::string text = person_name(birth->child_id) + " was born to " + person_name(birth->mother_id);
        if (birth->father_id.has_value()) {
            text += " and " + person_name(*birth->father_id);
        }
        return text;
    }
    if (const auto* death = std::get_if<DeathEventPayload>(&event.payload)) {
        return person_name(death->person_id) + " died";
    }
    if (const auto* founded = std::get_if<SettlementFoundedEventPayload>(&event.payload)) {
        std::string text = settlement_name(controller, founded->settlement_id) + " was founded";
        if (founded->source_settlement_id.has_value()) {
            text += " by settlers from " + settlement_name(controller, *founded->source_settlement_id);
        }
        return text;
    }
    if (const auto* extinct = std::get_if<SettlementExtinctEventPayload>(&event.payload)) {
        return settlement_name(controller, extinct->settlement_id) + " was abandoned";
    }
    if (const auto* merged = std::get_if<SettlementMergedEventPayload>(&event.payload)) {
        return settlement_name(controller, merged->absorbed_settlement_id) + " merged into " +
            settlement_name(controller, merged->survivor_settlement_id);
    }
    if (const auto* discovery = std::get_if<DiscoveryMadeEventPayload>(&event.payload)) {
        return person_name(discovery->discoverer_id) + " made basic discovery #" + std::to_string(discovery->discovery_key) +
            " in " + settlement_name(controller, discovery->settlement_id);
    }
    if (const auto* institution = std::get_if<InstitutionFoundedEventPayload>(&event.payload)) {
        return person_name(institution->founder_id) + " founded " + institution_name(institution->kind) + " in " +
            settlement_name(controller, institution->settlement_id);
    }
    if (const auto* change = std::get_if<OccupationChangedEventPayload>(&event.payload)) {
        if (change->to == Occupation::None) {
            return person_name(change->person_id) + " stopped working as a " + job_name(change->from);
        }
        return person_name(change->person_id) + " became a " + job_name(change->to);
    }
    if (const auto* moved = std::get_if<HouseholdRelocatedEventPayload>(&event.payload)) {
        return "Household " + std::to_string(moved->household_id) + " moved from " +
            settlement_name(controller, moved->from_settlement_id) + " to " + settlement_name(controller, moved->to_settlement_id);
    }
    return "Something happened";
}

}  // namespace universe::ui
