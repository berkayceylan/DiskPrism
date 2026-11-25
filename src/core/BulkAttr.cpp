#include "BulkAttr.hpp"

#include <sys/attr.h>
#include <sys/stat.h>
#include <sys/vnode.h>
#include <time.h>

#include <cstring>

namespace diskprism {

// The constants in the header mirror the SDK; fail the build if they ever drift.
static_assert(kVnodeRegular == static_cast<uint32_t>(VREG));
static_assert(kVnodeDirectory == static_cast<uint32_t>(VDIR));
static_assert(kVnodeSymlink == static_cast<uint32_t>(VLNK));
static_assert(kMountStatusMountPoint == DIR_MNTSTATUS_MNTPOINT);
static_assert(kMountStatusTrigger == DIR_MNTSTATUS_TRIGGER);
#ifdef SF_DATALESS
static_assert(kBsdFlagDataless == SF_DATALESS);
#endif
#ifdef SF_FIRMLINK
static_assert(kBsdFlagFirmlink == SF_FIRMLINK);
#endif
static_assert(sizeof(attribute_set_t) == 20);
static_assert(sizeof(attrreference_t) == 8);
static_assert(sizeof(struct timespec) == 16);
static_assert(sizeof(dev_t) == 4 && sizeof(fsobj_type_t) == 4 && sizeof(off_t) == 8);
static_assert(kBulkHeaderSize == sizeof(uint32_t) + sizeof(attribute_set_t) + sizeof(uint32_t) + sizeof(attrreference_t));
static_assert(kBulkCommonSize == kBulkHeaderSize + sizeof(dev_t) + sizeof(fsobj_type_t) +
                                     sizeof(struct timespec) + sizeof(uint32_t) + sizeof(uint64_t));
static_assert(kBulkDirectoryFixedSize == kBulkCommonSize + sizeof(uint32_t));
static_assert(kBulkFileFixedSize == kBulkCommonSize + sizeof(uint32_t) + 2 * sizeof(off_t));

namespace {

/// Reads a `T` from possibly unaligned memory. The kernel only guarantees 4-byte
/// alignment inside a record, so 8-byte values must never be read through a typed
/// pointer (that would be undefined behaviour and trips UBSan's alignment check).
template <typename T>
[[nodiscard]] T load(const char* p) noexcept {
    T value;
    std::memcpy(&value, p, sizeof(T));
    return value;
}

}  // namespace

void fillBulkAttrList(struct attrlist& list) noexcept {
    std::memset(&list, 0, sizeof(list));
    list.bitmapcount = ATTR_BIT_MAP_COUNT;
    list.commonattr = ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_NAME | ATTR_CMN_ERROR | ATTR_CMN_DEVID |
                      ATTR_CMN_OBJTYPE | ATTR_CMN_MODTIME | ATTR_CMN_FLAGS | ATTR_CMN_FILEID;
    list.dirattr = ATTR_DIR_MOUNTSTATUS;
    list.fileattr = ATTR_FILE_LINKCOUNT | ATTR_FILE_ALLOCSIZE | ATTR_FILE_DATALENGTH;
}

uint64_t bulkOptions() noexcept {
    return FSOPT_NOFOLLOW | FSOPT_PACK_INVAL_ATTRS;
}

bool BulkEntryReader::next(BulkEntry& entry) noexcept {
    if (remaining_ <= 0 || malformed_) return false;

    // --- Record envelope ------------------------------------------------------------------
    const size_t available = static_cast<size_t>(end_ - cursor_);
    if (available < kBulkHeaderSize) {
        malformed_ = true;
        return false;
    }
    const uint32_t length = load<uint32_t>(cursor_);
    if (length < kBulkHeaderSize || length > available) {
        malformed_ = true;
        return false;
    }
    const char* const record = cursor_;

    // --- Header: returned set, error, name reference (always present) ---------------------
    const char* p = record + sizeof(uint32_t);
    const attribute_set_t returned = load<attribute_set_t>(p);
    p += sizeof(attribute_set_t);

    entry = BulkEntry{};
    entry.error = load<uint32_t>(p);
    p += sizeof(uint32_t);

    const char* const nameRefPos = p;
    const attrreference_t nameRef = load<attrreference_t>(p);
    p += sizeof(attrreference_t);

    // attr_dataoffset is relative to the attrreference_t itself. The name lives in the
    // variable-length area, which starts where the fixed-size attributes end.
    const int64_t nameStart = static_cast<int64_t>(nameRefPos - record) + static_cast<int64_t>(nameRef.attr_dataoffset);
    if (nameStart < static_cast<int64_t>(kBulkHeaderSize) || nameStart > static_cast<int64_t>(length) ||
        static_cast<uint64_t>(nameRef.attr_length) > static_cast<uint64_t>(length) - static_cast<uint64_t>(nameStart)) {
        malformed_ = true;
        return false;
    }
    const char* const name = record + nameStart;
    entry.name = std::string_view(name, ::strnlen(name, nameRef.attr_length));

    // The record is structurally sound: consume it.
    cursor_ = record + length;
    --remaining_;

    if (entry.error != 0) return true;  // short record: only name + error were packed

    // --- Fixed-size attributes --------------------------------------------------------------
    const char* const fixedEnd = name;
    const auto take = [&p, fixedEnd](size_t bytes) noexcept -> const char* {
        if (static_cast<size_t>(fixedEnd - p) < bytes) return nullptr;
        const char* at = p;
        p += bytes;
        return at;
    };

    const char* at = take(sizeof(dev_t));
    if (at == nullptr) return true;
    entry.dev_id = load<dev_t>(at);
    if (returned.commonattr & ATTR_CMN_DEVID) entry.valid |= kBulkHasDevId;

    at = take(sizeof(fsobj_type_t));
    if (at == nullptr) return true;
    entry.obj_type = load<fsobj_type_t>(at);
    if (returned.commonattr & ATTR_CMN_OBJTYPE) entry.valid |= kBulkHasObjType;

    at = take(sizeof(struct timespec));
    if (at == nullptr) return true;
    entry.mtime_sec = static_cast<int64_t>(load<struct timespec>(at).tv_sec);
    if (returned.commonattr & ATTR_CMN_MODTIME) entry.valid |= kBulkHasModTime;

    at = take(sizeof(uint32_t));
    if (at == nullptr) return true;
    entry.bsd_flags = load<uint32_t>(at);
    if (returned.commonattr & ATTR_CMN_FLAGS) entry.valid |= kBulkHasFlags;

    at = take(sizeof(uint64_t));
    if (at == nullptr) return true;
    entry.file_id = load<uint64_t>(at);
    if (returned.commonattr & ATTR_CMN_FILEID) entry.valid |= kBulkHasFileId;

    if (!entry.has(kBulkHasObjType)) return true;  // cannot tell which group follows

    if (entry.obj_type == kVnodeDirectory) {
        at = take(sizeof(uint32_t));
        if (at == nullptr) return true;
        entry.mount_status = load<uint32_t>(at);
        if (returned.dirattr & ATTR_DIR_MOUNTSTATUS) entry.valid |= kBulkHasMountStatus;
        return true;
    }

    at = take(sizeof(uint32_t));
    if (at == nullptr) return true;
    entry.link_count = load<uint32_t>(at);
    if (returned.fileattr & ATTR_FILE_LINKCOUNT) entry.valid |= kBulkHasLinkCount;

    at = take(sizeof(off_t));
    if (at == nullptr) return true;
    const off_t allocSize = load<off_t>(at);
    if ((returned.fileattr & ATTR_FILE_ALLOCSIZE) && allocSize >= 0) {
        entry.alloc_size = static_cast<uint64_t>(allocSize);
        entry.valid |= kBulkHasAllocSize;
    }

    at = take(sizeof(off_t));
    if (at == nullptr) return true;
    const off_t dataLength = load<off_t>(at);
    if ((returned.fileattr & ATTR_FILE_DATALENGTH) && dataLength >= 0) {
        entry.data_length = static_cast<uint64_t>(dataLength);
        entry.valid |= kBulkHasDataLength;
    }
    return true;
}

}  // namespace diskprism
