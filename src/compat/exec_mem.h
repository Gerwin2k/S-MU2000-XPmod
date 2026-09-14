// license:BSD-3-Clause
//
// S-MU2000: the executable-memory layer the JITs sit on.
//
//   alloc_rw(size)       anonymous RW memory (staging and code buffers)
//   alloc_rwx(size)      RWX memory for code that is written and run in place
//   make_writable(b, s)  re-protect a buffer for writing (rebuilds)
//   make_executable(b, s) re-protect it for execution
//   free_mem(b, s)
//
// RWX is fine for the tools here: they are not hardened-runtime processes, so
// macOS maps MAP_JIT pages RWX and Linux allows the same. Code that wants to
// survive hardening uses the alloc_rw + make_writable/make_executable dance
//
// Copy-in always goes through make_writable → memcpy → make_executable, so a
// rebuild of an already-executable buffer works on every platform. Nothing is
// ever persisted: the generated code lives in anonymous memory and dies with
// the process (upstream's rule: no firmware-derived data on disk).
//
// macOS maps with MAP_JIT. The pthread_jit_write_protect_np toggle is only
// needed for restricted (hardened-runtime) processes; ours are not, so plain
// mprotect between RW and RX is enough. Linux uses plain anonymous mmap.

#ifndef S_MU2000_EXEC_MEM_H
#define S_MU2000_EXEC_MEM_H

#include "mamecompat.h"

#if defined(_WIN32)

#include <windows.h>

namespace exec_mem {

inline void *alloc_rw(size_t size)
{
	return VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
}

inline void *alloc_rwx(size_t size)
{
	return VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
}

inline bool make_writable(void *buf, size_t size)
{
	DWORD old;
	return VirtualProtect(buf, size, PAGE_READWRITE, &old) != 0;
}

inline bool make_executable(void *buf, size_t size)
{
	DWORD old;
	return VirtualProtect(buf, size, PAGE_EXECUTE_READ, &old) != 0;
}

inline void free_mem(void *buf, size_t)
{
	VirtualFree(buf, 0, MEM_RELEASE);
}

} // namespace exec_mem

#else

#include <sys/mman.h>

#ifdef __APPLE__
#ifndef MAP_JIT
#define MAP_JIT 0x800
#endif
#define SMU2000_MMAP_EXTRA MAP_JIT
#else
#define SMU2000_MMAP_EXTRA 0
#endif

namespace exec_mem {

inline void *alloc_rw(size_t size)
{
	void *p = mmap(nullptr, size, PROT_READ | PROT_WRITE,
	               MAP_PRIVATE | MAP_ANONYMOUS | SMU2000_MMAP_EXTRA, -1, 0);
	return p == MAP_FAILED ? nullptr : p;
}

inline void *alloc_rwx(size_t size)
{
	void *p = mmap(nullptr, size, PROT_READ | PROT_WRITE | PROT_EXEC,
	               MAP_PRIVATE | MAP_ANONYMOUS | SMU2000_MMAP_EXTRA, -1, 0);
	return p == MAP_FAILED ? nullptr : p;
}

inline bool make_writable(void *buf, size_t size)
{
	return mprotect(buf, size, PROT_READ | PROT_WRITE) == 0;
}

inline bool make_executable(void *buf, size_t size)
{
	return mprotect(buf, size, PROT_READ | PROT_EXEC) == 0;
}

inline void free_mem(void *buf, size_t size)
{
	munmap(buf, size);
}

} // namespace exec_mem

#undef SMU2000_MMAP_EXTRA

#endif

#endif // S_MU2000_EXEC_MEM_H
