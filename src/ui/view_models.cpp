#include "universe/ui/view_models.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <sstream>
#include <utility>

#include "protocol_text.hpp"
#include "universe/ui/life_history.hpp"
#include "universe/ui/web_protocol.hpp"

namespace universe::ui {

namespace {

constexpr std::array<std::pair<ViewModelKind, std::string_view>, 5> kKindLabels{{
    {ViewModelKind::PersonSummary, "person_summary"},
    {ViewModelKind::BuildingSummary, "building_summary"},
    {ViewModelKind::SettlementEconomy, "settlement_economy"},
    {ViewModelKind::PersonLifeHistory, "person_life_history"},
    {ViewModelKind::DemoFocus, "demo_focus"},
}};

constexpr std::uint64_t kPer10k = 10'000U;

[[nodiscard]] std::uint32_t ratio_per_10k(const std::uint64_t numerator, const std::uint64_t denominator) noexcept {
    if (denominator == 0U) {
        return 0U;
    }
    const auto bounded = std::min(numerator, denominator);
    if (bounded <= std::numeric_limits<std::uint64_t>::max() / kPer10k) {
        return static_cast<std::uint32_t>((bounded * kPer10k) / denominator);
    }
    return static_cast<std::uint32_t>(std::min(kPer10k, bounded / (denominator / kPer10k)));
}

void write_optional_id(std::ostringstream& out, const std::optional<std::uint64_t>& id) {
    if (id.has_value()) {
        out << *id;
    } else {
        out << "null";
    }
}

void write_envelope_start(std::ostringstream& out, const std::uint32_t protocol_version, const SimulationController& controller,
                          const ViewModelKind kind, const std::uint64_t entity_id) {
    out << "{\"protocolVersion\":" << protocol_version
        << ",\"type\":\"view_model\""
        << ",\"tick\":" << detail::non_negative_ticks(controller.summary().time)
        << ",\"payload\":{"
        << "\"kind\":\"" << view_model_kind_label(kind) << "\""
        << ",\"entityId\":" << entity_id
        << ",\"version\":" << view_model_version(kind)
        << ",\"data\":{";
}

void write_person_summary(std::ostringstream& out, const PersonSummaryView& view) {
    out << "\"alive\":" << (view.alive ? "true" : "false")
        << ",\"age\":" << view.age_years
        << ",\"sex\":\"" << detail::biological_sex_label(view.sex) << "\""
        << ",\"occupation\":\"" << detail::occupation_label(view.occupation) << "\""
        << ",\"currentActivity\":\"" << detail::activity_kind_label(view.current_activity) << "\""
        << ",\"householdId\":" << view.household_id
        << ",\"settlementId\":" << view.settlement_id
        << ",\"regionId\":" << view.region_id
        << ",\"nutrition\":" << view.nutrition_points
        << ",\"health\":" << view.health_points
        << ",\"memoryCount\":" << view.memory_count
        << ",\"knowledgeCount\":" << view.knowledge_count
        << ",\"motherId\":";
    write_optional_id(out, view.mother_id);
    out << ",\"fatherId\":";
    write_optional_id(out, view.father_id);
}

void write_building_summary(std::ostringstream& out, const BuildingSummaryView& view) {
    out << "\"buildingType\":\"" << detail::building_type_label(view.type) << "\""
        << ",\"constructionState\":\"" << detail::building_state_label(view.state) << "\""
        << ",\"settlementId\":" << view.settlement_id
        << ",\"settlementCommonsOwned\":" << (view.settlement_commons_owned ? "true" : "false")
        << ",\"ownerHouseholdId\":";
    write_optional_id(out, view.owner_household_id);
    out << ",\"timber\":{\"required\":" << view.timber_required << ",\"committed\":" << view.timber_committed << "}"
        << ",\"stone\":{\"required\":" << view.stone_required << ",\"committed\":" << view.stone_committed << "}"
        << ",\"labour\":{\"required\":" << view.labour_required << ",\"completed\":" << view.labour_completed << "}"
        << ",\"startedAtTicks\":" << detail::non_negative_ticks(view.started_at)
        << ",\"completedAtTicks\":";
    if (view.completed_at.has_value()) {
        out << detail::non_negative_ticks(*view.completed_at);
    } else {
        out << "null";
    }
}

void write_settlement_economy(std::ostringstream& out, const SettlementEconomyView& view) {
    out << "\"active\":" << (view.active ? "true" : "false")
        << ",\"tier\":\"" << detail::settlement_tier_label(view.tier) << "\""
        << ",\"regionId\":" << view.region_id
        << ",\"alivePopulation\":" << view.alive_population
        << ",\"households\":" << view.households
        << ",\"food\":{"
        << "\"producedTotal\":" << view.food_produced_total
        << ",\"consumedTotal\":" << view.food_consumed_total
        << ",\"unmetTotal\":" << view.food_unmet_total
        << ",\"householdStored\":" << view.household_food_stored
        << ",\"commonsStored\":" << view.commons_food_stored
        << ",\"accessibleCapacityPerDay\":" << view.accessible_food_capacity_per_day
        << "}"
        << ",\"jobs\":{"
        << "\"SubsistenceCultivator\":" << view.cultivators
        << ",\"WildFoodGatherer\":" << view.gatherers
        << ",\"WoodGatherer\":" << view.wood_gatherers
        << ",\"StoneExtractor\":" << view.stone_extractors
        << ",\"CraftWorker\":" << view.craft_workers
        << ",\"Unallocated\":" << view.unallocated_workers
        << "}"
        << ",\"wealth\":{"
        << "\"min\":" << view.min_household_wealth
        << ",\"median\":" << view.median_household_wealth
        << ",\"max\":" << view.max_household_wealth
        << ",\"inequalityPer10k\":" << view.inequality_per_10k
        << "}"
        << ",\"buildings\":{"
        << "\"complete\":" << view.buildings_complete
        << ",\"underConstruction\":" << view.buildings_under_construction
        << ",\"planned\":" << view.buildings_planned
        << "}";
}

void write_person_life_history(std::ostringstream& out, const PersonLifeHistoryView& view) {
    out << "\"alive\":" << (view.alive ? "true" : "false")
        << ",\"memoryCount\":" << view.memory_count
        << ",\"knowledgeCount\":" << view.knowledge_count
        << ",\"omittedEntries\":" << view.omitted_entries
        << ",\"institutionIds\":[";
    for (std::size_t i = 0; i < view.institution_ids.size(); ++i) {
        out << (i == 0U ? "" : ",") << view.institution_ids[i];
    }
    out << "],\"entries\":[";
    for (std::size_t i = 0; i < view.entries.size(); ++i) {
        const auto& entry = view.entries[i];
        out << (i == 0U ? "" : ",")
            << "{\"atTicks\":" << detail::non_negative_ticks(entry.at)
            << ",\"kind\":\"" << life_entry_kind_label(entry.kind) << "\""
            << ",\"sourceId\":" << entry.source_id
            << ",\"otherPersonId\":";
        write_optional_id(out, entry.other_person_id);
        out << ",\"text\":\"" << detail::json_escape(entry.text) << "\"}";
    }
    out << "]";
}

void write_demo_focus(std::ostringstream& out, const DemoFocusView& view) {
    out << "\"settlementId\":" << view.settlement_id << ",\"personId\":";
    write_optional_id(out, view.person_id);
    out << ",\"lifeEventCount\":" << view.life_event_count;
}

}  // namespace

std::optional<ViewModelKind> parse_view_model_kind(const std::string_view label) noexcept {
    for (const auto& [kind, kind_label] : kKindLabels) {
        if (kind_label == label) {
            return kind;
        }
    }
    return std::nullopt;
}

std::string_view view_model_kind_label(const ViewModelKind kind) noexcept {
    for (const auto& [candidate, label] : kKindLabels) {
        if (candidate == kind) {
            return label;
        }
    }
    return "unknown";
}

std::uint32_t view_model_version(const ViewModelKind kind) noexcept {
    switch (kind) {
        case ViewModelKind::PersonSummary:
        case ViewModelKind::BuildingSummary:
        case ViewModelKind::PersonLifeHistory:
        case ViewModelKind::DemoFocus:
            return 1U;
        case ViewModelKind::SettlementEconomy:
            return 2U;
    }
    return 0U;
}

std::optional<PersonSummaryView> build_person_summary(const SimulationController& controller, const PersonId person_id) {
    const auto* person = detail::find_by_id(controller.people(), person_id);
    if (person == nullptr) {
        return std::nullopt;
    }
    const auto* household = detail::find_by_id(controller.households(), person->household_id);

    return PersonSummaryView{
        .id = person->id,
        .alive = person->alive,
        .age_years = detail::age_years_from_person(controller.summary(), *person),
        .sex = person->sex,
        .occupation = person->occupation,
        .current_activity = person->current_activity,
        .household_id = person->household_id,
        .settlement_id = household != nullptr ? household->settlement_id : SettlementId{},
        .region_id = household != nullptr ? household->region_id : RegionId{},
        .nutrition_points = person->nutrition_points,
        .health_points = person->health_points,
        .memory_count = controller.memory_count_for_person(person_id),
        .knowledge_count = controller.knowledge_count_for_person(person_id),
        .mother_id = person->mother_id,
        .father_id = person->father_id,
    };
}

std::optional<BuildingSummaryView> build_building_summary(const SimulationController& controller, const BuildingId building_id) {
    const auto* building = detail::find_by_id(controller.buildings(), building_id);
    if (building == nullptr) {
        return std::nullopt;
    }

    return BuildingSummaryView{
        .id = building->id,
        .type = building->type,
        .state = building->state,
        .settlement_id = building->settlement_id,
        .owner_household_id = building->owner_household_id,
        .settlement_commons_owned = building->settlement_commons_owned,
        .timber_required = building->timber_required,
        .timber_committed = building->timber_committed,
        .stone_required = building->stone_required,
        .stone_committed = building->stone_committed,
        .labour_required = building->labour_required,
        .labour_completed = building->labour_completed,
        .started_at = building->started_at,
        .completed_at = building->completed_at,
    };
}

std::optional<SettlementEconomyView> build_settlement_economy(const SimulationController& controller, const SettlementId settlement_id) {
    const auto* settlement = detail::find_by_id(controller.settlements(), settlement_id);
    if (settlement == nullptr) {
        return std::nullopt;
    }

    const auto summaries = controller.settlement_summaries();
    const auto summary_it = std::find_if(summaries.begin(), summaries.end(), [settlement_id](const SettlementSummaryView& summary) {
        return summary.id == settlement_id;
    });
    if (summary_it == summaries.end()) {
        return std::nullopt;
    }

    SettlementEconomyView view{
        .id = settlement->id,
        .region_id = settlement->region_id,
        .active = settlement->active,
        .tier = settlement->tier,
        .alive_population = summary_it->alive_population,
        .households = summary_it->households,
        .food_unmet_total = summary_it->food_unmet_total,
        .household_food_stored = summary_it->food_stored,
        .commons_food_stored = settlement->commons_food.value,
        .accessible_food_capacity_per_day = summary_it->accessible_food_capacity_per_day,
        .cultivators = summary_it->cultivators,
        .gatherers = summary_it->gatherers,
        .wood_gatherers = summary_it->wood_gatherers,
        .stone_extractors = summary_it->stone_extractors,
        .craft_workers = summary_it->craft_workers,
        .unallocated_workers = summary_it->unallocated_workers,
        .min_household_wealth = settlement->last_min_household_wealth,
        .median_household_wealth = settlement->last_median_household_wealth,
        .max_household_wealth = settlement->last_max_household_wealth,
        .inequality_per_10k = ratio_per_10k(settlement->last_inequality_numerator, settlement->last_inequality_denominator),
    };

    for (const auto& household : controller.households()) {
        if (household.settlement_id == settlement_id) {
            view.food_produced_total += household.resources.food_produced_total;
            view.food_consumed_total += household.resources.food_consumed_total;
        }
    }

    for (const auto& building : controller.buildings()) {
        if (building.settlement_id != settlement_id) {
            continue;
        }
        switch (building.state) {
            case ConstructionState::Complete:
                ++view.buildings_complete;
                break;
            case ConstructionState::ActiveConstruction:
                ++view.buildings_under_construction;
                break;
            case ConstructionState::Planned:
                ++view.buildings_planned;
                break;
        }
    }

    return view;
}

std::string make_view_model_message(
    const std::uint32_t protocol_version,
    const SimulationController& controller,
    const ViewModelKind kind,
    const std::uint64_t entity_id) {
    std::ostringstream out;
    switch (kind) {
        case ViewModelKind::PersonSummary: {
            const auto view = build_person_summary(controller, entity_id);
            if (!view.has_value()) {
                break;
            }
            write_envelope_start(out, protocol_version, controller, kind, entity_id);
            write_person_summary(out, *view);
            out << "}}}";
            return out.str();
        }
        case ViewModelKind::BuildingSummary: {
            const auto view = build_building_summary(controller, entity_id);
            if (!view.has_value()) {
                break;
            }
            write_envelope_start(out, protocol_version, controller, kind, entity_id);
            write_building_summary(out, *view);
            out << "}}}";
            return out.str();
        }
        case ViewModelKind::SettlementEconomy: {
            const auto view = build_settlement_economy(controller, entity_id);
            if (!view.has_value()) {
                break;
            }
            write_envelope_start(out, protocol_version, controller, kind, entity_id);
            write_settlement_economy(out, *view);
            out << "}}}";
            return out.str();
        }
        case ViewModelKind::PersonLifeHistory: {
            const auto view = build_person_life_history(controller, entity_id);
            if (!view.has_value()) {
                break;
            }
            write_envelope_start(out, protocol_version, controller, kind, entity_id);
            write_person_life_history(out, *view);
            out << "}}}";
            return out.str();
        }
        case ViewModelKind::DemoFocus: {
            const auto view = build_demo_focus(controller, entity_id);
            if (!view.has_value()) {
                break;
            }
            write_envelope_start(out, protocol_version, controller, kind, entity_id);
            write_demo_focus(out, *view);
            out << "}}}";
            return out.str();
        }
    }

    return make_error_message(
        protocol_version,
        "view_model_not_found",
        std::string{view_model_kind_label(kind)} + " not found for entity " + std::to_string(entity_id));
}

}  // namespace universe::ui
