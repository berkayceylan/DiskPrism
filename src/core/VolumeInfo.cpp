#include "VolumeInfo.hpp"

#include "BulkAttr.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>

#include <algorithm>
#include <cstring>

namespace diskprism {

namespace {

constexpr std::string_view kDataVolumePrefix = "/System/Volumes/Data";

/// Reads the mount table into a caller-owned buffer (getmntinfo() uses static storage
/// and is therefore not thread-safe).
std::vector<struct statfs> readMountTable() {
    std::vector<struct statfs> table;
    for (int attempt = 0; attempt < 4; ++attempt) {
        const int count = ::getfsstat(nullptr, 0, MNT_NOWAIT);
        if (count <= 0) return {};
        table.resize(static_cast<size_t>(count) + 4);  // slack for mounts appearing in between
        const int filled = ::getfsstat(table.data(), static_cast<int>(table.size() * sizeof(struct statfs)), MNT_NOWAIT);
        if (filled < 0) return {};
        if (static_cast<size_t>(filled) < table.size()) {
            table.resize(static_cast<size_t>(filled));
            return table;
        }
    }
    return table;
}

std::string toStdString(CFStringRef string) {
    if (string == nullptr) return {};
    const CFIndex length = CFStringGetLength(string);
    const CFIndex capacity = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    std::string result(static_cast<size_t>(capacity), '\0');
    if (!CFStringGetCString(string, result.data(), capacity, kCFStringEncodingUTF8)) return {};
    result.resize(std::strlen(result.c_str()));
    return result;
}

uint64_t numberValue(CFDictionaryRef dict, CFStringRef key) {
    const void* value = CFDictionaryGetValue(dict, key);
    if (value == nullptr || CFGetTypeID(value) != CFNumberGetTypeID()) return 0;
    int64_t number = 0;
    if (!CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberSInt64Type, &number)) return 0;
    return number > 0 ? static_cast<uint64_t>(number) : 0;
}

bool boolValue(CFDictionaryRef dict, CFStringRef key) {
    const void* value = CFDictionaryGetValue(dict, key);
    return value != nullptr && CFGetTypeID(value) == CFBooleanGetTypeID() &&
           CFBooleanGetValue(static_cast<CFBooleanRef>(value));
}

/// Adds the URL resource values that statfs cannot provide: the user-visible volume
/// name and the capacity that accounts for purgeable space.
void fillResourceValues(VolumeInfo& info) {
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault, reinterpret_cast<const UInt8*>(info.mount_point.data()),
        static_cast<CFIndex>(info.mount_point.size()), true);
    if (url == nullptr) return;

    const void* keyValues[] = {
        kCFURLVolumeNameKey,
        kCFURLVolumeAvailableCapacityForImportantUsageKey,
        kCFURLVolumeAvailableCapacityForOpportunisticUsageKey,
        kCFURLVolumeIsInternalKey,
        kCFURLVolumeIsEjectableKey,
    };
    CFArrayRef keys = CFArrayCreate(kCFAllocatorDefault, keyValues,
                                    static_cast<CFIndex>(sizeof(keyValues) / sizeof(keyValues[0])),
                                    &kCFTypeArrayCallBacks);
    if (keys != nullptr) {
        if (CFDictionaryRef values = CFURLCopyResourcePropertiesForKeys(url, keys, nullptr)) {
            const void* nameValue = CFDictionaryGetValue(values, kCFURLVolumeNameKey);
            if (nameValue != nullptr && CFGetTypeID(nameValue) == CFStringGetTypeID()) {
                info.name = toStdString(static_cast<CFStringRef>(nameValue));
            }
            info.important_available_bytes = numberValue(values, kCFURLVolumeAvailableCapacityForImportantUsageKey);
            info.opportunistic_available_bytes =
                numberValue(values, kCFURLVolumeAvailableCapacityForOpportunisticUsageKey);
            info.is_internal = boolValue(values, kCFURLVolumeIsInternalKey);
            info.is_ejectable = boolValue(values, kCFURLVolumeIsEjectableKey);
            CFRelease(values);
        }
        CFRelease(keys);
    }
    CFRelease(url);
}

