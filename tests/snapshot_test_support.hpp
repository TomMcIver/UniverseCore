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

inline void write_u64_at(std::vector<std::byte>& bytes, const std::size_t offset, const std::uint64_t value) {
    for (std::size_t i = 0; i < 8U; ++i) {
        bytes[offset + i] = static_cast<std::byte>((value >> (8U * i)) & 0xFFU);
    }
}

inline void write_u16_at(std::vector<std::byte>& bytes, const std::size_t offset, const std::uint16_t value) {
    bytes[offset] = static_cast<std::byte>(value & 0xFFU);
    bytes[offset + 1U] = static_cast<std::byte>((value >> 8U) & 0xFFU);
}

// Rewrites a current (v27) snapshot as v26 by removing the coin section.
inline std::vector<std::byte> downgrade_to_v26(const std::vector<std::byte>& v27_bytes, const universe::WorldState& state) {
    std::vector<std::byte> bytes(v27_bytes.begin(), v27_bytes.end() - static_cast<std::ptrdiff_t>(coin_section_size(state)));
    write_u16_at(bytes, 4U, 26U);
    const auto payload_size = bytes.size() - kHeaderSize;
    write_u64_at(bytes, 8U, static_cast<std::uint64_t>(payload_size));
    const std::span<const std::byte> payload{bytes.data() + kHeaderSize, payload_size};
    write_u64_at(bytes, 16U, universe::snapshot_hash64(payload));
    return bytes;
}

}  // namespace snapshot_test
