// shared_net.h -- NNUE network in memory shared between processes (01/10/2026), only with -DTRIUMV_SHARED_NET.
//
// Why. With many engines on the same machine (matches with 76, SPSA) every process kept ITS OWN copy of the weights:
// about 245 MB each, 18 GB with 76 engines, and above all 19 different copies of the same data competing for the
// same socket L3. Measured on 01/10: an engine loses 46% of NPS with 18 other engines on the socket. With the shared
// network the processes read the SAME physical pages, so the hot rows of the network share the cache instead
// of evicting each other, and the RAM drops to one copy per NUMA node.
//
// The first process that loads a network creates a named
// area (CreateFileMapping on the pagefile), copies into it the whole Network object (it is byte-copyable: weights in arrays,
// names in FixedString) and marks it ready; the others open it READ-ONLY and free their own copy.
// The name contains the hash of the network's content, its size and the process's NUMA node: different networks or
// different layouts never mix, and every socket has its copy in its own memory (created by the first process
// of that node, which touches it first). Large pages if possible, then normal pages, then the usual local copy.
// All under a named mutex: a process dying mid-copy leaves "ready" at zero and the others do not use it.
#pragma once
#if defined(TRIUMV_SHARED_NET) && defined(_WIN32)

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#ifndef NOMINMAX
    #define NOMINMAX
#endif
#include <windows.h>

#include "large_pages.h"

#ifndef FILE_MAP_LARGE_PAGES
    #define FILE_MAP_LARGE_PAGES 0x20000000
#endif
#ifndef SEC_LARGE_PAGES
    #define SEC_LARGE_PAGES 0x80000000
#endif

namespace TriumvShm {

struct Header {
    std::uint64_t magic;
    std::uint64_t hash;
    std::uint64_t size;
    std::uint32_t largePages;
    volatile LONG ready;
};
constexpr std::uint64_t Magic       = 0x5452494D56534E31ull;   // "TRIMVSN1"
constexpr std::size_t   HeaderBytes = 4096;                    // i pesi partono allineati a pagina

struct View {
    HANDLE map  = nullptr;
    void*  base = nullptr;
};
inline View& current() { static View v; return v; }

inline int numa_node() {
    PROCESSOR_NUMBER pn{};
    GetCurrentProcessorNumberEx(&pn);
    USHORT node = 0;
    return GetNumaProcessorNodeEx(&pn, &node) ? int(node) : 0;
}

// Releases the previous view (network reload). The area disappears when the last process closes it.
inline void release() {
    View& v = current();
    if (v.base) UnmapViewOfFile(v.base);
    if (v.map) CloseHandle(v.map);
    v = View{};
}

// Opens read-only an already existing area (first as large pages, then normal) and checks the header.
// 1 = open and ready; 0 = does not exist; -1 = exists but is not (yet) ready or does not match.
// It can be called without the mutex too: the creator writes `ready` last, after the barrier.
inline int open_ready(const char* name, std::uint64_t hash, std::size_t size, int node, View& v, const void*& result,
                      std::string& status) {
    if ((v.map = OpenFileMappingA(FILE_MAP_READ, FALSE, name)) == nullptr)
        return 0;
    v.base = MapViewOfFile(v.map, FILE_MAP_READ | FILE_MAP_LARGE_PAGES, 0, 0, 0);
    if (!v.base) v.base = MapViewOfFile(v.map, FILE_MAP_READ, 0, 0, 0);
    const Header* h = static_cast<const Header*>(v.base);
    if (h && h->magic == Magic && h->hash == hash && h->size == size && h->ready == 1) {
        result = static_cast<const char*>(v.base) + HeaderBytes;
        status = std::string("shared (opened") + (h->largePages ? ", large pages" : "") + ", node "
               + std::to_string(node) + ")";
        return 1;
    }
    if (v.base) UnmapViewOfFile(v.base);
    CloseHandle(v.map);
    v = View{};
    return -1;
}

// Returns a pointer to the shared object (read-only for whoever opens it, written once by whoever creates it),
// or nullptr: then the caller keeps its local copy. `status` describes the outcome for the info string.
inline const void* attach(const void* src, std::size_t size, std::uint64_t hash, std::string& status) {
    release();
    const int node = numa_node();
    char name[128];
    std::snprintf(name, sizeof name, "Local\\triumv_net_%016llx_%llx_n%d", (unsigned long long) hash,
                  (unsigned long long) size, node);
    const std::string mtxName = std::string(name) + "_mtx";

    HANDLE mtx = CreateMutexA(nullptr, FALSE, mtxName.c_str());
    if (!mtx) { status = "local (mutex)"; return nullptr; }

    const std::size_t total = HeaderBytes + size;
    View v;
    const void* result = nullptr;

    // Waiting for the mutex without giving up (08/10/2026). Before, after 120 s, the process kept its private copy.
    // Whoever creates the area holds the mutex also while the system looks for large pages, and on a node with memory
    // fragmented by hours of matches this can take minutes: in the SPRT of 08/10 (6+0.06, 140 engines) 9 engines
    // were left with a private copy (402 MB instead of 175), all started in the same second, and socket 1
    // gave -20.8 +- 3.6 Elo against -0.1 +- 3.2 for socket 0 with the same network, constant from start to end.
    // Now it waits in steps of 1 s and at every step tries to open the already ready area without the mutex; the private
    // copy remains only after 15 minutes.
    DWORD w = WAIT_TIMEOUT;
    for (int s = 0; s < 900 && w == WAIT_TIMEOUT; ++s) {
        w = WaitForSingleObject(mtx, 1000);
        if (w == WAIT_TIMEOUT && open_ready(name, hash, size, node, v, result, status) == 1) {
            CloseHandle(mtx);
            current() = v;
            return result;
        }
    }
    if (w != WAIT_OBJECT_0 && w != WAIT_ABANDONED) { CloseHandle(mtx); status = "local (mutex timeout)"; return nullptr; }

    // 1) The area already exists: it opens it read-only.
    const int op = open_ready(name, hash, size, node, v, result, status);
    if (op == -1) {
        status = "local (shared area not ready)";
    } else if (op == 0) {
        // 2) It does not exist: it creates it, first with large pages (needs SeLockMemoryPrivilege), then with normal pages.
        DWORD lpErr = 0;   // why the large pages failed (0 = privilege not obtained)
        bool large = with_lock_memory_privilege(
          [&](std::size_t lp) {
              const std::size_t sz = (total + lp - 1) / lp * lp;
              v.map = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT | SEC_LARGE_PAGES,
                                         DWORD(std::uint64_t(sz) >> 32), DWORD(sz & 0xFFFFFFFFu), name);
              if (!v.map) { lpErr = GetLastError(); return false; }
              v.base = MapViewOfFile(v.map, FILE_MAP_READ | FILE_MAP_WRITE | FILE_MAP_LARGE_PAGES, 0, 0, 0);
              if (!v.base) { lpErr = 100000 + GetLastError(); CloseHandle(v.map); v.map = nullptr; return false; }
              return true;
          },
          [] { return false; });
        if (!large) {
            v.map = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE | SEC_COMMIT,
                                       DWORD(std::uint64_t(total) >> 32), DWORD(total & 0xFFFFFFFFu), name);
            if (v.map) v.base = MapViewOfFile(v.map, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
        }
        if (v.base) {
            Header* h = static_cast<Header*>(v.base);
            std::memcpy(static_cast<char*>(v.base) + HeaderBytes, src, size);   // touches the pages: local node
            h->magic = Magic; h->hash = hash; h->size = size; h->largePages = large ? 1 : 0;
            MemoryBarrier();
            h->ready = 1;
            result = static_cast<const char*>(v.base) + HeaderBytes;
            status = std::string("shared (created") + (large ? ", large pages" : ", no large pages: err "
                   + std::to_string(lpErr)) + ", node " + std::to_string(node) + ")";
        } else {
            if (v.map) CloseHandle(v.map);
            v = View{};
            status = "local (cannot create shared area)";
        }
    }
    ReleaseMutex(mtx);
    CloseHandle(mtx);
    if (result) current() = v;
    return result;
}

}  // namespace TriumvShm

