// DiskPrism core — turns one decoded directory entry into accounting decisions.
#pragma once

#include "BulkAttr.hpp"

#include <cstdint>

namespace diskprism {

class MountGuard;

/// How one directory entry is to be recorded. Kept separate from the scanner's threading
/// so the accuracy rules (cloud-only files, volume boundaries, hard links, unreadable
/// entries) can be unit-tested with synthetic records.
struct EntryClass {
    uint32_t flags = 0;              ///< NodeFlags for the new node
    uint64_t allocated_bytes = 0;    ///< physical bytes to account (before hard-link deduplication)
    uint64_t logical_bytes = 0;      ///< apparent bytes
    bool readable = true;            ///< false: the kernel returned only a name and an error
    bool permission_denied = false;  ///< the per-entry error was EACCES / EPERM
    bool foreign_device = false;     ///< the entry lives on a device this scan must not count
    bool descend = false;            ///< a directory that should be enumerated
    bool hardlink_candidate = false; ///< regular file with several links: deduplicate by (dev, file id)
};

/// Applies the accounting rules to one entry:
///   * SF_DATALESS (iCloud / File Provider, not downloaded): logical size is kept, the
///     physical size is forced to zero, and a dataless directory is not entered;
///   * mount points and automount triggers are recorded but not entered, and an entry on
///     a foreign device is reported as such (see `MountGuard`);
///   * symbolic links and special files are recorded as themselves, never followed;
///   * directories carry no size of their own (their total is aggregated later).
[[nodiscard]] EntryClass classifyEntry(const BulkEntry& entry, const MountGuard& guard) noexcept;

}  // namespace diskprism
