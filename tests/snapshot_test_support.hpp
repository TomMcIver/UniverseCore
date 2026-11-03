// Helpers for tests that rewrite snapshot bytes to older schema versions.
#pragma once

#include "universe/simulation.hpp"
#include "universe/snapshot.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace snapshot_test {

inline constexpr std::size_t kHeaderSize = 24U;
inline constexpr std::size_t kCoinRecordSize = 16U;
inline constexpr std::size_t kCountSize = 8U;

// Size of the schema v27 coin section, which is last in the payload: a count and
// one (id, coin) record per settlement, then the same for households.
inline std::size_t coin_section_size(const universe::WorldState& state) {
    return kCountSize + (state.settlements.size() * kCoinRecordSize) + kCountSize + (state.households.size() * kCoinRecordSize);
}

// Size of the schema v28 merchant memory section, which is last in the payload:
// a count and one 33-byte row per remembered price (merchant id, settlement id,
// resource byte, price, time seen).
inline constexpr std::size_t kMerchantMemoryRowSize = 33U;
inline std::size_t merchant_section_size(const universe::WorldState& state) {
    return kCountSize + (state.merchant_price_memory.size() * kMerchantMemoryRowSize);
}

// Everything after the aggregate demography totals: coin, then merchant memory.
inline std::size_t trailing_sections_size(const universe::WorldState& state) {
    return coin_section_size(state) + merchant_section_size(state);
}

inline void write_u64_at(std::vector<std::byte>& bytes, const std::size_t offset, const std::uint64_t value) {
    for (std::size_t i = 0; i < 8U; ++i) {
        bytes[offset + i] = static_cast<std::byte>((value >> (8U * i)) & 0xFFU);
    }
}

inline void write_u16_at(std::vector<std::byte>& bytes, const std::size_t offset, const std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xFFU);
    bytes[offset + 1U] = static_cast<std::byte>((value >> 8U) & 0xFFU);
}

// Strips `removed_bytes` from the end of a snapshot, stamps `version`, and
// refreshes the payload length and hash.
inline std::vector<std::byte> truncate_to_version(
    const std::vector<std::byte>& bytes_in, const std::size_t removed_bytes, const std::uint16_t version) {
    std::vector<std::byte> bytes(bytes_in.begin(), bytes_in.end() - static_cast<std::ptrdiff_t>(removed_bytes));
    write_u16_at(bytes, 4U, version);
    const auto payload_size = bytes.size() - kHeaderSize;
    write_u64_at(bytes, 8U, static_cast<std::uint64_t>(payload_size));
    const std::span<const std::byte> payload{bytes.data() + kHeaderSize, payload_size};
    write_u64_at(bytes, 16U, universe::snapshot_hash64(payload));
    return bytes;
}

// Rewrites a current (v28) snapshot as v27 by removing the merchant memory section.
inline std::vector<std::byte> downgrade_to_v27(const std::vector<std::byte>& v28_bytes, const universe::WorldState& state) {
    return truncate_to_version(v28_bytes, merchant_section_size(state), 27U);
}

// Rewrites a current (v28) snapshot as v26 by removing the merchant and coin sections.
inline std::vector<std::byte> downgrade_to_v26(const std::vector<std::byte>& v28_bytes, const universe::WorldState& state) {
    return truncate_to_version(v28_bytes, trailing_sections_size(state), 26U);
}

}  // namespace snapshot_test
