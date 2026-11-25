#include "StringPool.hpp"

#include <algorithm>
#include <cstring>

namespace diskprism {

StringPool::StringPool(uint64_t capacityBytes) {
    const uint64_t wanted = std::clamp<uint64_t>(capacityBytes, 1, kMaxCapacity);
    const size_t minimum = std::min<uint64_t>(wanted, VirtualRegion::pageSize());
    region_ = VirtualRegion::reserveUpTo(static_cast<size_t>(wanted), minimum);
    base_ = static_cast<char*>(region_.data());
    capacity_ = std::min<uint64_t>(region_.size(), wanted);
}

uint32_t StringPool::allocate(uint32_t bytes) noexcept {
    uint64_t current = used_.load(std::memory_order_relaxed);
    for (;;) {
        if (bytes > capacity_ - current) return kInvalidOffset;
        if (used_.compare_exchange_weak(current, current + bytes, std::memory_order_acq_rel,
                                        std::memory_order_relaxed)) {
            return static_cast<uint32_t>(current);
        }
    }
}

uint32_t StringPool::append(std::string_view text) noexcept {
    if (text.size() > UINT32_MAX) return kInvalidOffset;
    const uint32_t offset = allocate(static_cast<uint32_t>(text.size()));
    if (offset == kInvalidOffset) return kInvalidOffset;
    if (!text.empty()) std::memcpy(base_ + offset, text.data(), text.size());
    return offset;
}

}  // namespace diskprism
