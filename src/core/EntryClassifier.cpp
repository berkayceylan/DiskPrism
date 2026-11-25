#include "EntryClassifier.hpp"

#include "FlatArena.hpp"
#include "VolumeInfo.hpp"

#include <cerrno>

namespace diskprism {

EntryClass classifyEntry(const BulkEntry& entry, const MountGuard& guard) noexcept {
    EntryClass result;

    if (entry.error != 0 || !entry.has(kBulkHasObjType)) {
        // Only the name is trustworthy. Keep the entry visible, with no size.
        result.readable = false;
        result.permission_denied = entry.error == EACCES || entry.error == EPERM;
        result.flags = result.permission_denied ? kNodeAccessDenied : kNodeIOError;
        return result;
    }

    if (entry.has(kBulkHasDevId) && !guard.deviceAllowed(entry.dev_id)) {
        result.foreign_device = true;
        return result;
    }

    const bool dataless = entry.has(kBulkHasFlags) && (entry.bsd_flags & kBsdFlagDataless) != 0;

    if (entry.obj_type == kVnodeDirectory) {
        result.flags = kNodeDir;
        result.descend = true;
        if (entry.has(kBulkHasFlags) && (entry.bsd_flags & kBsdFlagFirmlink) != 0) result.flags |= kNodeFirmlink;
        if (entry.has(kBulkHasMountStatus) && guard.statusIsBoundary(entry.mount_status)) {
            result.flags |= kNodeMountPoint;
            result.descend = false;
        }
        if (dataless) {
            // No local listing exists; enumerating it would make the provider fetch it.
            result.flags |= kNodeDataless;
            result.descend = false;
        }
        return result;
    }

    if (entry.obj_type == kVnodeSymlink) {
        result.flags |= kNodeSymlink;
    } else if (entry.obj_type != kVnodeRegular) {
        result.flags |= kNodeSpecial;
    }

    result.logical_bytes = entry.has(kBulkHasDataLength) ? entry.data_length : 0;
    result.allocated_bytes = entry.has(kBulkHasAllocSize) ? entry.alloc_size : 0;
    if (dataless) {
        // Cloud-only: whatever length the file claims, its content is not on this disk.
        result.flags |= kNodeDataless;
        result.allocated_bytes = 0;
    }

    if (entry.obj_type == kVnodeRegular && entry.link_count > 1 &&
        entry.has(kBulkHasLinkCount | kBulkHasFileId | kBulkHasDevId)) {
        result.flags |= kNodeHardlink;
        result.hardlink_candidate = true;
    }
    return result;
}

}  // namespace diskprism
