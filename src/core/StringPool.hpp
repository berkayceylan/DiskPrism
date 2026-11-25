// DiskPrism core — contiguous append-only UTF-8 string pool.
#pragma once

#include "VirtualRegion.hpp"

#include <atomic>
#include <cstdint>
#include <string_view>

namespace diskprism {

/// Append-only pool holding every filename back to back (no terminators, no per-string
/// header — the owning `FsNode` stores offset and length).
///
/// Backed by a `VirtualRegion`, so the pool is one contiguous range whose base never
/// moves. `allocate()` is a lock-free bump of an atomic cursor, which lets scanner
/// threads reserve space for a whole directory batch with a single CAS and then copy the
/// bytes without holding any lock.
class StringPool {
public:
    /// Offsets are 32-bit, so a pool can hold just under 4 GiB of name bytes.
    static constexpr uint64_t kMaxCapacity = 0xFFFF'FFF0ull;
    static constexpr uint32_t kInvalidOffset = UINT32_MAX;

    explicit StringPool(uint64_t capacityBytes = kMaxCapacity);

    StringPool(const StringPool&) = delete;
    StringPool& operator=(const StringPool&) = delete;

    /// Reserves `bytes` contiguous bytes. Thread-safe and lock-free.
    /// Returns `kInvalidOffset` when the pool is exhausted (nothing is reserved then).
    [[nodiscard]] uint32_t allocate(uint32_t bytes) noexcept;

    /// Reserves space and copies `text` into it. Thread-safe.
    [[nodiscard]] uint32_t append(std::string_view text) noexcept;

    /// Writable pointer for a range previously returned by `allocate()`.
    [[nodiscard]] char* mutableData(uint32_t offset) noexcept { return base_ + offset; }

    [[nodiscard]] std::string_view view(uint32_t offset, uint32_t length) const noexcept {
        return std::string_view(base_ + offset, length);
    }

    [[nodiscard]] uint64_t size() const noexcept { return used_.load(std::memory_order_acquire); }
    [[nodiscard]] uint64_t capacity() const noexcept { return capacity_; }

private:
    VirtualRegion region_;
    char* base_ = nullptr;
    uint64_t capacity_ = 0;
    std::atomic<uint64_t> used_{0};
};

}  // namespace diskprism
