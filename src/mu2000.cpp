// license:BSD-3-Clause
//
// MU2000 一台ぶんの組み立て。配置は MAME の ymmu2000.cpp と同じ。

#include "mu2000.h"

#include <cstdio>
#include <cstring>

namespace {

bool read_file(const std::string &path, std::vector<u8> &out, size_t expect)
{
	std::FILE *f = std::fopen(path.c_str(), "rb");
	if (!f)
		return false;
	std::fseek(f, 0, SEEK_END);
	const long size = std::ftell(f);
	std::fseek(f, 0, SEEK_SET);
	if (expect && size_t(size) != expect) {
		std::fclose(f);
		return false;
	}
	out.resize(size_t(size));
	const size_t got = std::fread(out.data(), 1, out.size(), f);
	std::fclose(f);
	return got == out.size();
}

} // namespace


mu2000::mu2000()
{
	// CPU。MAME は 7MHz の水晶を PLL で 4 倍していた
	m_cpu = &m_config.make<sh7043a_device>(m_cpu_finder, 7000000u * 4);

	// 内蔵周辺を作る。MAME の device_add_mconfig をそのまま呼ぶ
	m_cpu->device_add_mconfig(m_config);

	m_ram.assign(0x40000, 0);        // 256KB
	m_dram.assign(0x80000, 0);       // 512KB
	m_iram.assign(0x1000, 0);        // CPU 内蔵 4KB
	m_sampram.assign(0x400000, 0);   // SWP30 のサンプリング RAM

	build_bus();
}

mu2000::~mu2000() = default;


bool mu2000::load_program(const std::string &path)
{
	if (!read_file(path, m_prog, 0x400000)) {
		m_error = "プログラム ROM を読めない（4MB でないか、見つからない）: " + path;
		return false;
	}
	build_bus();
	return true;
}


bool mu2000::load_wave(const std::string &dir)
{
	// MAME は 4 つの 8MB を 32bit 語に交互に置いている。
	//   ic49 -> 語の下位 16bit（0x0000000 から）
	//   ic50 -> 語の上位 16bit
	//   ic53 / ic54 -> 0x1000000 語目から同じ形で
	static const char *names[4] = {
		"xv364a0.ic49", "xv365a0.ic50", "xw848a0.ic53", "xw849a0.ic54"
	};

	m_wave.assign(0x2000000, 0);     // 32MB
	for (int i = 0; i < 4; i++) {
		std::vector<u8> part;
		const std::string path = dir + "/" + names[i];
		if (!read_file(path, part, 0x800000)) {
			m_error = "波形 ROM を読めない（8MB でないか、見つからない）: " + path;
			return false;
		}
		const size_t base = (i >= 2) ? 0x1000000 : 0;
		const size_t off  = (i & 1) ? 2 : 0;
		for (size_t j = 0; j < part.size(); j += 2) {
			const size_t dst = base + j * 2 + off;
			m_wave[dst + 0] = part[j + 0];
			m_wave[dst + 1] = part[j + 1];
		}
	}

	m_swpm.set_wave_rom(m_wave.data(), m_wave.size());
	m_swps.set_wave_rom(m_wave.data(), m_wave.size());
	return true;
}


bool mu2000::load_sintab(const std::string &path)
{
	std::vector<u8> raw;
	if (!read_file(path, raw, 0x10000)) {
		m_error = "sin 表を読めない（64KB でないか、見つからない）: " + path;
		return false;
	}
	m_sintab.resize(raw.size() / 2);
	for (size_t i = 0; i < m_sintab.size(); i++)
		m_sintab[i] = u16(raw[i * 2] | (raw[i * 2 + 1] << 8));
	m_swpm.set_sintab(m_sintab.data(), m_sintab.size());
	m_swps.set_sintab(m_sintab.data(), m_sintab.size());
	return true;
}


