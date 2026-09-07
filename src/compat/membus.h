// license:BSD-3-Clause
//
// CPU が読み書きするバス。
//
// SWP30 の相手（波形 ROM など）は素の配列だったので flat_space で足りたが、
// CPU の空間は ROM・RAM・周辺レジスタが混在するので、番地で振り分ける必要がある。
//
// MAME の address_space は汎用で重い。ここでは MU2000 に必要な形だけを持つ:
//   - 大きな連続領域（ROM / RAM）は配列を直に指す
//   - 周辺（SWP30 や SH7042 の内蔵レジスタ）は関数で受ける
//   - 空間は 32bit、ビッグエンディアン（SH-2）
//
// 周辺のハンドラは幅ごとに登録する。レジスタは読むだけで状態が変わるものが
// あるので、8bit の読み書きを 16bit の read-modify-write で代用してはいけない。
// 登録がない幅は、狭い方／広い方から組み立てる（MAME のメモリ機構と同じ）。

#ifndef S_MU2000_MEMBUS_H
#define S_MU2000_MEMBUS_H

#pragma once

#include "mamecompat.h"

#include <functional>

class mem_bus
{
public:
	// 連続領域。ROM は writable=false
	struct region {
		u32   start = 0, end = 0;
		u8   *base  = nullptr;
		bool  writable = false;
	};

	// 周辺。ハンドラには絶対番地がそのまま渡る
	struct device {
		u32 start = 0, end = 0;
		std::function<u8  (offs_t)>       r8;
		std::function<u16 (offs_t)>       r16;
		std::function<u32 (offs_t)>       r32;
		std::function<void(offs_t, u8)>   w8;
		std::function<void(offs_t, u16)>  w16;
		std::function<void(offs_t, u32)>  w32;
	};

	void add_region(u32 start, u32 end, void *base, bool writable)
	{
		m_regions.push_back({ start, end, reinterpret_cast<u8 *>(base), writable });
	}

	void add_device(device d) { m_devices.push_back(std::move(d)); }

	// ---- 読み出し。SH-2 はビッグエンディアン

	u8 read_byte(offs_t a)
	{
		if (const u8 *p = find_read(a)) return *p;
		if (device *d = find_dev(a)) {
			if (d->r8)  return d->r8(a);
			if (d->r16) return u8(d->r16(a & ~1u) >> ((a & 1) ? 0 : 8));
			if (d->r32) return u8(d->r32(a & ~3u) >> ((3 - (a & 3)) * 8));
		}
		return 0;
	}

	u16 read_word(offs_t a)
	{
		a &= ~1u;
		if (const u8 *p = find_read(a)) return u16((p[0] << 8) | p[1]);
		if (device *d = find_dev(a)) {
			if (d->r16) return d->r16(a);
			if (d->r32) return u16(d->r32(a & ~3u) >> ((a & 2) ? 0 : 16));
			if (d->r8)  return u16((d->r8(a) << 8) | d->r8(a + 1));
		}
		return 0;
	}

	u32 read_dword(offs_t a)
	{
		a &= ~3u;
		if (const u8 *p = find_read(a))
			return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3];
		if (device *d = find_dev(a)) {
			if (d->r32) return d->r32(a);
		}
		return (u32(read_word(a)) << 16) | read_word(a + 2);
	}

	// ---- 書き込み

	void write_byte(offs_t a, u8 v)
	{
		if (u8 *p = find_write(a)) { *p = v; return; }
		if (device *d = find_dev(a)) {
			if (d->w8)  { d->w8(a, v); return; }
			if (d->w16) { d->w16(a & ~1u, (a & 1) ? v : u16(v) << 8); return; }
			if (d->w32) { d->w32(a & ~3u, u32(v) << ((3 - (a & 3)) * 8)); return; }
		}
	}

	void write_word(offs_t a, u16 v)
	{
		a &= ~1u;
		if (u8 *p = find_write(a)) { p[0] = u8(v >> 8); p[1] = u8(v); return; }
		if (device *d = find_dev(a)) {
			if (d->w16) { d->w16(a, v); return; }
			if (d->w32) { d->w32(a & ~3u, (a & 2) ? v : u32(v) << 16); return; }
			if (d->w8)  { d->w8(a, u8(v >> 8)); d->w8(a + 1, u8(v)); return; }
		}
	}

	void write_dword(offs_t a, u32 v)
	{
		a &= ~3u;
		if (u8 *p = find_write(a)) {
			p[0] = u8(v >> 24); p[1] = u8(v >> 16); p[2] = u8(v >> 8); p[3] = u8(v);
			return;
		}
		if (device *d = find_dev(a)) {
			if (d->w32) { d->w32(a, v); return; }
		}
		write_word(a,     u16(v >> 16));
		write_word(a + 2, u16(v));
	}

private:
	// いまは素の線形探索。登録は 10 個ほどなので実用上は足りるが、
	// ここは音を出すたびに通る場所なので、遅ければページ表に差し替える
	const u8 *find_read(offs_t a)
	{
		for (const auto &r : m_regions)
			if (a >= r.start && a <= r.end) return r.base + (a - r.start);
		return nullptr;
	}

	u8 *find_write(offs_t a)
	{
		for (auto &r : m_regions)
			if (r.writable && a >= r.start && a <= r.end) return r.base + (a - r.start);
		return nullptr;
	}

	device *find_dev(offs_t a)
	{
		for (auto &d : m_devices)
			if (a >= d.start && a <= d.end) return &d;
		return nullptr;
	}

	std::vector<region> m_regions;
	std::vector<device> m_devices;
};

#endif // S_MU2000_MEMBUS_H
