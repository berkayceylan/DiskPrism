// getattrlistbulk decoding, scanner and APFS edge-case tests.
//
// Fixtures are created in a private temporary directory with plain POSIX calls. Wherever
// possible the scanner is compared against an independent implementation (lstat / a
// classic readdir + lstat walk) rather than against hard-coded numbers.
#include "TestHarness.hpp"

#include "BulkAttr.hpp"
#include "BulkScanner.hpp"
#include "EntryClassifier.hpp"
#include "FlatArena.hpp"
#include "VolumeInfo.hpp"

#include <dirent.h>
#include <fcntl.h>
#include <sys/attr.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/vnode.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace diskprism;

namespace {

// ----------------------------------------------------------------------------------------
// Temporary directory fixture
// ----------------------------------------------------------------------------------------

/// Removes a directory tree using only descriptor-relative calls, so it also works for
/// paths longer than PATH_MAX and for directories whose permissions were taken away.
void removeTreeAt(int parentFd, const char* name) {
    ::fchmodat(parentFd, name, 0700, AT_SYMLINK_NOFOLLOW);
    const int fd = ::openat(parentFd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        ::unlinkat(parentFd, name, 0);
        return;
    }
    DIR* dir = ::fdopendir(fd);
    if (dir == nullptr) {
        ::close(fd);
        return;
    }
    std::vector<std::pair<std::string, bool>> entries;
    while (const dirent* e = ::readdir(dir)) {
        if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) continue;
        entries.emplace_back(e->d_name, e->d_type == DT_DIR);
    }
    for (const auto& [child, isDir] : entries) {
        if (isDir) {
            removeTreeAt(fd, child.c_str());
        } else {
            ::unlinkat(fd, child.c_str(), 0);
        }
    }
    ::closedir(dir);
    ::unlinkat(parentFd, name, AT_REMOVEDIR);
}