void mu2000::build_bus()
{
	m_bus = mem_bus();

	// 000000-3fffff: プログラム ROM
	if (!m_prog.empty())
		m_bus.add_region(0x000000, 0x3fffff, m_prog.data(), false);
	// 400000-43ffff: ワーク RAM
	m_bus.add_region(0x400000, 0x43ffff, m_ram.data(), true);
	// 1000000-107ffff: DRAM
	m_bus.add_region(0x1000000, 0x107ffff, m_dram.data(), true);
	// fffff000-ffffffff: CPU 内蔵 RAM
	m_bus.add_region(0xfffff000, 0xffffffff, m_iram.data(), true);

	// 800000-801fff: SWP30 マスタ / 802000-803fff: スレーブ。
	// レジスタは 16bit 単位なので、番地を 2 で割って渡す
	auto swp = [this](swp30_device &dev, u32 base) {
		mem_bus::device d;
		d.start = base;
		d.end   = base + 0x1fff;
		d.r16 = [this, &dev, base](offs_t a) {
			const u16 v = dev.read16((a - base) >> 1);
			if (m_swp_trace && m_swp_trace_reads)
				std::fprintf(m_swp_trace, "R %08x %04x %04x  pc=%08x\n", base, (a - base) >> 1, v, m_cpu->pc());
			return v;
		};
		d.w16 = [this, &dev, base](offs_t a, u16 v) {
			if (m_swp_trace)
				std::fprintf(m_swp_trace, "%s%08x %04x %04x  pc=%08x\n",
				             m_swp_trace_reads ? "W " : "", base, (a - base) >> 1, v, m_cpu->pc());
			dev.write16((a - base) >> 1, v);
		};
		return d;
	};
	m_bus.add_device(swp(m_swpm, 0x800000));
	m_bus.add_device(swp(m_swps, 0x802000));

	// c80000: LED ラッチとスイッチ走査、e00000: LED ラッチその 2。
	// 音には関わらないが、firmware が起動時に触るので受けておく
	{
		mem_bus::device d;
		d.start = 0xc80000; d.end = 0xc80000;
		d.r8 = [this](offs_t) { return u8(0xff); };   // スイッチは全部離した状態
		d.w8 = [this](offs_t, u8 v) { m_ledsw1 = v; };
		m_bus.add_device(d);
	}
	{
		mem_bus::device d;
		d.start = 0xe00000; d.end = 0xe00000;
		d.w8 = [this](offs_t, u8 v) { m_ledsw2 = v; };
		m_bus.add_device(d);
	}

	// f00000-f0003f: PLG ボード用の SCI4。ボードを挿さないので空
	{
		mem_bus::device d;
		d.start = 0xf00000; d.end = 0xf0003f;
		d.r8  = [](offs_t) { return u8(0); };
		d.r16 = [](offs_t) { return u16(0); };
		d.w8  = [](offs_t, u8) {};
		d.w16 = [](offs_t, u16) {};
		m_bus.add_device(d);
	}

	// ffff8000-ffff9fff: CPU の内蔵周辺（sh7042_map.hxx が振り分ける）
	{
		mem_bus::device d;
		d.start = 0xffff8000; d.end = 0xffff9fff;
		d.r8  = [this](offs_t a) { return m_cpu->internal_r8(a); };
		d.r16 = [this](offs_t a) { return m_cpu->internal_r16(a); };
		d.r32 = [this](offs_t a) { return m_cpu->internal_r32(a); };
		d.w8  = [this](offs_t a, u8 v)  { m_cpu->internal_w8(a, v); };
		d.w16 = [this](offs_t a, u16 v) { m_cpu->internal_w16(a, v); };
		d.w32 = [this](offs_t a, u32 v) { m_cpu->internal_w32(a, v); };
		m_bus.add_device(d);
	}

	m_cpu->set_program_bus(&m_bus);
}


void mu2000::start_devices()
{
	// MAME はスケジューラが順に呼ぶ。こちらは生成順にそのまま呼ぶ
	for (auto &d : m_config.m_devices)
		d->device_start();
}


void mu2000::reset()
{
	// ポートの既定値。MAME の mu500_state::pa_r は 0xffff を返していた
	m_cpu->read_porta().set([]() { return u32(0xffff); });
	m_cpu->read_porte().set([this]() { return u16(0); });
	m_cpu->write_porte().set([this](u16 v) { m_pe = v; });

	m_swpm.reset();
	m_swps.reset();

	start_devices();

	for (auto &d : m_config.m_devices)
		d->device_reset();
}


void mu2000::run_cycles(u64 n)
{
	// MAME ではスケジューラがやっていたこと。周辺の予定を跨がないように区切る。
	// MAME は予定の時刻ちょうどで CPU を止めてタイマを鳴らし、そのあと再開する。
	// 周辺がレジスタ書き込みに反応して新しい予定を入れた場合は、CPU が
	// abort_timeslice() でその場で戻ってくるので、ここで組み直す
	int idle = 0;
	while (n) {
		const u64 now = m_cpu->total_cycles();
		const u64 ev  = m_cpu->event_cycles();

		if (ev && now >= ev) {
			m_cpu->event_tick();
			if (m_cpu->event_cycles() == ev && ++idle > 2)
				break;          // 予定が動かない。放っておくと止まる
			continue;
		}
		idle = 0;

		u64 chunk = n;
		if (ev && ev - now < chunk)
			chunk = ev - now;

		const int done = m_cpu->run_cycles(int(chunk));
		if (done <= 0) {
			if (m_cpu->event_cycles() == ev)
				break;
			continue;
		}
		n -= u64(done) < n ? u64(done) : n;
	}
}


void mu2000::run_sample(s32 &left, s32 &right)
{
	// SWP30 は 44100Hz で 1 サンプル。CPU はその間に 28MHz/44100 ≒ 634.9 サイクル
	m_cycle_debt += 28000000;
	const u64 cycles = m_cycle_debt / 44100;
	m_cycle_debt -= cycles * 44100;

	run_cycles(cycles);

	// マスタとスレーブを 1 サンプルずつ。出力は足す
	s32 lm = 0, rm = 0, ls = 0, rs = 0;
	m_swpm.run_sample(lm, rm);
	m_swps.run_sample(ls, rs);
	left  = lm + ls;
	right = rm + rs;
}
