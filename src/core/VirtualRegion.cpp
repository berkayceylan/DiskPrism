#include "VirtualRegion.hpp"

#include <sys/mman.h>
#include <unistd.h>

#include <new>
#include <utility>

namespace diskprism {

namespace {

size_t roundUpToPage(size_t bytes) noexcept {
    const size_t page = VirtualRegion::pageSize();
    return (bytes + page - 1) / page * page;
}

void* reserve(size_t roundedBytes) noexcept {
    // MAP_NORESERVE is advisory on Darwin (Mach VM never reserves swap up front), but it
    // documents the intent: address space now, pages on first touch.
    void* p = ::mmap(nullptr, roundedBytes, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}

}  // namespace

size_t VirtualRegion::pageSize() noexcept {
    static const size_t page = static_cast<size_t>(::sysconf(_SC_PAGESIZE));
    return page;
}

VirtualRegion::VirtualRegion(size_t bytes) {
    if (bytes == 0) return;
    const size_t rounded = roundUpToPage(bytes);
    void* p = reserve(rounded);
    if (p == nullptr) throw std::bad_alloc();
    base_ = p;
    size_ = rounded;
}

VirtualRegion VirtualRegion::reserveUpTo(size_t preferredBytes, size_t minimumBytes) {
    if (minimumBytes == 0) minimumBytes = pageSize();
    size_t want = preferredBytes < minimumBytes ? minimumBytes : preferredBytes;
    for (;;) {
        const size_t rounded = roundUpToPage(want);
        if (void* p = reserve(rounded)) return VirtualRegion(p, rounded);
        if (want <= minimumBytes) throw std::bad_alloc();
        want /= 2;
        if (want < minimumBytes) want = minimumBytes;
    }
}

VirtualRegion::~VirtualRegion() { release(); }

VirtualRegion::VirtualRegion(VirtualRegion&& other) noexcept
    : base_(std::exchange(other.base_, nullptr)), size_(std::exchange(other.size_, 0)) {}

VirtualRegion& VirtualRegion::operator=(VirtualRegion&& other) noexcept {
    if (this != &other) {
        release();
        base_ = std::exchange(other.base_, nullptr);
        size_ = std::exchange(other.size_, 0);
    }
    return *this;
}

void VirtualRegion::release() noexcept {
    if (base_ != nullptr) {
        ::munmap(base_, size_);
        base_ = nullptr;
        size_ = 0;
    }
}

}  // namespace diskprism