#elif defined(TRIUMV_SHARED_NET) && defined(__linux__)

// Linux (01/11/2026, a rented 384-thread machine for the SPSA: 768 engines with a private copy each would need about
// 190 GB of 125). The same design on a file that every process maps: the first process of a NUMA node writes the
// Network object into <dir>/triumv_net_<hash>_<size>_n<node> (a temporary name, renamed when complete, so the final
// name exists only once the weights are in), the others map it read-only and free their own copy. <dir> is
// $TRIUMV_SHM_DIR, else /dev/shm (memory), else /tmp (the page cache, still one copy). An exclusive flock on
// <name>.lock serialises the creators; posix_fallocate reserves the space first, so a full directory fails there
// instead of with SIGBUS in the copy. The files stay after the engines exit and are reused by the next ones.

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace TriumvShm {

struct Header {
    std::uint64_t magic;
    std::uint64_t hash;
    std::uint64_t size;
    std::uint32_t largePages;
    volatile std::uint32_t ready;
};
constexpr std::uint64_t Magic       = 0x5452494D56534E31ull;   // "TRIMVSN1"
constexpr std::size_t   HeaderBytes = 4096;                    // the weights start page-aligned

struct View {
    void*       base = nullptr;
    std::size_t len  = 0;
};
inline View& current() { static View v; return v; }

inline int numa_node() {
    unsigned cpu = 0, node = 0;
    return syscall(SYS_getcpu, &cpu, &node, nullptr) == 0 ? int(node) : 0;
}

// Releases the previous view (network reload).
inline void release() {
    View& v = current();
    if (v.base) munmap(v.base, v.len);
    v = View{};
}

// Maps an existing complete file read-only and checks the header (the content hash too unless anyHash); false if it
// is missing or does not match.
inline bool map_ready(const std::string& path, std::uint64_t hash, std::size_t size, View& v, const void*& result,
                      bool anyHash = false) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return false;
    const std::size_t total = HeaderBytes + size;
    struct stat st;
    if (fstat(fd, &st) != 0 || std::size_t(st.st_size) != total) {
        close(fd);
        return false;
    }
    void* b = mmap(nullptr, total, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (b == MAP_FAILED)
        return false;
    const Header* h = static_cast<const Header*>(b);
    if (h->magic == Magic && (anyHash || h->hash == hash) && h->size == size && h->ready == 1) {
        v.base = b, v.len = total;
        result = static_cast<const char*>(b) + HeaderBytes;
        return true;
    }
    munmap(b, total);
    return false;
}

// Second name of the same file, from the identity of the network file on disk (device, inode, size, modification
// time) and a load parameter that changes the weights (PassedScale): a process maps the weights through it BEFORE
// loading them, so an engine start holds no private copy at all (with hundreds of engines restarting at random times
// those 245 MB copies emptied the memory for moments). "" when the file cannot be read.
inline std::string file_alias(const char* dir, const char* netpath, std::uint64_t param, std::size_t size, int node) {
    struct stat st;
    if (!netpath || stat(netpath, &st) != 0)
        return "";
    char name[512];
    std::snprintf(name, sizeof name, "%s/triumv_netf_%llx_%llx_%llx_%llx_%llx_%llx_n%d", dir,
                  (unsigned long long) st.st_dev, (unsigned long long) st.st_ino, (unsigned long long) st.st_size,
                  (unsigned long long) st.st_mtime, (unsigned long long) param, (unsigned long long) size, node);
    return name;
}

// Maps the shared weights of `netpath` through its file alias, or nullptr (no alias yet: load the file as usual).
inline const void* attach_by_file(const char* netpath, std::uint64_t param, std::size_t size, std::string& status) {
    const int   node = numa_node();
    const char* env  = std::getenv("TRIUMV_SHM_DIR");
    const char* dirs[] = {env && *env ? env : "/dev/shm", "/tmp"};
    for (const char* dir : dirs) {
        const std::string alias = file_alias(dir, netpath, param, size, node);
        View        v;
        const void* result = nullptr;
        if (!alias.empty() && map_ready(alias, 0, size, v, result, true)) {
            release();
            current() = v;
            status    = "shared (opened " + alias + ")";
            return result;
        }
    }
    return nullptr;
}

// Returns a pointer to the shared object, or nullptr: then the caller keeps its local copy. `status` describes the
// outcome for the info string. With netpath, the file also gets its alias (file_alias) for the next processes.
inline const void* attach(const void* src, std::size_t size, std::uint64_t hash, std::string& status,
                          const char* netpath = nullptr, std::uint64_t param = 0) {
    release();
    const int         node  = numa_node();
    const std::size_t total = HeaderBytes + size;
    const char*       env   = std::getenv("TRIUMV_SHM_DIR");
    const char*       dirs[] = {env && *env ? env : "/dev/shm", "/tmp"};
    for (const char* dir : dirs) {
        char name[512];
        std::snprintf(name, sizeof name, "%s/triumv_net_%016llx_%llx_n%d", dir, (unsigned long long) hash,
                      (unsigned long long) size, node);
        const std::string path(name);
        View        v;
        const void* result = nullptr;
        if (map_ready(path, hash, size, v, result)) {
            const std::string alias = netpath ? file_alias(dir, netpath, param, size, node) : "";
            if (!alias.empty())
                link(path.c_str(), alias.c_str());
            current() = v;
            status    = "shared (opened " + path + ")";
            return result;
        }
        const int lk = open((path + ".lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0666);
        if (lk < 0)
            continue;
        if (flock(lk, LOCK_EX) != 0) {   // waits while another process creates the file
            close(lk);
            continue;
        }
        bool created = false;
        if (!map_ready(path, hash, size, v, result)) {
            const std::string tmp = path + ".tmp." + std::to_string(getpid());
            const int         fd  = open(tmp.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
            bool              ok  = false;
            if (fd >= 0) {
                if (posix_fallocate(fd, 0, off_t(total)) == 0) {
                    void* b = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                    if (b != MAP_FAILED) {
                        Header* h = static_cast<Header*>(b);
                        std::memcpy(static_cast<char*>(b) + HeaderBytes, src, size);   // touches the pages: local node
                        h->magic = Magic, h->hash = hash, h->size = size, h->largePages = 0;
                        __sync_synchronize();
                        h->ready = 1;
                        munmap(b, total);
                        ok = true;
                    }
                }
                close(fd);
                ok = ok && rename(tmp.c_str(), path.c_str()) == 0;
                if (!ok)
                    unlink(tmp.c_str());
            }
            created = ok && map_ready(path, hash, size, v, result);
        }
        if (result && netpath) {   // the alias, a hard link to the same file (same directory); an existing one stays
            const std::string alias = file_alias(dir, netpath, param, size, node);
            if (!alias.empty())
                link(path.c_str(), alias.c_str());
        }
        flock(lk, LOCK_UN);
        close(lk);
        if (result) {
            current() = v;
            status    = std::string(created ? "shared (created " : "shared (opened ") + path + ")";
            return result;
        }
    }
    status = "local (no shared file)";
    return nullptr;
}

}  // namespace TriumvShm

#endif
