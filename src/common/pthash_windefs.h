// Provides macro fallbacks to support the PTHash dependency under Windows MSVC
// or MinGW compilers by defining missing POSIX or built-in functions.

#ifndef PTHASH_WINDEFS_H
#define PTHASH_WINDEFS_H

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <direct.h>
#include <errno.h>
#include <io.h>
#ifndef PSAPI_VERSION
#define PSAPI_VERSION 1
#endif
// windows.h must be included before psapi.h to provide necessary type
// definitions.
#include <windows.h>

#include <fcntl.h>
#include <psapi.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <limits>

#if !defined(_MODE_T_DECLARED) && !defined(_MODE_T_)
typedef int mode_t;
#define _MODE_T_DECLARED
#endif

// Resource usage
#define RUSAGE_SELF 0
struct rusage {
  long ru_maxrss;
};

inline int getrusage([[maybe_unused]] int who, struct rusage *usage) {
  PROCESS_MEMORY_COUNTERS pmc;
  if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
    usage->ru_maxrss = (long)(pmc.WorkingSetSize / 1024);
    return 0;
  }
  return -1;
}

// Memory mapping
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_SHARED 1
#define MAP_PRIVATE 2
#define MAP_ANON 0x20
#define MAP_ANONYMOUS MAP_ANON
#define MAP_FAILED ((void *)-1)

/* Provide a noop fallback for O_CLOEXEC on Windows so POSIX flags compile. */
#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif
inline void *mmap([[maybe_unused]] void *addr, size_t length, int prot,
                  [[maybe_unused]] int flags, int fd, long long offset) {
  HANDLE hFile = (HANDLE)_get_osfhandle(fd);
  if (hFile == INVALID_HANDLE_VALUE) {
    errno = EBADF;
    return MAP_FAILED;
  }

  DWORD flProtect = PAGE_READONLY;
  DWORD dwAccess = FILE_MAP_READ;

  if (prot & PROT_WRITE) {
    flProtect = PAGE_READWRITE;
    dwAccess = FILE_MAP_WRITE;
  }

  HANDLE hMap = CreateFileMapping(hFile, NULL, flProtect, 0, 0, NULL);
  if (hMap == NULL) {
    DWORD err = GetLastError();
    if (err == ERROR_DISK_FULL)
      errno = ENOSPC;
    else
      errno = ENOMEM;
    return MAP_FAILED;
  }

  DWORD offsetLow = (DWORD)(offset & 0xFFFFFFFF);
  DWORD offsetHigh = (DWORD)((offset >> 32) & 0xFFFFFFFF);

  void *ptr = MapViewOfFile(hMap, dwAccess, offsetHigh, offsetLow, length);
  CloseHandle(hMap);

  if (ptr == NULL) {
    DWORD err = GetLastError();
    if (err == ERROR_INVALID_PARAMETER)
      errno = EINVAL;
    else
      errno = ENOMEM;
    return MAP_FAILED;
  }
  return ptr;
}

inline int munmap(void *addr, [[maybe_unused]] size_t length) {
  return UnmapViewOfFile(addr) ? 0 : -1;
}

inline int ftruncate(int fd, long long length) { return _chsize_s(fd, length); }

#ifndef O_BINARY
#define O_BINARY 0x8000
#endif

// System configuration
#define _SC_PAGESIZE 0
#define _SC_PHYS_PAGES 1

inline long sysconf(int name) {
  if (name == _SC_PAGESIZE) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize;
  } else if (name == _SC_PHYS_PAGES) {
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (GlobalMemoryStatusEx(&ms)) {
      SYSTEM_INFO si;
      GetSystemInfo(&si);
      const uint64_t page_count = ms.ullTotalPhys / si.dwPageSize;
      return static_cast<long>(
          std::min<uint64_t>(page_count, std::numeric_limits<long>::max()));
    }
  }
  return -1;
}

// madvise (no-op)
#define POSIX_MADV_NORMAL 0
#define POSIX_MADV_RANDOM 0
#define POSIX_MADV_SEQUENTIAL 0
inline int posix_madvise([[maybe_unused]] void *addr,
                         [[maybe_unused]] size_t len,
                         [[maybe_unused]] int advice) {
  return 0;
}

// Use namespaced mkdir to avoid macro clashes with STL .mkdir()
namespace pthash_win {
inline int mkdir(const char *path, [[maybe_unused]] int mode) {
  return _mkdir(path);
}
} // namespace pthash_win

#endif // _WIN32

#endif // PTHASH_WINDEFS_H
