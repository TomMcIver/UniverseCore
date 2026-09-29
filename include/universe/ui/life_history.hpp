#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "universe/ui/simulation_controller.hpp"

namespace universe::ui {

// One dated line in a person's life history. Every entry is backed by a
// recorded event, knowledge transmission or record; nothing is inferred.
enum class LifeEntryKind : std::uint8_t {
    Born = 0,
    HadChild = 1,
    Died = 2,
    OccupationChanged = 3,
    HouseholdRelocated = 4,
    MadeDiscovery = 5,
    FoundedInstitution = 6,
    LearnedFrom = 7,
    Taught = 8,
    AuthoredRecord = 9,
    ReadRecord = 10,
};

struct LifeHistoryEntry {
    SimulationTime at{};
    LifeEntryKind kind{LifeEntryKind::Born};
    // Source record id: an EventId, CommunicationId or RecordId depending on kind.
    std::uint64_t source_id{};
    std::optional<PersonId> other_person_id{};
    std::string text{};
};

struct PersonLifeHistoryView {
    PersonId id{};
    bool alive{false};
    std::vector<LifeHistoryEntry> entries{};
    // Entries dropped from the oldest end when the history exceeds the cap.
    std::uint64_t omitted_entries{};
    std::vector<InstitutionId> institution_ids{};
    std::uint64_t memory_count{};
    std::uint64_t knowledge_count{};
};

// The living person in a settlement with the most event-log life entries
// (ties go to the lowest id), used to mark the demo's NPC.
struct DemoFocusView {
    SettlementId settlement_id{};
    std::optional<PersonId> person_id{};
    std::uint64_t life_event_count{};
};

inline constexpr std::size_t kMaxLifeHistoryEntries = 200;

[[nodiscard]] std::string_view life_entry_kind_label(LifeEntryKind kind) noexcept;

[[nodiscard]] std::optional<PersonLifeHistoryView> build_person_life_history(
    const SimulationController& controller,
    PersonId person_id,
    std::size_t max_entries = kMaxLifeHistoryEntries);

[[nodiscard]] std::optional<DemoFocusView> build_demo_focus(const SimulationController& controller, SettlementId settlement_id);

// Player-readable one-line description of a world event, e.g. for the event list.
[[nodiscard]] std::string describe_event(const SimulationController& controller, const SimulationEvent& event);

}  // namespace universe::ui