class TempDir {
public:
    TempDir() {
        const char* base = std::getenv("TMPDIR");
        std::string pattern = std::string(base != nullptr && *base != '\0' ? base : "/tmp");
        if (pattern.back() != '/') pattern.push_back('/');
        pattern += "diskprism-test-XXXXXX";
        std::vector<char> buffer(pattern.begin(), pattern.end());
        buffer.push_back('\0');
        if (::mkdtemp(buffer.data()) == nullptr) {
            std::perror("mkdtemp");
            std::abort();
        }
        char* resolved = ::realpath(buffer.data(), nullptr);  // /var -> /private/var
        path_ = resolved != nullptr ? resolved : buffer.data();
        std::free(resolved);
    }
    ~TempDir() {
        const size_t slash = path_.find_last_of('/');
        const std::string parent = path_.substr(0, slash);
        const std::string name = path_.substr(slash + 1);
        const int fd = ::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (fd >= 0) {
            removeTreeAt(fd, name.c_str());
            ::close(fd);
        }
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;

    [[nodiscard]] const std::string& path() const { return path_; }
    [[nodiscard]] std::string sub(const std::string& relative) const { return path_ + "/" + relative; }

    void mkdir(const std::string& relative, mode_t mode = 0755) const {
        if (::mkdir(sub(relative).c_str(), mode) != 0) {
            std::perror(("mkdir " + relative).c_str());
            std::abort();
        }
    }

    /// Writes `size` bytes of non-zero data and flushes them so block allocation is settled.
    void writeFile(const std::string& relative, size_t size) const {
        const int fd = ::open(sub(relative).c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0) {
            std::perror(("open " + relative).c_str());
            std::abort();
        }
        std::vector<char> chunk(64 * 1024);
        for (size_t i = 0; i < chunk.size(); ++i) chunk[i] = static_cast<char>((i * 131 + 7) % 251 + 1);
        size_t remaining = size;
        while (remaining > 0) {
            const size_t n = std::min(remaining, chunk.size());
            if (::write(fd, chunk.data(), n) != static_cast<ssize_t>(n)) {
                std::perror("write");
                std::abort();
            }
            remaining -= n;
        }
        ::fsync(fd);
        ::close(fd);
    }

    /// Creates a file whose length is `logicalSize` but which only stores `dataBytes`.
    void writeSparseFile(const std::string& relative, off_t logicalSize, size_t dataBytes) const {
        const int fd = ::open(sub(relative).c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        if (fd < 0 || ::ftruncate(fd, logicalSize) != 0) {
            std::perror("sparse file");
            std::abort();
        }
        if (dataBytes > 0) {
            const std::vector<char> data(dataBytes, 'x');
            if (::pwrite(fd, data.data(), data.size(), 0) != static_cast<ssize_t>(data.size())) std::abort();
        }
        ::fsync(fd);
        ::close(fd);
    }

    void hardLink(const std::string& existing, const std::string& link) const {
        if (::link(sub(existing).c_str(), sub(link).c_str()) != 0) {
            std::perror("link");
            std::abort();
        }
    }
    void symLink(const std::string& target, const std::string& link) const {
        if (::symlink(target.c_str(), sub(link).c_str()) != 0) {
            std::perror("symlink");
            std::abort();
        }
    }

private:
    std::string path_;
};

struct stat lstatOrDie(const std::string& path) {
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0) {
        std::perror(("lstat " + path).c_str());
        std::abort();
    }
    return st;
}

uint64_t allocatedBytes(const struct stat& st) {
    return static_cast<uint64_t>(st.st_blocks) * 512;
}

bool onApfs(const std::string& path) {
    struct statfs fs;
    return ::statfs(path.c_str(), &fs) == 0 && std::strcmp(fs.f_fstypename, "apfs") == 0;
}

/// Finds a node by its path relative to the root ("a/b/c"); kInvalidNode if absent.
uint32_t findNode(const FlatArena& arena, const std::string& relative) {
    uint32_t current = arena.root();
    size_t pos = 0;
    while (pos < relative.size() && current != kInvalidNode) {
        const size_t slash = relative.find('/', pos);
        const std::string component = relative.substr(pos, slash == std::string::npos ? std::string::npos : slash - pos);
        uint32_t found = kInvalidNode;
        arena.forEachChild(current, [&](uint32_t c) {
            if (arena.name(c) == component) found = c;
        });
        current = found;
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
    return current;
}

struct ScanFixture {
    FlatArena arena;
    BulkScanner scanner;
    ScanResult result;

    explicit ScanFixture(const std::string& path, const ScanOptions& options = ScanOptions{})
        : arena(ArenaConfig{1u << 22, 256u << 20}) {
        result = scanner.scan(path, arena, options);
    }
};

/// The stored totals of every directory must equal the sum over its live children.
bool treeIsConsistent(const FlatArena& arena) {
    std::vector<uint64_t> allocated(arena.size(), 0), logical(arena.size(), 0), files(arena.size(), 0);
    for (uint32_t i = arena.size(); i-- > 0;) {
        const FsNode& n = arena.node(i);
        if (n.is_deleted()) continue;
        if (!n.is_dir()) {
            allocated[i] = n.allocated_bytes;
            logical[i] = n.logical_bytes;
            files[i] = 1;
        } else if (allocated[i] != n.allocated_bytes || logical[i] != n.logical_bytes || files[i] != n.file_count) {
            return false;
        }
        if (n.parent_idx != kInvalidNode) {
            if (n.parent_idx >= i) return false;
            allocated[n.parent_idx] += allocated[i];
            logical[n.parent_idx] += logical[i];
            files[n.parent_idx] += files[i];
        }
    }
    return true;
}

// ----------------------------------------------------------------------------------------
// Synthetic getattrlistbulk buffers (mirrors the kernel's packing)
// ----------------------------------------------------------------------------------------

struct FakeEntry {
    std::string name;
    uint32_t type = VREG;
    uint32_t error = 0;         // non-zero: the kernel's short "name + error" record
    int32_t dev = 16777233;
    int64_t mtime = 1700000000;
    uint32_t flags = 0;
    uint64_t fileId = 1000;
    uint32_t mountStatus = 0;
    uint32_t linkCount = 1;
    int64_t allocSize = 0;
    int64_t dataLength = 0;
};

template <typename T>
void put(std::vector<char>& out, const T& value) {
    const char* p = reinterpret_cast<const char*>(&value);
    out.insert(out.end(), p, p + sizeof(T));
}

std::vector<char> packEntry(const FakeEntry& e) {
    std::vector<char> rec;
    put<uint32_t>(rec, 0);  // length, patched below

    attribute_set_t returned{};
    returned.commonattr = ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_NAME | ATTR_CMN_ERROR;
    const bool isDir = e.type == VDIR;
    if (e.error == 0) {
        returned.commonattr |= ATTR_CMN_DEVID | ATTR_CMN_OBJTYPE | ATTR_CMN_MODTIME | ATTR_CMN_FLAGS | ATTR_CMN_FILEID;
        if (isDir) {
            returned.dirattr = ATTR_DIR_MOUNTSTATUS;
        } else {
            returned.fileattr = ATTR_FILE_LINKCOUNT | ATTR_FILE_ALLOCSIZE | ATTR_FILE_DATALENGTH;
        }
    }
    put(rec, returned);
    put<uint32_t>(rec, e.error);

    const size_t nameRefPos = rec.size();
    put<attrreference_t>(rec, attrreference_t{0, 0});  // patched below

    if (e.error == 0) {
        put<int32_t>(rec, e.dev);
        put<uint32_t>(rec, e.type);
        struct timespec ts {};
        ts.tv_sec = e.mtime;
        ts.tv_nsec = 123456789;
        put(rec, ts);
        put<uint32_t>(rec, e.flags);
        put<uint64_t>(rec, e.fileId);
        if (isDir) {
            put<uint32_t>(rec, e.mountStatus);
        } else {
            put<uint32_t>(rec, e.linkCount);
            put<int64_t>(rec, e.allocSize);
            put<int64_t>(rec, e.dataLength);
        }
    }

    // Variable-length area: the name, NUL terminated, record padded to 8 bytes.
    attrreference_t ref;
    ref.attr_dataoffset = static_cast<int32_t>(rec.size() - nameRefPos);
    ref.attr_length = static_cast<uint32_t>(e.name.size() + 1);
    std::memcpy(rec.data() + nameRefPos, &ref, sizeof(ref));
    rec.insert(rec.end(), e.name.begin(), e.name.end());
    rec.push_back('\0');
    while (rec.size() % 8 != 0) rec.push_back('\0');

    const auto length = static_cast<uint32_t>(rec.size());
    std::memcpy(rec.data(), &length, sizeof(length));
    return rec;
}

std::vector<char> packEntries(const std::vector<FakeEntry>& entries) {
    std::vector<char> buffer;
    for (const FakeEntry& e : entries) {
        const std::vector<char> rec = packEntry(e);
        buffer.insert(buffer.end(), rec.begin(), rec.end());
    }
    return buffer;
}

/// Copies `bytes` into an allocation of *exactly* that size, so that AddressSanitizer
/// reports any read past the end of the buffer.
std::unique_ptr<char[]> exactCopy(const std::vector<char>& bytes) {
    std::unique_ptr<char[]> copy(new char[bytes.size()]);
    std::memcpy(copy.get(), bytes.data(), bytes.size());
    return copy;
}

std::vector<FakeEntry> sampleEntries() {
    FakeEntry file;
    file.name = "Holiday Movie.MOV";
    file.fileId = 0x0FFFFFFF00001234ull;
    file.linkCount = 1;
    file.allocSize = 8ll << 30;
    file.dataLength = (8ll << 30) - 17;

    FakeEntry dir;
    dir.name = "Documents";
    dir.type = VDIR;
    dir.fileId = 42;
    dir.mtime = 1600000000;

    FakeEntry link;
    link.name = "shortcut";
    link.type = VLNK;
    link.dataLength = 11;

    FakeEntry broken;
    broken.name = "unreadable";
    broken.error = EACCES;

    FakeEntry utf8;
    utf8.name = "日本語のファイル名🙂.txt";
    utf8.fileId = 77;
    utf8.linkCount = 3;
    utf8.allocSize = 4096;
    utf8.dataLength = 5;
    return {file, dir, link, broken, utf8};
}

const MountGuard& defaultGuard() {
    static const MountGuard guard({"/", "/dev", "/System/Volumes/Data"}, "/", 16777233, false);
    return guard;
}

/// Lists one directory with getattrlistbulk() exactly as the scanner does.
std::vector<std::pair<std::string, BulkEntry>> bulkList(const std::string& dir, size_t bufferBytes, int* syscalls = nullptr) {
    std::vector<std::pair<std::string, BulkEntry>> out;
    const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return out;
    struct attrlist list;
    fillBulkAttrList(list);
    std::unique_ptr<char[]> buffer(new char[bufferBytes]);
    for (;;) {
        const int count = ::getattrlistbulk(fd, &list, buffer.get(), bufferBytes, bulkOptions());
        if (syscalls != nullptr) ++*syscalls;
        if (count <= 0) break;
        BulkEntryReader reader(buffer.get(), bufferBytes, count);
        BulkEntry entry;
        while (reader.next(entry)) {
            BulkEntry copy = entry;
            std::string name(entry.name);
            copy.name = {};
            out.emplace_back(std::move(name), copy);
        }
        if (reader.malformed() || reader.remaining() != 0) {
            out.clear();
            break;
        }
    }
    ::close(fd);
    return out;
}

// ----------------------------------------------------------------------------------------
// Independent reference walk (classic readdir + lstat)
// ----------------------------------------------------------------------------------------

struct WalkTotals {
    uint64_t files = 0;
    uint64_t directories = 0;
    uint64_t logical = 0;
    uint64_t allocated = 0;
    uint64_t hardlinkDups = 0;
    std::set<std::pair<dev_t, ino_t>> seenLinks;
};

void referenceWalk(const std::string& path, dev_t rootDev, WalkTotals& totals) {
    DIR* dir = ::opendir(path.c_str());
    if (dir == nullptr) return;
    ++totals.directories;
    std::vector<std::string> subdirs;
    while (const dirent* e = ::readdir(dir)) {
        if (std::strcmp(e->d_name, ".") == 0 || std::strcmp(e->d_name, "..") == 0) continue;
        const std::string child = path + "/" + e->d_name;
        struct stat st;
        if (::lstat(child.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            if (st.st_dev == rootDev) subdirs.push_back(child);
            continue;
        }
        ++totals.files;
        totals.logical += static_cast<uint64_t>(st.st_size);
        if (S_ISREG(st.st_mode) && st.st_nlink > 1 && !totals.seenLinks.insert({st.st_dev, st.st_ino}).second) {
            ++totals.hardlinkDups;
            continue;
        }
        totals.allocated += allocatedBytes(st);
    }
    ::closedir(dir);
    for (const std::string& sub : subdirs) referenceWalk(sub, rootDev, totals);
}

}  // namespace

// ----------------------------------------------------------------------------------------
// Decoder: layout, bounds, alignment
// ----------------------------------------------------------------------------------------

DP_TEST(bulk_attr_request_uses_the_documented_attribute_set) {
    struct attrlist list;
    fillBulkAttrList(list);
    CHECK_EQ(list.bitmapcount, static_cast<u_short>(ATTR_BIT_MAP_COUNT));
    CHECK_EQ(list.commonattr, static_cast<attrgroup_t>(ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_NAME | ATTR_CMN_ERROR |
                                                         ATTR_CMN_DEVID | ATTR_CMN_OBJTYPE | ATTR_CMN_FILEID |
                                                         ATTR_CMN_MODTIME | ATTR_CMN_FLAGS));
    CHECK_EQ(list.fileattr, static_cast<attrgroup_t>(ATTR_FILE_LINKCOUNT | ATTR_FILE_DATALENGTH | ATTR_FILE_ALLOCSIZE));
    CHECK_EQ(list.dirattr, static_cast<attrgroup_t>(ATTR_DIR_MOUNTSTATUS));
    CHECK_EQ(list.volattr, static_cast<attrgroup_t>(0));
    CHECK_EQ(list.forkattr, static_cast<attrgroup_t>(0));
    CHECK_EQ(bulkOptions(), static_cast<uint64_t>(FSOPT_NOFOLLOW | FSOPT_PACK_INVAL_ATTRS));
    // Canonical order is the numeric order of the attribute bits (ALLOCSIZE packs
    // before DATALENGTH, MODTIME and FLAGS before FILEID) — never the order in source.
    CHECK(ATTR_FILE_LINKCOUNT < ATTR_FILE_ALLOCSIZE);
    CHECK(ATTR_FILE_ALLOCSIZE < ATTR_FILE_DATALENGTH);
    CHECK(ATTR_CMN_NAME < ATTR_CMN_DEVID && ATTR_CMN_DEVID < ATTR_CMN_OBJTYPE);
    CHECK(ATTR_CMN_OBJTYPE < ATTR_CMN_MODTIME && ATTR_CMN_MODTIME < ATTR_CMN_FLAGS && ATTR_CMN_FLAGS < ATTR_CMN_FILEID);
}

DP_TEST(bulk_decoder_reads_every_field_of_synthetic_records) {
    const std::vector<FakeEntry> entries = sampleEntries();
    const std::vector<char> packed = packEntries(entries);
    const auto buffer = exactCopy(packed);

    BulkEntryReader reader(buffer.get(), packed.size(), static_cast<int>(entries.size()));
    BulkEntry e;

    REQUIRE(reader.next(e));
    CHECK_EQ(e.name, "Holiday Movie.MOV");
    CHECK_EQ(e.error, 0u);
    CHECK_EQ(e.dev_id, 16777233);
    CHECK_EQ(e.obj_type, kVnodeRegular);
    CHECK_EQ(e.mtime_sec, int64_t{1700000000});
    CHECK_EQ(e.file_id, uint64_t{0x0FFFFFFF00001234ull});
    CHECK_EQ(e.link_count, 1u);
    CHECK_EQ(e.alloc_size, uint64_t{8} << 30);
    CHECK_EQ(e.data_length, (uint64_t{8} << 30) - 17);
    CHECK(e.has(kBulkHasDevId | kBulkHasObjType | kBulkHasModTime | kBulkHasFlags | kBulkHasFileId |
                kBulkHasLinkCount | kBulkHasAllocSize | kBulkHasDataLength));
    CHECK(!e.has(kBulkHasMountStatus));

    REQUIRE(reader.next(e));
    CHECK_EQ(e.name, "Documents");
    CHECK_EQ(e.obj_type, kVnodeDirectory);
    CHECK_EQ(e.mtime_sec, int64_t{1600000000});
    CHECK_EQ(e.file_id, uint64_t{42});
    CHECK(e.has(kBulkHasMountStatus));
    CHECK(!e.has(kBulkHasAllocSize));
    CHECK_EQ(e.alloc_size, uint64_t{0});

    REQUIRE(reader.next(e));
    CHECK_EQ(e.name, "shortcut");
    CHECK_EQ(e.obj_type, kVnodeSymlink);
    CHECK_EQ(e.data_length, uint64_t{11});

    REQUIRE(reader.next(e));
    CHECK_EQ(e.name, "unreadable");
    CHECK_EQ(e.error, static_cast<uint32_t>(EACCES));
    CHECK_EQ(e.valid, 0u);  // short record: nothing but the name

    REQUIRE(reader.next(e));
    CHECK_EQ(e.name, "日本語のファイル名🙂.txt");
    CHECK_EQ(e.link_count, 3u);
    CHECK_EQ(e.alloc_size, uint64_t{4096});

    CHECK(!reader.next(e));
    CHECK(!reader.malformed());
    CHECK_EQ(reader.remaining(), 0);
}

DP_TEST(bulk_decoder_layout_offsets_match_the_kernel) {
    // A hand-built record written at the exact byte offsets documented in BulkAttr.hpp.
    FakeEntry file;
    file.name = "x";
    file.dev = 0x01020304;
    file.flags = 0x0A0B0C0D;
    file.fileId = 0x1112131415161718ull;
    file.linkCount = 0x21222324;
    file.allocSize = 0x3132333435363738ll;
    file.dataLength = 0x4142434445464748ll;
    const std::vector<char> rec = packEntry(file);
    REQUIRE(rec.size() >= kBulkFileFixedSize + 2);

    const auto u32At = [&rec](size_t offset) {
        uint32_t v;
        std::memcpy(&v, rec.data() + offset, sizeof(v));
        return v;
    };
    const auto u64At = [&rec](size_t offset) {
        uint64_t v;
        std::memcpy(&v, rec.data() + offset, sizeof(v));
        return v;
    };
    CHECK_EQ(u32At(0), static_cast<uint32_t>(rec.size()));  // length
    CHECK_EQ(u32At(24), 0u);                                // error, directly after the returned set
    CHECK_EQ(u32At(28), static_cast<uint32_t>(kBulkFileFixedSize - 28));  // name offset, relative to itself
    CHECK_EQ(u32At(36), 0x01020304u);                       // devid
    CHECK_EQ(u32At(40), static_cast<uint32_t>(VREG));       // objtype
    CHECK_EQ(u64At(44), uint64_t{1700000000});              // modtime.tv_sec (only 4-byte aligned!)
    CHECK_EQ(u32At(60), 0x0A0B0C0Du);                       // flags
    CHECK_EQ(u64At(64), uint64_t{0x1112131415161718ull});   // fileid
    CHECK_EQ(u32At(72), 0x21222324u);                       // linkcount
    CHECK_EQ(u64At(76), uint64_t{0x3132333435363738ull});   // allocsize precedes…
    CHECK_EQ(u64At(84), uint64_t{0x4142434445464748ull});   // …datalength
    CHECK_EQ(rec[92], 'x');

    FakeEntry dir;
    dir.name = "d";
    dir.type = VDIR;
    dir.mountStatus = DIR_MNTSTATUS_MNTPOINT;
    const std::vector<char> dirRec = packEntry(dir);
    uint32_t mount;
    std::memcpy(&mount, dirRec.data() + 72, sizeof(mount));
    CHECK_EQ(mount, static_cast<uint32_t>(DIR_MNTSTATUS_MNTPOINT));
    CHECK_EQ(dirRec[kBulkDirectoryFixedSize], 'd');

    BulkEntryReader reader(rec.data(), rec.size(), 1);
    BulkEntry e;
    REQUIRE(reader.next(e));
    CHECK_EQ(e.alloc_size, uint64_t{0x3132333435363738ull});
    CHECK_EQ(e.data_length, uint64_t{0x4142434445464748ull});
    CHECK_EQ(e.bsd_flags, 0x0A0B0C0Du);
}

DP_TEST(bulk_decoder_tolerates_unaligned_buffers) {
    const std::vector<FakeEntry> entries = sampleEntries();
    const std::vector<char> packed = packEntries(entries);
    // Decode the same bytes from every alignment 0..7. Reading a uint64_t through a
    // typed pointer here would be undefined behaviour and UBSan would abort the test.
    for (size_t shift = 0; shift < 8; ++shift) {
        std::unique_ptr<char[]> storage(new char[packed.size() + shift]);
        std::memcpy(storage.get() + shift, packed.data(), packed.size());
        BulkEntryReader reader(storage.get() + shift, packed.size(), static_cast<int>(entries.size()));
        BulkEntry e;
        size_t index = 0;
        while (reader.next(e)) {
            CHECK_EQ(e.name, entries[index].name);
            if (entries[index].error == 0) {
                CHECK_EQ(e.file_id, entries[index].fileId);
                CHECK_EQ(e.mtime_sec, entries[index].mtime);
            }
            ++index;
        }
        CHECK_EQ(index, entries.size());
        CHECK(!reader.malformed());
    }
}

DP_TEST(bulk_decoder_never_reads_past_a_truncated_buffer) {
    const std::vector<FakeEntry> entries = sampleEntries();
    const std::vector<char> packed = packEntries(entries);
    // Every possible truncation point, each in an exact-size allocation: the decoder
    // must stop cleanly (ASan would catch a single stray byte read).
    for (size_t cut = 0; cut < packed.size(); ++cut) {
        const std::vector<char> truncated(packed.begin(), packed.begin() + static_cast<std::ptrdiff_t>(cut));
        const auto buffer = exactCopy(truncated);
        BulkEntryReader reader(buffer.get(), cut, static_cast<int>(entries.size()));
        BulkEntry e;
        size_t decoded = 0;
        while (reader.next(e)) {
            // Whatever was decoded must be a complete record with the right name.
            if (decoded < entries.size()) CHECK_EQ(e.name, entries[decoded].name);
            ++decoded;
        }
        CHECK(decoded < entries.size());
        CHECK(reader.malformed());
    }
}

DP_TEST(bulk_decoder_rejects_corrupt_lengths_and_offsets) {
    FakeEntry file;
    file.name = "victim.bin";
    file.allocSize = 4096;
    file.dataLength = 100;
    const std::vector<char> good = packEntry(file);
    BulkEntry e;

    const auto withU32 = [&good](size_t offset, uint32_t value) {
        std::vector<char> copy = good;
        std::memcpy(copy.data() + offset, &value, sizeof(value));
        return copy;
    };
    const auto decodeOne = [&e](const std::vector<char>& bytes) {
        const auto buffer = exactCopy(bytes);
        BulkEntryReader reader(buffer.get(), bytes.size(), 1);
        const bool ok = reader.next(e);
        return std::make_pair(ok, reader.malformed());
    };

    CHECK(decodeOne(good) == std::make_pair(true, false));

    // Record length: zero, shorter than the header, longer than the buffer, huge.
    for (uint32_t badLength : {0u, 4u, 35u, static_cast<uint32_t>(good.size() + 1), 0x7FFFFFFFu, 0xFFFFFFFFu}) {
        CHECK(decodeOne(withU32(0, badLength)) == std::make_pair(false, true));
    }
    // Name reference: negative offset, offset inside the header, offset beyond the record.
    for (uint32_t badOffset : {static_cast<uint32_t>(-8), 0u, 4u, static_cast<uint32_t>(good.size()), 0x7FFFFFFFu}) {
        CHECK(decodeOne(withU32(28, badOffset)) == std::make_pair(false, true));
    }
    // Name length running past the end of the record.
    CHECK(decodeOne(withU32(32, static_cast<uint32_t>(good.size()))) == std::make_pair(false, true));
    CHECK(decodeOne(withU32(32, 0xFFFFFFFFu)) == std::make_pair(false, true));

    // A name without a terminating NUL is clamped to the declared length.
    {
        std::vector<char> copy = good;
        const size_t nameStart = kBulkFileFixedSize;
        const auto declared = static_cast<uint32_t>(copy.size() - nameStart);
        std::memcpy(copy.data() + 32, &declared, sizeof(declared));
        for (size_t i = nameStart; i < copy.size(); ++i) copy[i] = 'Z';
        const auto result = decodeOne(copy);
        CHECK(result.first && !result.second);
        CHECK_EQ(e.name.size(), static_cast<size_t>(declared));
    }

    // A name offset pointing into the fixed area shortens that area: fields that no
    // longer fit are simply not reported as valid (and never read out of bounds).
    {
        const auto result = decodeOne(withU32(28, static_cast<uint32_t>(kBulkCommonSize - 28)));
        CHECK(result.first && !result.second);
        CHECK(e.has(kBulkHasFileId));
        CHECK(!e.has(kBulkHasLinkCount));
        CHECK(!e.has(kBulkHasAllocSize));
        CHECK_EQ(e.alloc_size, uint64_t{0});
    }

    // Negative sizes are never turned into huge unsigned byte counts.
    {
        std::vector<char> copy = good;
        const int64_t negative = -4096;
        std::memcpy(copy.data() + 76, &negative, sizeof(negative));
        std::memcpy(copy.data() + 84, &negative, sizeof(negative));
        const auto result = decodeOne(copy);
        CHECK(result.first && !result.second);
        CHECK_EQ(e.alloc_size, uint64_t{0});
        CHECK_EQ(e.data_length, uint64_t{0});
        CHECK(!e.has(kBulkHasAllocSize));
    }

    // More entries claimed than the buffer holds.
    {
        const auto buffer = exactCopy(good);
        BulkEntryReader reader(buffer.get(), good.size(), 5);
        CHECK(reader.next(e));
        CHECK(!reader.next(e));
        CHECK(reader.malformed());
    }
}

DP_TEST(bulk_decoder_survives_random_corruption) {
    const std::vector<FakeEntry> entries = sampleEntries();
    const std::vector<char> packed = packEntries(entries);
    uint64_t state = 0x1234567;
    const auto next = [&state]() {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    };
    size_t decodedTotal = 0;
    for (int round = 0; round < 20000; ++round) {
        std::vector<char> mutated = packed;
        const int flips = 1 + static_cast<int>(next() % 6);
        for (int i = 0; i < flips; ++i) mutated[next() % mutated.size()] = static_cast<char>(next());
        const size_t size = (round % 3 == 0) ? next() % (mutated.size() + 1) : mutated.size();
        mutated.resize(size);
        const auto buffer = exactCopy(mutated);

        BulkEntryReader reader(buffer.get(), size, static_cast<int>(entries.size()));
        BulkEntry e;
        while (reader.next(e)) {
            // Touch everything that was decoded so ASan validates the name bounds too.
            decodedTotal += e.name.size();
            for (const char c : e.name) decodedTotal += static_cast<unsigned char>(c) & 1u;
            const EntryClass cls = classifyEntry(e, defaultGuard());
            decodedTotal += cls.descend ? 1 : 0;
        }
    }
    CHECK(decodedTotal > 0);
}

DP_TEST(bulk_decoder_agrees_with_lstat_on_real_directories) {
    TempDir tmp;
    tmp.mkdir("sub");
    tmp.writeFile("small.txt", 10);
    tmp.writeFile("medium.bin", 300 * 1024);
    tmp.writeSparseFile("sparse.img", 32 << 20, 4096);
    tmp.writeFile("linked.dat", 20000);
    tmp.hardLink("linked.dat", "linked-again.dat");
    tmp.symLink("small.txt", "alias");
    tmp.symLink("/nonexistent/target", "dangling");

    for (const std::string& dir : {tmp.path(), std::string("/usr/share"), std::string("/usr/bin")}) {
        const auto listed = bulkList(dir, 128 * 1024);
        REQUIRE(!listed.empty());
        size_t compared = 0;
        size_t firmlinks = 0;
        for (const auto& [name, e] : listed) {
            struct stat st;
            if (::lstat((dir + "/" + name).c_str(), &st) != 0) continue;
            CHECK_EQ(e.error, 0u);
            CHECK_EQ(e.dev_id, st.st_dev);
            if ((e.bsd_flags & kBsdFlagFirmlink) != 0) {
                // Documented difference: getattrlistbulk describes the firmlink itself,
                // lstat its target on the data volume (e.g. /usr/share/snmp).
                CHECK(S_ISDIR(st.st_mode));
                CHECK(e.file_id != static_cast<uint64_t>(st.st_ino));
                ++firmlinks;
                ++compared;
                continue;
            }
            CHECK_EQ(e.file_id, static_cast<uint64_t>(st.st_ino));
            CHECK_EQ(e.mtime_sec, static_cast<int64_t>(st.st_mtimespec.tv_sec));
            CHECK_EQ(e.bsd_flags, st.st_flags);
            if (S_ISDIR(st.st_mode)) {
                CHECK_EQ(e.obj_type, kVnodeDirectory);
                CHECK(e.has(kBulkHasMountStatus));
                CHECK(!e.has(kBulkHasAllocSize));
            } else {
                CHECK_EQ(e.obj_type, S_ISLNK(st.st_mode) ? kVnodeSymlink : kVnodeRegular);
                CHECK(e.has(kBulkHasLinkCount | kBulkHasAllocSize | kBulkHasDataLength));
                CHECK_EQ(e.link_count, static_cast<uint32_t>(st.st_nlink));
                CHECK_EQ(e.data_length, static_cast<uint64_t>(st.st_size));
                CHECK_EQ(e.alloc_size, allocatedBytes(st));
            }
            ++compared;
        }
        CHECK_EQ(compared, listed.size());
        std::printf("      %s: %zu entries identical to lstat (%zu firmlink%s)\n", dir.c_str(), compared - firmlinks,
                    firmlinks, firmlinks == 1 ? "" : "s");
    }
}

DP_TEST(bulk_listing_spans_multiple_syscalls_with_a_small_buffer) {
    TempDir tmp;
    constexpr int kFiles = 300;
    for (int i = 0; i < kFiles; ++i) tmp.writeFile("file-with-a-reasonably-long-name-" + std::to_string(i) + ".dat", 1);
    int syscalls = 0;
    const auto listed = bulkList(tmp.path(), 4096, &syscalls);
    CHECK_EQ(listed.size(), size_t{kFiles});
    CHECK(syscalls > 5);  // several batches plus the terminating empty one
    std::set<std::string> names;
    for (const auto& entry : listed) names.insert(entry.first);
    CHECK_EQ(names.size(), size_t{kFiles});
}

// ----------------------------------------------------------------------------------------
// Accounting rules (classifier)
// ----------------------------------------------------------------------------------------

DP_TEST(dataless_cloud_files_occupy_no_physical_space) {
    FakeEntry cloud;
    cloud.name = "Evicted Video.mov";
    cloud.flags = kBsdFlagDataless;       // SF_DATALESS
    cloud.allocSize = 5ll << 30;          // even if a file system claimed local blocks…
    cloud.dataLength = 5ll << 30;
    const std::vector<char> rec = packEntry(cloud);
    BulkEntryReader reader(rec.data(), rec.size(), 1);
    BulkEntry e;
    REQUIRE(reader.next(e));

    const EntryClass cls = classifyEntry(e, defaultGuard());
    CHECK(cls.readable);
    CHECK((cls.flags & kNodeDataless) != 0);
    CHECK_EQ(cls.allocated_bytes, uint64_t{0});          // …none is counted,
    CHECK_EQ(cls.logical_bytes, uint64_t{5} << 30);      // while the logical size is preserved.
    CHECK((cls.flags & kNodeDir) == 0);
#ifdef SF_DATALESS
    CHECK_EQ(kBsdFlagDataless, static_cast<uint32_t>(SF_DATALESS));
#endif
    CHECK_EQ(kBsdFlagDataless, 0x40000000u);

    // A local file with the same sizes keeps its physical footprint.
    cloud.flags = 0;
    const std::vector<char> localRec = packEntry(cloud);
    BulkEntryReader localReader(localRec.data(), localRec.size(), 1);
    REQUIRE(localReader.next(e));
    const EntryClass local = classifyEntry(e, defaultGuard());
    CHECK_EQ(local.allocated_bytes, uint64_t{5} << 30);
    CHECK((local.flags & kNodeDataless) == 0);

    // A dataless directory is recorded but never entered (that would trigger a download).
    FakeEntry cloudDir;
    cloudDir.name = "iCloud Folder";
    cloudDir.type = VDIR;
    cloudDir.flags = kBsdFlagDataless;
    const std::vector<char> dirRec = packEntry(cloudDir);
    BulkEntryReader dirReader(dirRec.data(), dirRec.size(), 1);
    REQUIRE(dirReader.next(e));
    const EntryClass dirCls = classifyEntry(e, defaultGuard());
    CHECK((dirCls.flags & kNodeDir) != 0);
    CHECK((dirCls.flags & kNodeDataless) != 0);
    CHECK(!dirCls.descend);
}

DP_TEST(classifier_handles_links_special_files_and_errors) {
    BulkEntry e;
    const auto classify = [&e](const FakeEntry& fake) {
        static std::vector<char> rec;
        rec = packEntry(fake);
        BulkEntryReader reader(rec.data(), rec.size(), 1);
        if (!reader.next(e)) std::abort();
        return classifyEntry(e, defaultGuard());
    };

    FakeEntry dir;
    dir.name = "plain";
    dir.type = VDIR;
    EntryClass cls = classify(dir);
    CHECK(cls.descend);
    CHECK_EQ(cls.flags, static_cast<uint32_t>(kNodeDir));
    CHECK_EQ(cls.allocated_bytes, uint64_t{0});

    FakeEntry firmlink = dir;
    firmlink.name = "Users";
    firmlink.flags = kBsdFlagFirmlink;
    cls = classify(firmlink);
    CHECK(cls.descend);  // firmlinks are the one route into the data volume
    CHECK((cls.flags & kNodeFirmlink) != 0);

    FakeEntry symlink;
    symlink.name = "link";
    symlink.type = VLNK;
    symlink.dataLength = 9;
    cls = classify(symlink);
    CHECK(!cls.descend);
    CHECK((cls.flags & kNodeSymlink) != 0);
    CHECK_EQ(cls.logical_bytes, uint64_t{9});

    FakeEntry socket;
    socket.name = "sock";
    socket.type = VSOCK;
    cls = classify(socket);
    CHECK((cls.flags & kNodeSpecial) != 0);
    CHECK(!cls.hardlink_candidate);

    FakeEntry hardlinked;
    hardlinked.name = "twice";
    hardlinked.linkCount = 2;
    hardlinked.allocSize = 8192;
    cls = classify(hardlinked);
    CHECK(cls.hardlink_candidate);
    CHECK((cls.flags & kNodeHardlink) != 0);
    CHECK_EQ(cls.allocated_bytes, uint64_t{8192});

    FakeEntry denied;
    denied.name = "secret";
    denied.error = EPERM;
    cls = classify(denied);
    CHECK(!cls.readable);
    CHECK(cls.permission_denied);
    CHECK((cls.flags & kNodeAccessDenied) != 0);

    FakeEntry ioError;
    ioError.name = "broken";
    ioError.error = EIO;
    cls = classify(ioError);
    CHECK(!cls.readable);
    CHECK(!cls.permission_denied);
    CHECK((cls.flags & kNodeIOError) != 0);
}

// ----------------------------------------------------------------------------------------
// Volume boundaries and firmlinks
// ----------------------------------------------------------------------------------------

DP_TEST(mount_guard_policy_with_a_synthetic_mount_table) {
    const std::vector<std::string> table = {
        "/", "/dev", "/System/Volumes/VM", "/System/Volumes/Preboot", "/System/Volumes/Update",
        "/System/Volumes/Data", "/System/Volumes/Data/home", "/Volumes/Backup",
        "/Library/Developer/CoreSimulator/Volumes/iOS_21A328",
    };
    const MountGuard guard(table, "/", 100, false);

    // 1. Kernel mount status.
    CHECK(guard.statusIsBoundary(kMountStatusMountPoint));
    CHECK(guard.statusIsBoundary(kMountStatusTrigger));
    CHECK(guard.statusIsBoundary(kMountStatusMountPoint | kMountStatusTrigger));
    CHECK(!guard.statusIsBoundary(0));

    // 2. Mount table: the data volume (and its siblings) must never be entered from "/",
    //    because firmlinks already expose its contents under /Users, /Applications, …
    CHECK(guard.pathIsBoundary("/System/Volumes/Data"));
    CHECK(guard.pathIsBoundary("/System/Volumes/VM"));
    CHECK(guard.pathIsBoundary("/System/Volumes/Preboot"));
    CHECK(guard.pathIsBoundary("/dev"));
    CHECK(guard.pathIsBoundary("/Volumes/Backup"));
    CHECK(!guard.pathIsBoundary("/"));                 // the scan root itself
    CHECK(!guard.pathIsBoundary("/Users"));            // firmlink: traversed
    CHECK(!guard.pathIsBoundary("/System/Volumes"));   // ordinary directory
    CHECK(!guard.pathIsBoundary("/Volumes"));

    // 3. Device IDs.
    CHECK(guard.deviceAllowed(100));
    CHECK(!guard.deviceAllowed(101));

    // The same checks through the classifier.
    FakeEntry dataVolume;
    dataVolume.name = "Data";
    dataVolume.type = VDIR;
    dataVolume.dev = 100;  // same *unified* device ID as "/": only the mount status tells
    dataVolume.mountStatus = kMountStatusMountPoint;
    std::vector<char> rec = packEntry(dataVolume);
    BulkEntry e;
    {
        BulkEntryReader reader(rec.data(), rec.size(), 1);
        REQUIRE(reader.next(e));
    }
    EntryClass cls = classifyEntry(e, guard);
    CHECK((cls.flags & kNodeMountPoint) != 0);
    CHECK(!cls.descend);
    CHECK(!cls.foreign_device);

    FakeEntry foreign;
    foreign.name = "on-another-disk.bin";
    foreign.dev = 555;
    rec = packEntry(foreign);
    {
        BulkEntryReader reader(rec.data(), rec.size(), 1);
        REQUIRE(reader.next(e));
    }
    CHECK(classifyEntry(e, guard).foreign_device);

    // An explicitly targeted volume is scanned even though its root is a mount point…
    const MountGuard targeted(table, "/Volumes/Backup", 300, false);
    CHECK(!targeted.pathIsBoundary("/Volumes/Backup"));
    CHECK(targeted.deviceAllowed(300));
    CHECK(!targeted.deviceAllowed(100));

    // …a scan that starts inside the data volume still recognises mounts that the kernel
    // reports by their firmlink path…
    const MountGuard insideData(table, "/System/Volumes/Data", 100, false);
    CHECK(!insideData.pathIsBoundary("/System/Volumes/Data"));
    CHECK(insideData.pathIsBoundary("/System/Volumes/Data/home"));
    CHECK(insideData.pathIsBoundary("/System/Volumes/Data/Library/Developer/CoreSimulator/Volumes/iOS_21A328"));

    // …and crossing can be requested explicitly.
    const MountGuard crossing(table, "/", 100, true);
    CHECK(!crossing.statusIsBoundary(kMountStatusMountPoint));
    CHECK(!crossing.pathIsBoundary("/System/Volumes/Data"));
    CHECK(crossing.deviceAllowed(555));
    {
        BulkEntryReader reader(rec.data(), rec.size(), 1);
        REQUIRE(reader.next(e));
    }
    CHECK(!classifyEntry(e, crossing).foreign_device);
}

DP_TEST(scanning_never_enters_mounted_volumes_on_this_system) {
    struct stat st;
    if (::stat("/System/Volumes/Data", &st) != 0) {
        std::printf("      (no /System/Volumes/Data on this system — live check skipped)\n");
        return;
    }
    // The data volume is mounted here *with the same unified device ID as "/"*, which is
    // exactly why a plain st_dev comparison would double-count it.
    struct stat rootStat;
    REQUIRE(::stat("/", &rootStat) == 0);
    std::printf("      st_dev: / = %d, /System/Volumes/Data = %d\n", rootStat.st_dev, st.st_dev);

    ScanFixture scan("/System/Volumes");
    REQUIRE(scan.result.status == ScanStatus::Completed);
    const FlatArena& arena = scan.arena;

    const uint32_t data = findNode(arena, "Data");
    REQUIRE(data != kInvalidNode);
    CHECK(arena.node(data).is_mount_point());
    CHECK_EQ(arena.node(data).file_count, 0u);
    CHECK_EQ(arena.node(data).child_count, 0u);
    CHECK_EQ(arena.node(data).allocated_bytes, uint64_t{0});

    // Every mounted volume below /System/Volumes was recorded as a boundary, not entered.
    uint64_t mountedChildren = 0;
    for (const VolumeInfo& volume : listAllVolumes()) {
        const std::string prefix = "/System/Volumes/";
        if (volume.mount_point.compare(0, prefix.size(), prefix) != 0) continue;
        const std::string name = volume.mount_point.substr(prefix.size());
        if (name.find('/') != std::string::npos) continue;
        const uint32_t node = findNode(arena, name);
        REQUIRE(node != kInvalidNode);
        CHECK(arena.node(node).is_mount_point());
        CHECK_EQ(arena.node(node).child_count, 0u);
        ++mountedChildren;
    }
    CHECK(mountedChildren >= 1);
    CHECK(scan.result.totals.mount_points_skipped >= mountedChildren);
    // What is left is a handful of empty placeholder directories, not 100+ GB of data.
    CHECK(arena.node(arena.root()).file_count < 1000u);
    CHECK(treeIsConsistent(arena));
}

DP_TEST(firmlinks_are_followed_exactly_once) {
    struct stat usrLocal;
    if (::lstat("/usr/local", &usrLocal) != 0 || !S_ISDIR(usrLocal.st_mode)) {
        std::printf("      (/usr/local not present — live check skipped)\n");
        return;
    }
    const auto listed = bulkList("/usr", 64 * 1024);
    const auto it = std::find_if(listed.begin(), listed.end(), [](const auto& p) { return p.first == "local"; });
    REQUIRE(it != listed.end());
    if ((it->second.bsd_flags & kBsdFlagFirmlink) == 0) {
        std::printf("      (/usr/local is not a firmlink on this system — live check skipped)\n");
        return;
    }
    // getattrlistbulk reports the firmlink itself, not its target, and the target shares
    // the root volume's unified device ID.
    CHECK(it->second.file_id != static_cast<uint64_t>(usrLocal.st_ino));
    CHECK_EQ(it->second.dev_id, usrLocal.st_dev);
    CHECK_EQ(it->second.mount_status, 0u);

    ScanFixture scan("/usr");
    REQUIRE(scan.result.status == ScanStatus::Completed);
    const uint32_t local = findNode(scan.arena, "local");
    REQUIRE(local != kInvalidNode);
    CHECK((scan.arena.node(local).flags & kNodeFirmlink) != 0);
    CHECK(!scan.arena.node(local).is_mount_point());

    // The firmlink was traversed: its children are exactly what readdir shows.
    std::set<std::string> expected;
    if (DIR* dir = ::opendir("/usr/local")) {
        while (const dirent* e = ::readdir(dir)) {
            if (std::strcmp(e->d_name, ".") != 0 && std::strcmp(e->d_name, "..") != 0) expected.insert(e->d_name);
        }
        ::closedir(dir);
    }
    std::set<std::string> actual;
    scan.arena.forEachChild(local, [&](uint32_t c) { actual.insert(std::string(scan.arena.name(c))); });
    CHECK(actual == expected);
    CHECK(treeIsConsistent(scan.arena));
}

// ----------------------------------------------------------------------------------------
// Scanner on fixtures
// ----------------------------------------------------------------------------------------

DP_TEST(scan_builds_the_exact_tree_of_a_nested_fixture) {
    TempDir tmp;
    tmp.mkdir("alpha");
    tmp.mkdir("alpha/beta");
    tmp.mkdir("alpha/beta/gamma");
    tmp.mkdir("empty");
    tmp.writeFile("top.txt", 1234);
    tmp.writeFile("alpha/one.bin", 70000);
    tmp.writeFile("alpha/beta/two.MOV", 200000);
    tmp.writeFile("alpha/beta/gamma/three", 5);
    tmp.writeFile("alpha/beta/gamma/four.tar.gz", 12345);

    ScanFixture scan(tmp.path());
    REQUIRE(scan.result.status == ScanStatus::Completed);
    const FlatArena& arena = scan.arena;
    CHECK_EQ(scan.result.root_path, tmp.path());
    CHECK_EQ(arena.name(arena.root()), tmp.path());
    CHECK_EQ(arena.liveNodeCount(), 10u);  // root + 4 directories + 5 files
    CHECK_EQ(scan.result.totals.files, uint64_t{5});
    CHECK_EQ(scan.result.totals.directories, uint64_t{5});
    CHECK_EQ(scan.result.totals.denied, uint64_t{0});
    CHECK_EQ(scan.result.totals.errors, uint64_t{0});
    CHECK(scan.result.totals.finished);
    // Two getattrlistbulk calls per non-empty directory (entries, then "no more"), one for the empty one.
    CHECK_EQ(scan.result.totals.syscalls, uint64_t{4 * 2 + 1});

    const uint32_t root = arena.root();
    CHECK_EQ(arena.node(root).file_count, 5u);
    CHECK_EQ(arena.node(root).logical_bytes, uint64_t{1234 + 70000 + 200000 + 5 + 12345});
    CHECK_EQ(arena.node(root).child_count, 3u);

    // Every node agrees with lstat: sizes, modification time, type.
    uint64_t expectedAllocated = 0;
    for (const char* relative : {"top.txt", "alpha/one.bin", "alpha/beta/two.MOV", "alpha/beta/gamma/three",
                                 "alpha/beta/gamma/four.tar.gz"}) {
        const uint32_t node = findNode(arena, relative);
        REQUIRE(node != kInvalidNode);
        const struct stat st = lstatOrDie(tmp.sub(relative));
        CHECK(!arena.node(node).is_dir());
        CHECK_EQ(arena.node(node).logical_bytes, static_cast<uint64_t>(st.st_size));
        CHECK_EQ(arena.node(node).allocated_bytes, allocatedBytes(st));
        CHECK_EQ(arena.node(node).mtime, static_cast<int64_t>(st.st_mtimespec.tv_sec));
        CHECK_EQ(arena.node(node).file_count, 1u);
        CHECK_EQ(arena.path(node), tmp.sub(relative));
        expectedAllocated += allocatedBytes(st);
    }
    CHECK_EQ(arena.node(root).allocated_bytes, expectedAllocated);
    CHECK_EQ(scan.result.totals.allocated_bytes, expectedAllocated);
    CHECK_EQ(scan.result.totals.logical_bytes, arena.node(root).logical_bytes);

    const uint32_t beta = findNode(arena, "alpha/beta");
    REQUIRE(beta != kInvalidNode);
    CHECK(arena.node(beta).is_dir());
    CHECK_EQ(arena.node(beta).file_count, 3u);
    CHECK_EQ(arena.node(beta).logical_bytes, uint64_t{200000 + 5 + 12345});
    CHECK_EQ(arena.node(findNode(arena, "empty")).child_count, 0u);
    CHECK(treeIsConsistent(arena));
    CHECK(arena.isSorted());

    // Extensions were interned while scanning ("two.MOV" folds to "mov").
    const ExtensionTable& table = arena.extensions();
    CHECK_EQ(table.name(arena.node(findNode(arena, "alpha/beta/two.MOV")).ext_id), "mov");
    CHECK_EQ(table.name(arena.node(findNode(arena, "alpha/beta/gamma/four.tar.gz")).ext_id), "gz");
    CHECK_EQ(arena.node(findNode(arena, "alpha/beta/gamma/three")).ext_id, ExtensionTable::kNone);
    CHECK_EQ(table.totals().logical_bytes, arena.node(root).logical_bytes);
    CHECK_EQ(table.totals().file_count, uint64_t{5});
}

DP_TEST(sparse_file_allocates_less_than_its_logical_size) {
    TempDir tmp;
    constexpr off_t kLogical = off_t{256} << 20;  // 256 MiB apparent size
    tmp.writeSparseFile("sparse.img", kLogical, 8192);
    tmp.writeFile("dense.bin", 1 << 20);

    ScanFixture scan(tmp.path());
    REQUIRE(scan.result.status == ScanStatus::Completed);
    const uint32_t sparse = findNode(scan.arena, "sparse.img");
    const uint32_t dense = findNode(scan.arena, "dense.bin");
    REQUIRE(sparse != kInvalidNode && dense != kInvalidNode);

    const struct stat st = lstatOrDie(tmp.sub("sparse.img"));
    CHECK_EQ(scan.arena.node(sparse).logical_bytes, static_cast<uint64_t>(kLogical));
    CHECK_EQ(scan.arena.node(sparse).allocated_bytes, allocatedBytes(st));
    if (onApfs(tmp.path())) {
        CHECK(scan.arena.node(sparse).allocated_bytes < scan.arena.node(sparse).logical_bytes);
        CHECK(scan.arena.node(sparse).allocated_bytes <= uint64_t{1} << 20);
        std::printf("      sparse.img: logical %llu bytes, allocated %llu bytes\n",
                    static_cast<unsigned long long>(scan.arena.node(sparse).logical_bytes),
                    static_cast<unsigned long long>(scan.arena.node(sparse).allocated_bytes));
    } else {
        std::printf("      (temporary directory is not on APFS — sparse assertion skipped)\n");
    }
    CHECK(scan.arena.node(dense).allocated_bytes >= uint64_t{1} << 20);

    // Sorting follows the metric: physically the dense file is larger, logically the sparse one.
    CHECK_EQ(scan.arena.name(scan.arena.firstChild(scan.arena.root())), "dense.bin");
    scan.arena.sortChildren(SizeMetric::Logical);
    CHECK_EQ(scan.arena.name(scan.arena.firstChild(scan.arena.root())), "sparse.img");
}

DP_TEST(hard_links_count_their_physical_bytes_exactly_once) {
    TempDir tmp;
    tmp.mkdir("a");
    tmp.mkdir("b");
    tmp.mkdir("b/c");
    constexpr size_t kSize = 512 * 1024;
    tmp.writeFile("a/original.bin", kSize);
    tmp.hardLink("a/original.bin", "a/second-name.bin");
    tmp.hardLink("a/original.bin", "b/third-name.bin");
    tmp.hardLink("a/original.bin", "b/c/fourth-name.bin");
    tmp.writeFile("b/unrelated.bin", 100 * 1024);
    tmp.writeFile("pair-1.dat", 64 * 1024);
    tmp.hardLink("pair-1.dat", "pair-2.dat");

    const struct stat original = lstatOrDie(tmp.sub("a/original.bin"));
    const struct stat unrelated = lstatOrDie(tmp.sub("b/unrelated.bin"));
    const struct stat pair = lstatOrDie(tmp.sub("pair-1.dat"));
    CHECK_EQ(original.st_nlink, nlink_t{4});
    const uint64_t expectedAllocated = allocatedBytes(original) + allocatedBytes(unrelated) + allocatedBytes(pair);

    // Repeat the scan: whichever thread sees which name first, the totals must not change.
    for (int round = 0; round < 8; ++round) {
        ScanOptions options;
        options.thread_count = (round % 2 == 0) ? 8 : 1;
        ScanFixture scan(tmp.path(), options);
        REQUIRE(scan.result.status == ScanStatus::Completed);
        const FlatArena& arena = scan.arena;
        const uint32_t root = arena.root();

        CHECK_EQ(arena.node(root).allocated_bytes, expectedAllocated);
        CHECK_EQ(scan.result.totals.allocated_bytes, expectedAllocated);
        CHECK_EQ(scan.result.totals.hardlink_duplicates, uint64_t{4});  // 3 extra names + 1 extra name
        CHECK_EQ(arena.node(root).file_count, 7u);
        // Logical size stays per name, as `ls -l` shows it.
        CHECK_EQ(arena.node(root).logical_bytes, uint64_t{4 * kSize + 100 * 1024 + 2 * 64 * 1024});

        uint32_t carriers = 0;
        uint32_t duplicates = 0;
        for (const char* name : {"a/original.bin", "a/second-name.bin", "b/third-name.bin", "b/c/fourth-name.bin"}) {
            const uint32_t node = findNode(arena, name);
            REQUIRE(node != kInvalidNode);
            const FsNode& n = arena.node(node);
            CHECK((n.flags & kNodeHardlink) != 0);
            CHECK_EQ(n.logical_bytes, uint64_t{kSize});
            if (n.is_hardlink_dup()) {
                ++duplicates;
                CHECK_EQ(n.allocated_bytes, uint64_t{0});
            } else {
                ++carriers;
                CHECK_EQ(n.allocated_bytes, allocatedBytes(original));
            }
        }
        CHECK_EQ(carriers, 1u);
        CHECK_EQ(duplicates, 3u);
        const FsNode& single = arena.node(findNode(arena, "b/unrelated.bin"));
        CHECK((single.flags & (kNodeHardlink | kNodeHardlinkDup)) == 0);
        CHECK(treeIsConsistent(arena));
        CHECK_EQ(arena.extensions().totals().allocated_bytes, expectedAllocated);
    }
}

DP_TEST(circular_symlinks_cannot_trap_the_scanner) {
    TempDir tmp;
    tmp.mkdir("ring");
    tmp.mkdir("ring/inner");
    tmp.writeFile("ring/inner/payload.bin", 4096);
    tmp.symLink("b", "ring/a");                       // a -> b -> a
    tmp.symLink("a", "ring/b");
    tmp.symLink(".", "ring/self");                     // points at its own directory
    tmp.symLink("..", "ring/inner/up");                // points at the parent
    tmp.symLink(tmp.path(), "ring/inner/to-root");     // absolute link back to the scan root
    tmp.symLink("inner", "ring/dir-alias");            // link to a real directory
    tmp.symLink("/nonexistent", "ring/dangling");

    ScanFixture scan(tmp.path());
    REQUIRE(scan.result.status == ScanStatus::Completed);
    const FlatArena& arena = scan.arena;

    // 3 directories (root, ring, inner); 1 real file; 7 links recorded as leaves.
    CHECK_EQ(scan.result.totals.directories, uint64_t{3});
    CHECK_EQ(scan.result.totals.files, uint64_t{8});
    CHECK_EQ(arena.liveNodeCount(), 11u);
    CHECK_EQ(scan.result.totals.errors, uint64_t{0});

    for (const char* link : {"ring/a", "ring/b", "ring/self", "ring/inner/up", "ring/inner/to-root", "ring/dir-alias",
                             "ring/dangling"}) {
        const uint32_t node = findNode(arena, link);
        REQUIRE(node != kInvalidNode);
        const FsNode& n = arena.node(node);
        CHECK(n.is_symlink());
        CHECK(!n.is_dir());
        CHECK_EQ(n.child_count, 0u);
        CHECK_EQ(n.first_child_idx, kInvalidNode);
        const struct stat st = lstatOrDie(tmp.sub(link));
        CHECK_EQ(n.logical_bytes, static_cast<uint64_t>(st.st_size));  // the link itself, not its target
    }
    // The payload was reached exactly once, through its real path.
    uint32_t payloads = 0;
    for (uint32_t i = 0; i < arena.size(); ++i) {
        if (arena.name(i) == "payload.bin") ++payloads;
    }
    CHECK_EQ(payloads, 1u);
    CHECK(treeIsConsistent(arena));
}

DP_TEST(scan_root_given_as_symlink_is_resolved_once) {
    TempDir tmp;
    tmp.mkdir("real");
    tmp.writeFile("real/file.txt", 100);
    tmp.symLink("real", "entry");

    ScanFixture scan(tmp.sub("entry"));
    REQUIRE(scan.result.status == ScanStatus::Completed);
    CHECK_EQ(scan.result.root_path, tmp.sub("real"));
    CHECK_EQ(scan.arena.node(scan.arena.root()).file_count, 1u);
    CHECK_EQ(scan.arena.path(findNode(scan.arena, "file.txt")), tmp.sub("real/file.txt"));
}

DP_TEST(permission_denied_directories_are_recorded_not_fatal) {
    if (::geteuid() == 0) {
        std::printf("      (running as root — permission check skipped)\n");
        return;
    }
    TempDir tmp;
    tmp.mkdir("open");
    tmp.mkdir("locked");
    tmp.mkdir("locked/deeper");
    tmp.writeFile("open/visible.txt", 1000);
    tmp.writeFile("locked/hidden.txt", 2000);
    tmp.writeFile("locked/deeper/also-hidden.txt", 3000);
    REQUIRE(::chmod(tmp.sub("locked").c_str(), 0000) == 0);

    ScanFixture scan(tmp.path());
    ::chmod(tmp.sub("locked").c_str(), 0755);  // let the fixture clean up
    REQUIRE(scan.result.status == ScanStatus::Completed);

    CHECK_EQ(scan.result.totals.denied, uint64_t{1});
    CHECK_EQ(scan.result.totals.errors, uint64_t{0});
    CHECK_EQ(scan.result.totals.files, uint64_t{1});
    const uint32_t locked = findNode(scan.arena, "locked");
    REQUIRE(locked != kInvalidNode);
    CHECK(scan.arena.node(locked).access_denied());
    CHECK(scan.arena.node(locked).is_dir());
    CHECK_EQ(scan.arena.node(locked).child_count, 0u);
    CHECK_EQ(scan.arena.node(locked).allocated_bytes, uint64_t{0});
    CHECK(!scan.arena.node(findNode(scan.arena, "open")).access_denied());
    CHECK_EQ(scan.arena.node(scan.arena.root()).logical_bytes, uint64_t{1000});
    CHECK(treeIsConsistent(scan.arena));
}

DP_TEST(large_directory_is_linked_across_many_batches) {
    TempDir tmp;
    tmp.mkdir("many");
    constexpr int kFiles = 1500;
    std::set<std::string> expected;
    for (int i = 0; i < kFiles; ++i) {
        const std::string name = "entry-number-" + std::to_string(i) + (i % 3 == 0 ? ".log" : ".dat");
        const int fd = ::open(tmp.sub("many/" + name).c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
        REQUIRE(fd >= 0);
        ::close(fd);
        expected.insert(name);
    }
    for (int i = 0; i < 40; ++i) tmp.mkdir("many/subdir-" + std::to_string(i));

    for (size_t bufferBytes : {size_t{4096}, size_t{128 * 1024}}) {
        ScanOptions options;
        options.buffer_bytes = bufferBytes;  // 4 KiB forces dozens of getattrlistbulk batches
        ScanFixture scan(tmp.path(), options);
        REQUIRE(scan.result.status == ScanStatus::Completed);
        const uint32_t many = findNode(scan.arena, "many");
        REQUIRE(many != kInvalidNode);
        CHECK_EQ(scan.arena.node(many).child_count, static_cast<uint32_t>(kFiles + 40));
        CHECK_EQ(scan.arena.node(many).file_count, static_cast<uint32_t>(kFiles));

        std::set<std::string> seen;
        uint32_t chainLength = 0;
        uint32_t subdirs = 0;
        scan.arena.forEachChild(many, [&](uint32_t c) {
            ++chainLength;
            if (scan.arena.node(c).is_dir()) {
                ++subdirs;
            } else {
                seen.insert(std::string(scan.arena.name(c)));
            }
            if (scan.arena.node(c).parent_idx != many) seen.insert("<wrong parent>");
        });
        CHECK_EQ(chainLength, static_cast<uint32_t>(kFiles + 40));  // no entry lost or duplicated
        CHECK_EQ(subdirs, 40u);
        CHECK(seen == expected);
        CHECK_EQ(scan.result.totals.files, uint64_t{kFiles});
        CHECK_EQ(scan.result.totals.directories, uint64_t{42});
        if (bufferBytes == 4096) CHECK(scan.result.totals.syscalls > 60);
        CHECK(treeIsConsistent(scan.arena));
    }
}

DP_TEST(paths_longer_than_path_max_are_still_scanned) {
    TempDir tmp;
    // 30 levels of 60-character names: about 1 850 bytes, well beyond PATH_MAX (1024).
    const std::string component(60, 'n');
    int fd = ::open(tmp.path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    REQUIRE(fd >= 0);
    constexpr int kLevels = 30;
    for (int level = 0; level < kLevels; ++level) {
        REQUIRE(::mkdirat(fd, component.c_str(), 0755) == 0);
        const int next = ::openat(fd, component.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        REQUIRE(next >= 0);
        ::close(fd);
        fd = next;
    }
    const int file = ::openat(fd, "treasure.bin", O_WRONLY | O_CREAT | O_CLOEXEC, 0644);
    REQUIRE(file >= 0);
    const std::vector<char> data(12345, 'z');
    REQUIRE(::write(file, data.data(), data.size()) == static_cast<ssize_t>(data.size()));
    ::fsync(file);
    ::close(file);
    ::close(fd);

    ScanFixture scan(tmp.path());
    REQUIRE(scan.result.status == ScanStatus::Completed);
    const FlatArena& arena = scan.arena;
    CHECK_EQ(scan.result.totals.errors, uint64_t{0});
    CHECK_EQ(scan.result.totals.directories, uint64_t{kLevels + 1});
    CHECK_EQ(arena.node(arena.root()).file_count, 1u);
    CHECK_EQ(arena.node(arena.root()).logical_bytes, uint64_t{12345});

    uint32_t treasure = kInvalidNode;
    for (uint32_t i = 0; i < arena.size(); ++i) {
        if (arena.name(i) == "treasure.bin") treasure = i;
    }
    REQUIRE(treasure != kInvalidNode);
    CHECK_EQ(arena.depth(treasure), static_cast<uint32_t>(kLevels + 1));
    CHECK(arena.path(treasure).size() > size_t{PATH_MAX});
}

DP_TEST(unicode_and_awkward_names_round_trip_byte_for_byte) {
    TempDir tmp;
    const std::vector<std::string> names = {
        "plain.txt",
        "with space and (parens) [brackets].md",
        "ünïcödé-ñame.jpg",
        "日本語のファイル名.pdf",
        "emoji-🙂🚀-file.png",
        "e\xCC\x81-decomposed-accent.txt",  // "e" + combining acute (NFD)
        "trailing.dot.",
        ".hidden-file",
        "-leading-dash",
        std::string(200, 'L') + ".long",
    };
    for (const std::string& name : names) tmp.writeFile(name, 1);

    ScanFixture scan(tmp.path());
    REQUIRE(scan.result.status == ScanStatus::Completed);
    std::set<std::string> scanned;
    scan.arena.forEachChild(scan.arena.root(), [&](uint32_t c) { scanned.insert(std::string(scan.arena.name(c))); });

    // Compare against what readdir reports (the file system may normalise names).
    std::set<std::string> onDisk;
    DIR* dir = ::opendir(tmp.path().c_str());
    REQUIRE(dir != nullptr);
    while (const dirent* e = ::readdir(dir)) {
        if (std::strcmp(e->d_name, ".") != 0 && std::strcmp(e->d_name, "..") != 0) onDisk.insert(e->d_name);
    }
    ::closedir(dir);
    CHECK_EQ(onDisk.size(), names.size());
    CHECK(scanned == onDisk);
    for (uint32_t c = scan.arena.firstChild(scan.arena.root()); c != kInvalidNode; c = scan.arena.nextSibling(c)) {
        struct stat st;
        CHECK(::lstat(scan.arena.path(c).c_str(), &st) == 0);  // the reconstructed path opens the same file
    }
}

DP_TEST(invalid_roots_are_rejected_cleanly) {
    {
        ScanFixture scan("/definitely/not/a/real/path-diskprism");
        CHECK(scan.result.status == ScanStatus::InvalidRoot);
        CHECK(!scan.result.error_message.empty());
        CHECK(!scan.result.ok());
        CHECK_EQ(scan.arena.size(), 0u);
    }
    {
        TempDir tmp;
        tmp.writeFile("just-a-file", 10);
        ScanFixture scan(tmp.sub("just-a-file"));
        CHECK(scan.result.status == ScanStatus::InvalidRoot);
        CHECK(scan.result.error_message.find("not a directory") != std::string::npos);
    }
    {
        // An arena that already holds a tree must not be scanned into again.
        TempDir tmp;
        ScanFixture scan(tmp.path());
        REQUIRE(scan.result.status == ScanStatus::Completed);
        const ScanResult again = scan.scanner.scan(tmp.path(), scan.arena);
        CHECK(again.status == ScanStatus::InvalidRoot);
    }
}

DP_TEST(empty_directory_scans_to_a_single_root) {
    TempDir tmp;
    ScanFixture scan(tmp.path());
    REQUIRE(scan.result.status == ScanStatus::Completed);
    CHECK_EQ(scan.arena.size(), 1u);
    CHECK_EQ(scan.arena.node(0).file_count, 0u);
    CHECK_EQ(scan.arena.node(0).allocated_bytes, uint64_t{0});
    CHECK_EQ(scan.result.totals.directories, uint64_t{1});
}

DP_TEST(arena_overflow_truncates_the_scan_gracefully) {
    TempDir tmp;
    for (int i = 0; i < 300; ++i) tmp.writeFile("f" + std::to_string(i) + ".bin", 1);
    FlatArena tiny(ArenaConfig{64, 1 << 16});
    BulkScanner scanner;
    ScanOptions options;
    options.buffer_bytes = 4096;
    const ScanResult result = scanner.scan(tmp.path(), tiny, options);
    CHECK(result.status == ScanStatus::ArenaFull);
    CHECK(result.ok());
    CHECK(tiny.size() <= 64u);
    CHECK(treeIsConsistent(tiny));  // truncated, but still a valid tree
    CHECK(tiny.node(0).file_count < 300u);
}

// ----------------------------------------------------------------------------------------
// Progress, cancellation, concurrency
// ----------------------------------------------------------------------------------------

DP_TEST(progress_snapshots_are_monotonic_and_end_with_the_totals) {
    std::vector<ScanProgress> snapshots;
    FlatArena arena(ArenaConfig{1u << 22, 256u << 20});
    BulkScanner scanner;
    ScanOptions options;
    options.progress_interval_seconds = 0.002;
    const ScanResult result =
        scanner.scan("/usr/share", arena, options, [&snapshots](const ScanProgress& p) { snapshots.push_back(p); });
    REQUIRE(result.status == ScanStatus::Completed);
    REQUIRE(!snapshots.empty());

    bool monotonic = true;
    for (size_t i = 1; i < snapshots.size(); ++i) {
        monotonic = monotonic && snapshots[i].files >= snapshots[i - 1].files &&
                    snapshots[i].directories >= snapshots[i - 1].directories &&
                    snapshots[i].allocated_bytes >= snapshots[i - 1].allocated_bytes &&
                    snapshots[i].elapsed_seconds >= snapshots[i - 1].elapsed_seconds;
    }
    CHECK(monotonic);
    for (size_t i = 0; i + 1 < snapshots.size(); ++i) CHECK(!snapshots[i].finished);
    const ScanProgress& last = snapshots.back();
    CHECK(last.finished);
    CHECK_EQ(last.files, result.totals.files);
    CHECK_EQ(last.directories, result.totals.directories);
    CHECK_EQ(last.files, uint64_t{arena.node(0).file_count});
    CHECK_EQ(last.allocated_bytes, arena.node(0).allocated_bytes);
    CHECK_EQ(last.logical_bytes, arena.node(0).logical_bytes);
    CHECK(last.files_per_second > 0);
    CHECK(last.elapsed_seconds > 0);
    // The scanner's own snapshot accessor agrees after the fact.
    CHECK_EQ(scanner.progress().files, result.totals.files);
    std::printf("      %zu progress snapshots over %.1f ms\n", snapshots.size(), result.scan_seconds * 1e3);
}

DP_TEST(cancellation_stops_promptly_and_leaves_a_consistent_tree) {
    FlatArena arena(ArenaConfig{1u << 22, 256u << 20});
    BulkScanner scanner;
    ScanOptions options;
    options.progress_interval_seconds = 0.001;
    std::atomic<int> callbacks{0};
    const ScanResult result = scanner.scan("/usr", arena, options, [&](const ScanProgress& p) {
        if (!p.finished && callbacks.fetch_add(1) == 0) scanner.cancel();
    });
    // The scan is either cancelled part-way or (on a very fast machine) already complete.
    CHECK(result.status == ScanStatus::Cancelled || result.status == ScanStatus::Completed);
    CHECK(result.ok());
    CHECK(treeIsConsistent(arena));
    CHECK(arena.isSorted());
    CHECK_EQ(result.totals.files, uint64_t{arena.node(0).file_count});
    if (result.status == ScanStatus::Cancelled) {
        CHECK(scanner.cancelRequested());
        std::printf("      cancelled after %llu files (%.1f ms)\n", static_cast<unsigned long long>(result.totals.files),
                    result.scan_seconds * 1e3);
    }

    // Cancelling from another thread while workers are asleep or busy must not deadlock.
    for (int round = 0; round < 20; ++round) {
        FlatArena roundArena(ArenaConfig{1u << 22, 256u << 20});
        BulkScanner roundScanner;
        std::thread canceller([&roundScanner, round] {
            std::this_thread::sleep_for(std::chrono::microseconds(200 * round));
            roundScanner.cancel();
        });
        const ScanResult r = roundScanner.scan("/usr/share", roundArena);
        canceller.join();
        CHECK(r.ok());
        CHECK(treeIsConsistent(roundArena));
    }

    // A request that arrives before the scan has started is not lost…
    {
        FlatArena early(ArenaConfig{1u << 22, 256u << 20});
        BulkScanner earlyScanner;
        earlyScanner.cancel();
        const ScanResult r = earlyScanner.scan("/usr/share", early);
        CHECK(r.status == ScanStatus::Cancelled);
        CHECK_EQ(early.size(), 1u);  // only the root: nothing was enumerated
        // …and applies to exactly that scan.
        FlatArena next(ArenaConfig{1u << 22, 256u << 20});
        CHECK(earlyScanner.scan("/usr/bin", next).status == ScanStatus::Completed);
    }

    // A scanner can be reused after a cancelled scan.
    FlatArena again(ArenaConfig{1u << 22, 256u << 20});
    const ScanResult rerun = scanner.scan("/usr/bin", again);
    CHECK(rerun.status == ScanStatus::Completed);
    CHECK(!scanner.cancelRequested());
}

DP_TEST(scanner_matches_an_independent_readdir_walk) {
    // /usr/share lives on the sealed, read-only system volume: nothing changes under us.
    const std::string root = "/usr/share";
    struct stat rootStat;
    REQUIRE(::stat(root.c_str(), &rootStat) == 0);
    WalkTotals reference;
    referenceWalk(root, rootStat.st_dev, reference);
    REQUIRE(reference.files > 1000);

    for (unsigned threads : {1u, 3u, 8u, 16u}) {
        ScanOptions options;
        options.thread_count = threads;
        ScanFixture scan(root, options);
        REQUIRE(scan.result.status == ScanStatus::Completed);
        CHECK_EQ(scan.result.threads, threads);
        CHECK_EQ(scan.result.totals.files, reference.files);
        CHECK_EQ(scan.result.totals.directories, reference.directories);
        CHECK_EQ(scan.result.totals.logical_bytes, reference.logical);
        CHECK_EQ(scan.result.totals.allocated_bytes, reference.allocated);
        CHECK_EQ(scan.result.totals.hardlink_duplicates, reference.hardlinkDups);
        CHECK_EQ(scan.arena.node(0).file_count, static_cast<uint32_t>(reference.files));
        CHECK_EQ(scan.arena.node(0).allocated_bytes, reference.allocated);
        CHECK_EQ(scan.arena.node(0).logical_bytes, reference.logical);
        CHECK_EQ(uint64_t{scan.arena.liveNodeCount()}, reference.files + reference.directories);
        CHECK(treeIsConsistent(scan.arena));
        CHECK_EQ(scan.arena.extensions().totals().allocated_bytes, reference.allocated);
        CHECK_EQ(scan.arena.extensions().totals().file_count, reference.files);
    }
    std::printf("      /usr/share: %llu files, %llu directories, %.1f MB physical, %.1f MB logical\n",
                static_cast<unsigned long long>(reference.files), static_cast<unsigned long long>(reference.directories),
                static_cast<double>(reference.allocated) / 1e6, static_cast<double>(reference.logical) / 1e6);
}

DP_TEST(repeated_parallel_scans_are_stable) {
    // Hammer the lock-free allocation and work stealing paths: many scans, all identical.
    uint64_t files = 0, allocated = 0, nodes = 0;
    for (int round = 0; round < 12; ++round) {
        ScanOptions options;
        options.thread_count = 4 + static_cast<unsigned>(round % 5) * 3;
        options.buffer_bytes = round % 2 == 0 ? 128 * 1024 : 8 * 1024;
        ScanFixture scan("/usr/share/zsh", options);
        REQUIRE(scan.result.status == ScanStatus::Completed);
        if (round == 0) {
            files = scan.result.totals.files;
            allocated = scan.result.totals.allocated_bytes;
            nodes = scan.arena.liveNodeCount();
            CHECK(files > 100);
        }
        CHECK_EQ(scan.result.totals.files, files);
        CHECK_EQ(scan.result.totals.allocated_bytes, allocated);
        CHECK_EQ(uint64_t{scan.arena.liveNodeCount()}, nodes);
        CHECK(treeIsConsistent(scan.arena));
    }
}

// ----------------------------------------------------------------------------------------
// Volume capacity
// ----------------------------------------------------------------------------------------

DP_TEST(volume_capacity_metrics_are_plausible) {
    const std::vector<VolumeInfo> all = listAllVolumes();
    REQUIRE(!all.empty());
    const auto rootIt = std::find_if(all.begin(), all.end(), [](const VolumeInfo& v) { return v.mount_point == "/"; });
    REQUIRE(rootIt != all.end());
    CHECK(rootIt->is_root);
    CHECK(rootIt->is_local);
    CHECK(rootIt->total_bytes > uint64_t{1} << 30);
    CHECK(rootIt->free_bytes <= rootIt->total_bytes);
    CHECK(rootIt->available_bytes <= rootIt->free_bytes);
    CHECK(rootIt->used_bytes() + rootIt->free_bytes == rootIt->total_bytes);
    CHECK(!rootIt->fs_type.empty());
    CHECK(!rootIt->name.empty());

    const std::vector<VolumeInfo> user = listUserVolumes();
    REQUIRE(!user.empty());
    CHECK_EQ(user.front().mount_point, "/");  // boot volume first
    // "Important usage" capacity includes purgeable space, so it is never below statfs.
    CHECK(user.front().important_available_bytes >= user.front().available_bytes);
    CHECK(user.front().important_available_bytes <= user.front().total_bytes);
    CHECK_EQ(user.front().purgeable_bytes(), user.front().important_available_bytes - user.front().available_bytes);
    for (const VolumeInfo& v : user) CHECK(v.is_root || (v.is_local && v.is_browsable));

    const auto forTmp = volumeForPath("/private/tmp");
    REQUIRE(forTmp.has_value());
    CHECK(forTmp->total_bytes > 0);
    CHECK(!volumeForPath("/definitely/not/here-diskprism").has_value());

    // statfs reports each volume's *real* device, while stat() reports one unified device
    // for the whole system + data volume group (which is why st_dev cannot separate them).
    struct stat st;
    REQUIRE(::stat("/", &st) == 0);
    std::printf("      device IDs for \"/\": statfs f_fsid = %d, stat st_dev = %d\n", rootIt->dev_id, st.st_dev);
    std::printf("      \"%s\": %.1f GB total, %.1f GB free, %.1f GB purgeable (%s)\n", user.front().name.c_str(),
                static_cast<double>(user.front().total_bytes) / 1e9, static_cast<double>(user.front().available_bytes) / 1e9,
                static_cast<double>(user.front().purgeable_bytes()) / 1e9, user.front().fs_type.c_str());
}

DP_TEST_MAIN()
