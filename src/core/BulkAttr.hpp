// DiskPrism core — decoding of getattrlistbulk(2) result buffers.
//
// Attribute set requested for every directory entry (see `fillBulkAttrList`):
//
//   commonattr  ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_NAME | ATTR_CMN_ERROR | ATTR_CMN_DEVID |
//               ATTR_CMN_OBJTYPE | ATTR_CMN_MODTIME | ATTR_CMN_FLAGS | ATTR_CMN_FILEID
//   dirattr     ATTR_DIR_MOUNTSTATUS
//   fileattr    ATTR_FILE_LINKCOUNT | ATTR_FILE_ALLOCSIZE | ATTR_FILE_DATALENGTH
//
// The kernel packs each entry in *bit-value order of the attribute constants*, not in the
// order they are written in source, with two documented exceptions (getattrlistbulk(2)):
// the returned-attribute set always comes first and ATTR_CMN_ERROR directly follows it.
// On-disk-order of one record (offsets in bytes, everything only 4-byte aligned):
//
//     0  uint32_t        length            total size of this record
//     4  attribute_set_t returned          5 x uint32_t — which attributes are valid
//    24  uint32_t        error             ATTR_CMN_ERROR      (0x20000000, hoisted)
//    28  attrreference_t name              ATTR_CMN_NAME       (0x00000001) offset+length
//    36  dev_t           devid             ATTR_CMN_DEVID      (0x00000002)
//    40  fsobj_type_t    objtype           ATTR_CMN_OBJTYPE    (0x00000008)
//    44  struct timespec modtime           ATTR_CMN_MODTIME    (0x00000400) 16 bytes
//    60  uint32_t        flags             ATTR_CMN_FLAGS      (0x00040000) st_flags
//    64  uint64_t        fileid            ATTR_CMN_FILEID     (0x02000000)
//   --- directories only (the dir group is packed only when objtype == VDIR) ---
//    72  uint32_t        mountstatus       ATTR_DIR_MOUNTSTATUS (0x00000004)
//   --- everything else (the file group is packed only when objtype != VDIR) ---
//    72  uint32_t        linkcount         ATTR_FILE_LINKCOUNT  (0x00000001)
//    76  off_t           allocsize         ATTR_FILE_ALLOCSIZE  (0x00000004)
//    84  off_t           datalength        ATTR_FILE_DATALENGTH (0x00000200)
//   --- variable-length area ---
//    76 / 92  char[]     name bytes, NUL terminated, padded
//
// When the kernel cannot read an entry's attributes it emits a short record that holds
// only `length`, `returned`, `error` and the name. Hence nothing here trusts a fixed
// layout: every field is read with memcpy (no alignment assumption) and only after
// checking that it lies inside the record's fixed-size area.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

struct attrlist;

namespace diskprism {

/// Values of ATTR_CMN_OBJTYPE (`enum vtype` in <sys/vnode.h>).
inline constexpr uint32_t kVnodeRegular = 1;
inline constexpr uint32_t kVnodeDirectory = 2;
inline constexpr uint32_t kVnodeSymlink = 5;

/// BSD file flags (ATTR_CMN_FLAGS == st_flags). Defined here so the code also builds
/// against SDKs that predate them.
inline constexpr uint32_t kBsdFlagDataless = 0x40000000;  ///< SF_DATALESS
inline constexpr uint32_t kBsdFlagFirmlink = 0x00800000;  ///< SF_FIRMLINK

/// Values of ATTR_DIR_MOUNTSTATUS.
inline constexpr uint32_t kMountStatusMountPoint = 0x00000001;  ///< DIR_MNTSTATUS_MNTPOINT
inline constexpr uint32_t kMountStatusTrigger = 0x00000002;     ///< DIR_MNTSTATUS_TRIGGER

/// Bits of `BulkEntry::valid`.
enum BulkValid : uint32_t {
    kBulkHasDevId = 1u << 0,
    kBulkHasObjType = 1u << 1,
    kBulkHasModTime = 1u << 2,
    kBulkHasFlags = 1u << 3,
    kBulkHasFileId = 1u << 4,
    kBulkHasMountStatus = 1u << 5,
    kBulkHasLinkCount = 1u << 6,
    kBulkHasAllocSize = 1u << 7,
    kBulkHasDataLength = 1u << 8,
};

/// One decoded directory entry. `name` points into the buffer that was decoded.
struct BulkEntry {
    std::string_view name;
    uint32_t error = 0;         ///< errno for this entry; non-zero means only `name` is meaningful
    int32_t dev_id = 0;
    uint32_t obj_type = 0;
    int64_t mtime_sec = 0;
    uint32_t bsd_flags = 0;
    uint64_t file_id = 0;
    uint32_t mount_status = 0;  ///< directories only
    uint32_t link_count = 0;    ///< non-directories only
    uint64_t alloc_size = 0;    ///< non-directories only
    uint64_t data_length = 0;   ///< non-directories only
    uint32_t valid = 0;         ///< BulkValid bits: present in the record *and* reported valid

    [[nodiscard]] bool has(uint32_t bits) const noexcept { return (valid & bits) == bits; }
};

/// Fills `list` with the attribute request described at the top of this file.
void fillBulkAttrList(struct attrlist& list) noexcept;

/// Options passed to getattrlistbulk(): FSOPT_NOFOLLOW | FSOPT_PACK_INVAL_ATTRS.
[[nodiscard]] uint64_t bulkOptions() noexcept;

/// Size in bytes of the fixed part of a complete directory / non-directory record.
inline constexpr size_t kBulkHeaderSize = 36;         ///< length + returned + error + name ref
inline constexpr size_t kBulkCommonSize = 72;         ///< header + devid..fileid
inline constexpr size_t kBulkDirectoryFixedSize = 76;
inline constexpr size_t kBulkFileFixedSize = 92;

/// Iterates over the records getattrlistbulk() wrote into a buffer.
///
/// `capacityBytes` is the size of the buffer that was handed to the kernel and
/// `entryCount` the value the system call returned. Decoding stops — and `malformed()`
/// becomes true — as soon as a record does not fit, so a corrupt or truncated buffer can
/// never cause a read outside `[buffer, buffer + capacityBytes)`.
class BulkEntryReader {
public:
    BulkEntryReader(const char* buffer, size_t capacityBytes, int entryCount) noexcept
        : cursor_(buffer), end_(buffer + capacityBytes), remaining_(entryCount) {}

    /// Decodes the next record into `entry`. Returns false when no record is left or the
    /// buffer is malformed.
    [[nodiscard]] bool next(BulkEntry& entry) noexcept;

    [[nodiscard]] bool malformed() const noexcept { return malformed_; }
    [[nodiscard]] int remaining() const noexcept { return remaining_; }

private:
    const char* cursor_;
    const char* end_;
    int remaining_;
    bool malformed_ = false;
};

}  // namespace diskprism
