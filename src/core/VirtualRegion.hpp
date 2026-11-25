// DiskPrism core — lazily committed virtual memory reservation.
#pragma once

#include <cstddef>

namespace diskprism {

/// RAII reservation of zero-filled anonymous virtual memory.
///
/// The kernel commits physical pages only when they are first touched, so reserving a
/// very large range costs address space, not RAM. Because the base address never moves,
/// writers can bump-allocate from many threads without ever invalidating a pointer that
/// another thread holds — which is exactly what the scanner needs, and what a growing
/// `std::vector` cannot offer (it would also briefly double peak RSS on every regrowth).
class VirtualRegion {
public:
    VirtualRegion() noexcept = default;

    /// Reserves exactly `bytes` (rounded up to the page size). Throws `std::bad_alloc`.
    explicit VirtualRegion(size_t bytes);

    /// Reserves `preferredBytes`, halving the request down to `minimumBytes` if the
    /// address space is constrained. Throws `std::bad_alloc` if even the minimum fails.
    [[nodiscard]] static VirtualRegion reserveUpTo(size_t preferredBytes, size_t minimumBytes);

    ~VirtualRegion();

    VirtualRegion(VirtualRegion&& other) noexcept;
    VirtualRegion& operator=(VirtualRegion&& other) noexcept;
    VirtualRegion(const VirtualRegion&) = delete;
    VirtualRegion& operator=(const VirtualRegion&) = delete;

    [[nodiscard]] void* data() const noexcept { return base_; }
    [[nodiscard]] size_t size() const noexcept { return size_; }

    /// System page size (16 KiB on Apple Silicon, 4 KiB on Intel).
    [[nodiscard]] static size_t pageSize() noexcept;

private:
    VirtualRegion(void* base, size_t size) noexcept : base_(base), size_(size) {}
    void release() noexcept;

    void* base_ = nullptr;
    size_t size_ = 0;
};

}  // namespace diskprism
