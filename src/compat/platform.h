// license:BSD-3-Clause
//
// Primitives that genuinely differ per platform.
//
// Keep this layer tiny and behavior-preserving: this is a synthesizer and its
// output is compared bit for bit between builds, so anything added here must
// not change what the emulator computes.

#ifndef S_MU2000_COMPAT_PLATFORM_H
#define S_MU2000_COMPAT_PLATFORM_H

#pragma once

#include <chrono>
#include <thread>

// The x86 pause intrinsic. Only pull the header on x86 so the ARM build does
// not trip over <immintrin.h> (which errors out on non-x86 targets).
#if defined(_MSC_VER)
#  include <intrin.h>
#elif defined(__i386__) || defined(__x86_64__)
#  include <immintrin.h>
#endif

namespace smu2000 {

// Spin-wait hint for a tight loop that is waiting on another thread (the two
// SWP30s hand a sample off every 22.7us). It must not sleep or yield the core.
//
//   x86   … PAUSE, exactly what _mm_pause() emitted before
//   arm64 … YIELD, the Apple silicon equivalent
//   other … give up the timeslice; a fallback that neither target hits
inline void cpu_pause() noexcept
{
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
	_mm_pause();
#elif defined(__i386__) || defined(__x86_64__)
	_mm_pause();
#elif defined(__aarch64__) || defined(__arm__)
	__asm__ __volatile__("yield" ::: "memory");
#else
	std::this_thread::yield();
#endif
}

// Sleep for a number of milliseconds.
//
// std::this_thread::sleep_for rather than Sleep() on purpose: it needs no
// windows.h, and on Windows it waits on the same timer, so it rounds up to the
// same granularity when timeBeginPeriod() has not been asked for.
inline void sleep_ms(int ms) noexcept
{
	std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

} // namespace smu2000

#endif // S_MU2000_COMPAT_PLATFORM_H