VolumeInfo makeVolumeInfo(const struct statfs& fs, bool withResourceValues) {
    VolumeInfo info;
    info.mount_point = fs.f_mntonname;
    info.device = fs.f_mntfromname;
    info.fs_type = fs.f_fstypename;
    info.dev_id = fs.f_fsid.val[0];
    const uint64_t blockSize = fs.f_bsize;
    info.total_bytes = static_cast<uint64_t>(fs.f_blocks) * blockSize;
    info.free_bytes = static_cast<uint64_t>(fs.f_bfree) * blockSize;
    info.available_bytes = static_cast<uint64_t>(fs.f_bavail) * blockSize;
    info.is_local = (fs.f_flags & MNT_LOCAL) != 0;
    info.is_browsable = (fs.f_flags & MNT_DONTBROWSE) == 0;
    info.is_read_only = (fs.f_flags & MNT_RDONLY) != 0;
    info.is_root = (fs.f_flags & MNT_ROOTFS) != 0;

    if (withResourceValues) fillResourceValues(info);
    if (info.name.empty()) {
        // Fall back to the last path component ("/" stays "/").
        const size_t slash = info.mount_point.find_last_of('/');
        info.name = (slash == std::string::npos || slash + 1 >= info.mount_point.size())
                        ? info.mount_point
                        : info.mount_point.substr(slash + 1);
    }
    // Volumes without purgeable accounting report 0 here; never show less than statfs.
    info.important_available_bytes = std::max(info.important_available_bytes, info.available_bytes);
    return info;
}

}  // namespace

std::vector<VolumeInfo> listAllVolumes() {
    std::vector<VolumeInfo> volumes;
    for (const struct statfs& fs : readMountTable()) {
        // Resource values require touching the volume; only do that for local ones so a
        // stalled network share can never block the caller.
        volumes.push_back(makeVolumeInfo(fs, (fs.f_flags & MNT_LOCAL) != 0 && (fs.f_flags & MNT_DONTBROWSE) == 0));
    }
    return volumes;
}

std::vector<VolumeInfo> listUserVolumes() {
    std::vector<VolumeInfo> volumes;
    for (const struct statfs& fs : readMountTable()) {
        const bool isRoot = (fs.f_flags & MNT_ROOTFS) != 0;
        const bool local = (fs.f_flags & MNT_LOCAL) != 0;
        const bool browsable = (fs.f_flags & MNT_DONTBROWSE) == 0;
        if (!isRoot && !(local && browsable)) continue;
        if (fs.f_blocks == 0) continue;  // synthetic file systems
        volumes.push_back(makeVolumeInfo(fs, true));
    }
    // Boot volume first, then alphabetical.
    std::stable_sort(volumes.begin(), volumes.end(), [](const VolumeInfo& a, const VolumeInfo& b) {
        if (a.is_root != b.is_root) return a.is_root;
        return a.name < b.name;
    });
    return volumes;
}

std::optional<VolumeInfo> volumeForPath(const std::string& path) {
    struct statfs fs;
    if (::statfs(path.c_str(), &fs) != 0) return std::nullopt;
    return makeVolumeInfo(fs, (fs.f_flags & MNT_LOCAL) != 0);
}

MountGuard::MountGuard(const std::vector<std::string>& mountPoints, std::string_view rootPath, int32_t rootDev,
                       bool crossMountPoints)
    : root_path_(rootPath), root_dev_(rootDev), cross_mount_points_(crossMountPoints) {
    for (const std::string& mountPoint : mountPoints) {
        if (mountPoint.empty() || mountPoint == "/") continue;
        mount_points_.insert(mountPoint);
        // The kernel reports mounts below firmlinked directories with their firmlink
        // path ("/Library/…"). A scan that started inside the data volume reaches the
        // same directory as "/System/Volumes/Data/Library/…", so register that alias.
        if (mountPoint.compare(0, kDataVolumePrefix.size(), kDataVolumePrefix) != 0) {
            mount_points_.insert(std::string(kDataVolumePrefix) + mountPoint);
        }
    }
}

MountGuard MountGuard::forSystem(std::string_view rootPath, int32_t rootDev, bool crossMountPoints) {
    std::vector<std::string> mountPoints;
    for (const struct statfs& fs : readMountTable()) mountPoints.emplace_back(fs.f_mntonname);
    return MountGuard(mountPoints, rootPath, rootDev, crossMountPoints);
}

bool MountGuard::statusIsBoundary(uint32_t mountStatus) const noexcept {
    if (cross_mount_points_) return false;
    // A trigger is an automount point: opening it would mount a (possibly remote) volume.
    return (mountStatus & (kMountStatusMountPoint | kMountStatusTrigger)) != 0;
}

bool MountGuard::pathIsBoundary(const std::string& path) const {
    if (cross_mount_points_ || path == root_path_) return false;
    return mount_points_.find(path) != mount_points_.end();
}

}  // namespace diskprism
