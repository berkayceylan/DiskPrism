// DiskPrism core — mounted-volume inventory, capacity metrics and scan boundaries.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace diskprism {

/// Snapshot of one mounted file system.
struct VolumeInfo {
    std::string mount_point;  ///< e.g. "/" or "/Volumes/Backup"
    std::string name;         ///< user-visible volume name, e.g. "Macintosh HD"
    std::string device;       ///< f_mntfromname, e.g. "/dev/disk3s1s1"
    std::string fs_type;      ///< f_fstypename, e.g. "apfs"
    /// f_fsid.val[0]: the volume's own device. Note that for the members of an APFS
    /// volume group (system + data) stat(2) and getattrlistbulk(2) instead report one
    /// shared, unified device ID, so this value is not comparable with st_dev there.
    int32_t dev_id = 0;

    uint64_t total_bytes = 0;      ///< statfs: f_blocks * f_bsize
    uint64_t free_bytes = 0;       ///< statfs: f_bfree * f_bsize
    uint64_t available_bytes = 0;  ///< statfs: f_bavail * f_bsize (free to unprivileged users)

    /// NSURLVolumeAvailableCapacityForImportantUsageKey: what the system could make
    /// available for user-requested work, i.e. free space plus purgeable space.
    uint64_t important_available_bytes = 0;
    /// NSURLVolumeAvailableCapacityForOpportunisticUsageKey.
    uint64_t opportunistic_available_bytes = 0;

    bool is_local = false;
    bool is_browsable = false;  ///< shown in Finder (not MNT_DONTBROWSE)
    bool is_read_only = false;
    bool is_root = false;       ///< the boot volume mounted at "/"
    bool is_internal = false;
    bool is_ejectable = false;

    [[nodiscard]] uint64_t used_bytes() const noexcept {
        return total_bytes > free_bytes ? total_bytes - free_bytes : 0;
    }
    /// Space APFS can reclaim on demand (caches, local snapshots, evictable cloud files).
    [[nodiscard]] uint64_t purgeable_bytes() const noexcept {
        return important_available_bytes > available_bytes ? important_available_bytes - available_bytes : 0;
    }
};

/// Every mounted file system, in mount-table order.
[[nodiscard]] std::vector<VolumeInfo> listAllVolumes();

/// Volumes a user would pick as a scan target: local, browsable (plus always "/").
[[nodiscard]] std::vector<VolumeInfo> listUserVolumes();

/// The volume that contains `path`, if it can be determined.
[[nodiscard]] std::optional<VolumeInfo> volumeForPath(const std::string& path);

/// Decides where a scan must stop so it never leaves the volume it was pointed at.
///
/// Darwin gives the read-only system volume and its data volume one *unified* device ID
/// and stitches them together with firmlinks (`/Users`, `/Applications`, `/private`, …),
/// so a scan of `/` reaches user data exactly once through those firmlinks. The data
/// volume is *also* mounted at `/System/Volumes/Data` with that same device ID, so
/// comparing st_dev cannot tell the two routes apart. Three independent checks therefore
/// guard the boundary:
///   1. the kernel's ATTR_DIR_MOUNTSTATUS flag on the directory entry (authoritative),
///   2. the mount table: a directory whose path is a mount point is never opened,
///   3. the device ID of every entry: objects on a foreign device are never counted.
class MountGuard {
public:
    /// `mountPoints` are absolute mount-on paths; `rootPath` is the canonical scan root.
    MountGuard(const std::vector<std::string>& mountPoints, std::string_view rootPath, int32_t rootDev,
               bool crossMountPoints);

    /// Builds a guard from the live mount table.
    [[nodiscard]] static MountGuard forSystem(std::string_view rootPath, int32_t rootDev, bool crossMountPoints);

    /// Check 1: is a directory entry with this mount status a boundary?
    [[nodiscard]] bool statusIsBoundary(uint32_t mountStatus) const noexcept;

    /// Check 2: is `path` (other than the scan root itself) a mount point?
    [[nodiscard]] bool pathIsBoundary(const std::string& path) const;

    /// Check 3: may objects on device `devId` be counted?
    [[nodiscard]] bool deviceAllowed(int32_t devId) const noexcept {
        return cross_mount_points_ || devId == root_dev_;
    }

    [[nodiscard]] int32_t rootDevice() const noexcept { return root_dev_; }
    [[nodiscard]] bool crossesMountPoints() const noexcept { return cross_mount_points_; }

private:
    std::unordered_set<std::string> mount_points_;
    std::string root_path_;
    int32_t root_dev_ = 0;
    bool cross_mount_points_ = false;
};

}  // namespace diskprism
