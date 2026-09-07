// license:BSD-3-Clause
//
// CPU が読み書きするバス。
//
// SWP30 の相手（波形 ROM など）は素の配列だったので flat_space で足りたが、
// CPU の空間は ROM・RAM・周辺レジスタが混在するので、番地で振り分ける必要がある。
//
// MAME の address_space は汎用で重い。ここでは MU2000 に必要な形だけを持つ:
//   - 大きな連続領域（ROM / RAM）は配列を直に指す
//   - 周辺（SWP30 のレジスタなど）は関数で受ける
//   - 空間は 32bit、ビッグエンディアン（SH-2）
//
// CPU が実際に使うのは read/write の byte/word/dword の 6 つだけ。

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

	// 周辺。16bit 単位で読み書きする
	struct device {
		u32 start = 0, end = 0;
		std::function<u16(offs_t)>       read16;
		std::function<void(offs_t, u16)> write16;
	};

	void add_region(u32 start, u32 end, void *base, bool writable)
	{
		m_regions.push_back({ start, end, reinterpret_cast<u8 *>(base), writable });
	}

	void add_device(u32 start, u32 end,
	                std::function<u16(offs_t)> rd,
	                std::function<void(offs_t, u16)> wr)
	{
		m_devices.push_back({ start, end, std::move(rd), std::move(wr) });
	}

	// ---- 読み出し。SH-2 はビッグエンディアン

	u8 read_byte(offs_t a)
	{
		if (const u8 *p = find_read(a)) return *p;
		// 周辺は 16bit 単位。上位/下位を切り出す
		if (device *d = find_dev(a))
			return u8(d->read16((a - d->start) >> 1) >> ((a & 1) ? 0 : 8));
		return 0;
	}

	u16 read_word(offs_t a)
	{
		a &= ~1u;
		if (const u8 *p = find_read(a)) return u16((p[0] << 8) | p[1]);
		if (device *d = find_dev(a)) return d->read16((a - d->start) >> 1);
		return 0;
	}

	u32 read_dword(offs_t a)
	{
		a &= ~3u;
		if (const u8 *p = find_read(a))
			return (u32(p[0]) << 24) | (u32(p[1]) << 16) | (u32(p[2]) << 8) | p[3];
		return (u32(read_word(a)) << 16) | read_word(a + 2);
	}

	// ---- 書き込み

	void write_byte(offs_t a, u8 v)
	{
		if (u8 *p = find_write(a)) { *p = v; return; }
		if (device *d = find_dev(a)) {
			const offs_t off = (a - d->start) >> 1;
			const u16 old = d->read16(off);
			d->write16(off, (a & 1) ? u16((old & 0xff00) | v)
			                        : u16((old & 0x00ff) | (u16(v) << 8)));
		}
	}

	void write_word(offs_t a, u16 v)
	{
		a &= ~1u;
		if (u8 *p = find_write(a)) { p[0] = u8(v >> 8); p[1] = u8(v); return; }
		if (device *d = find_dev(a)) d->write16((a - d->start) >> 1, v);
	}

	void write_dword(offs_t a, u32 v)
	{
		a &= ~3u;
		if (u8 *p = find_write(a)) {
			p[0] = u8(v >> 24); p[1] = u8(v >> 16); p[2] = u8(v >> 8); p[3] = u8(v);
			return;
		}
		write_word(a,     u16(v >> 16));
		write_word(a + 2, u16(v));
	}

private:
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
