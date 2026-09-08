// license:BSD-3-Clause
//
// MU2000 一台ぶんの組み立て。配置は MAME の ymmu2000.cpp と同じ。

#include "mu2000.h"

#include <cstdio>
#include <cstring>

#include <immintrin.h>


namespace {

// MIDI は 31250bps。28MHz の CPU から見て 1 ビット = 896 サイクル
constexpr u64 MIDI_BIT_CYCLES = 28000000 / 31250;

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

	// PLG ボード用のシリアル。ボードは挿さないが、firmware はレジスタを触る
	m_sci4 = &m_config.make<sci4_device>(m_sci4_finder);

	// 時計とタイマの置き場を全デバイスに配る
	m_machine.set_clock_hz(7000000 * 4);
	for (auto &d : m_config.m_devices)
		d->set_machine(&m_machine);

	m_ram.assign(0x40000, 0);        // 256KB
	m_dram.assign(0x80000, 0);       // 512KB
	m_iram.assign(0x1000, 0);        // CPU 内蔵 4KB
	m_sampram.assign(0x400000, 0);   // SWP30 のサンプリング RAM

	build_bus();
}

mu2000::~mu2000()
{
	set_threaded(false);
}

// スレーブを別スレッドで回す。1 サンプルの中では 2 個の SWP30 は
// 互いに独立しているので、並べて走らせても出る音は変わらない
void mu2000::set_threaded(bool on)
{
	if (on == m_slave_thread.joinable())
		return;

	if (!on) {
		m_slave_quit = true;
		m_slave_go++;
		m_slave_go.notify_one();
		m_slave_thread.join();
		m_slave_quit = false;
		return;
	}
	m_slave_thread = std::thread([this] { slave_loop(); });
}

// 空振りを何回続けたら眠るか。0 以下なら永久に回す（比較用）
#ifndef SLAVE_SPINS
#define SLAVE_SPINS 20000
#endif

void mu2000::slave_loop()
{
	u64 seen = 0;
	for (;;) {
		// 合図を待つ。1 サンプルの中の待ちは 1 マイクロ秒に満たないので、
		// まず回して待つ。眠っていては 44100 回/秒には間に合わない。
		//
		// ただし DAW の中では、1 ブロック作り終えてから次に呼ばれるまでの
		// 数ミリ秒がまるごと空く。そこまで回し続けると 1 コアを常時
		// 焼くことになるので、しばらく空振りしたら本当に眠る
		int spins = 0;
		while (m_slave_go.load(std::memory_order_acquire) == seen) {
			if (m_slave_quit.load(std::memory_order_relaxed))
				return;
			if (SLAVE_SPINS <= 0 || ++spins < SLAVE_SPINS)
				_mm_pause();
			else
				m_slave_go.wait(seen, std::memory_order_acquire);
		}
		seen = m_slave_go.load(std::memory_order_acquire);
		if (m_slave_quit.load(std::memory_order_relaxed))
			return;

		m_slave_l = m_slave_r = 0;
		m_swps.run_sample(m_slave_l, m_slave_r);
		m_slave_done.store(seen, std::memory_order_release);
	}
}


bool mu2000::load_program(const std::string &path)
{
	auto rom = std::make_shared<std::vector<u8>>();
	if (!read_file(path, *rom, 0x400000)) {
		m_error = "プログラム ROM を読めない（4MB でないか、見つからない）: " + path;
		return false;
	}
	set_program_rom(std::move(rom));
	return true;
}

// ROM は読むだけなので、何台の MU2000 で分け合っても構わない。
// VST3 を複数挿したときに 36MB を人数分持たずに済む
void mu2000::set_program_rom(u8rom p)
{
	m_prog = std::move(p);
	build_bus();
}

void mu2000::set_wave_rom(u8rom p)
{
	m_wave = std::move(p);
	if (!m_wave)
		return;
	m_swpm.set_wave_rom(m_wave->data(), m_wave->size());
	m_swps.set_wave_rom(m_wave->data(), m_wave->size());
}

void mu2000::set_sintab_rom(u16rom p)
{
	m_sintab = std::move(p);
	if (!m_sintab)
		return;
	m_swpm.set_sintab(m_sintab->data(), m_sintab->size());
	m_swps.set_sintab(m_sintab->data(), m_sintab->size());
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

	auto rom = std::make_shared<std::vector<u8>>(0x2000000, 0);   // 32MB
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
			(*rom)[dst + 0] = part[j + 0];
			(*rom)[dst + 1] = part[j + 1];
		}
	}

	set_wave_rom(std::move(rom));
	return true;
}


bool mu2000::load_sintab(const std::string &path)
{
	std::vector<u8> raw;
	if (!read_file(path, raw, 0x10000)) {
		m_error = "sin 表を読めない（64KB でないか、見つからない）: " + path;
		return false;
	}
	auto rom = std::make_shared<std::vector<u16>>(raw.size() / 2);
	for (size_t i = 0; i < rom->size(); i++)
		(*rom)[i] = u16(raw[i * 2] | (raw[i * 2 + 1] << 8));
	set_sintab_rom(std::move(rom));
	return true;
}


void mu2000::build_bus()
{
	m_bus = mem_bus();

	// 000000-3fffff: プログラム ROM
	if (m_prog && !m_prog->empty())
		m_bus.add_region(0x000000, 0x3fffff, m_prog->data(), false);
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
		// 幅の内訳を数える。MAME は 16bit ハンドラに mem_mask を渡せるが
		// こちらは渡せないので、byte 幅の書き込みがあると片側が壊れる
		d.w8 = [this, &dev, base](offs_t a, u8 v) {
			m_swp_w8++;
			const offs_t reg = (a - base) >> 1;
			const u16 old = dev.read16(reg);
			dev.write16(reg, (a & 1) ? u16((old & 0xff00) | v)
			                         : u16((old & 0x00ff) | (u16(v) << 8)));
		};
		d.r8 = [this, &dev, base](offs_t a) {
			m_swp_r8++;
			return u8(dev.read16((a - base) >> 1) >> ((a & 1) ? 0 : 8));
		};
		d.w32 = [this, &dev, base](offs_t a, u32 v) {
			m_swp_w32++;
			const offs_t reg = (a - base) >> 1;
			if (m_swp_trace) {
				std::fprintf(m_swp_trace, "%s%08x %04x %04x  pc=%08x\n",
				             m_swp_trace_reads ? "W " : "", base, reg, u16(v >> 16), m_cpu->pc());
				std::fprintf(m_swp_trace, "%s%08x %04x %04x  pc=%08x\n",
				             m_swp_trace_reads ? "W " : "", base, reg + 1, u16(v), m_cpu->pc());
			}
			dev.write16(reg, u16(v >> 16));
			dev.write16(reg + 1, u16(v));
		};
		d.w16 = [this, &dev, base](offs_t a, u16 v) {
			m_swp_w16++;
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

	// f00000-f0003f: PLG ボード用の SCI4。ボードは挿さないが register は生きている
	{
		mem_bus::device d;
		d.start = 0xf00000; d.end = 0xf0003f;
		d.r8 = [this](offs_t a) { return m_sci4->read8(a - 0xf00000); };
		d.w8 = [this](offs_t a, u8 v) { m_sci4->write8(a - 0xf00000, v); };
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


// ポート E は LCD の 8bit バス。上位バイトがデータ、下位が制御線。
// MAME の mu500_state::pe_r / pe_w と同じ形にしてある
//   bit 4: E（立ち下がりで確定）  bit 2: RS（1 でデータ）  bit 0: R/W
u16 mu2000::lcd_port_r()
{
	m_lcd.set_now(m_cpu->total_cycles());
	if (BIT(m_pe, 4)) {
		if (BIT(m_pe, 0))
			return u16((BIT(m_pe, 2) ? m_lcd.data_r() : m_lcd.control_r()) << 8);
		return 0x0000;
	}
	return 0;
}

void mu2000::lcd_port_w(u16 data)
{
	m_lcd.set_now(m_cpu->total_cycles());
	if (BIT(m_pe, 4) && !BIT(data, 4)) {        // E の立ち下がり
		if (!BIT(data, 0)) {                    // R/W = 0、つまり書き込み
			if (BIT(data, 2))
				m_lcd.data_w(u8(data >> 8));
			else
				m_lcd.control_w(u8(data >> 8));
		}
	}
	m_pe = data;
}

void mu2000::update_sci_irq()
{
	m_cpu->execute_set_input(0, (m_sci_irq[0] || m_sci_irq[1]) ? ASSERT_LINE : CLEAR_LINE);
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
	m_cpu->read_porte().set([this]() { return lcd_port_r(); });
	m_cpu->write_porte().set([this](u16 v) { lcd_port_w(v); });

	m_lcd.reset();

	// SCI4 の割り込み。MAME は 0 と 1 を input_merger で束ねて CPU の IRQ0 に、
	// 3 を IRQ1 に入れていた
	m_sci4->write_irq<0>().set([this](int s) { m_sci_irq[0] = s; update_sci_irq(); });
	m_sci4->write_irq<1>().set([this](int s) { m_sci_irq[1] = s; update_sci_irq(); });
	m_sci4->write_irq<3>().set([this](int s) { m_cpu->execute_set_input(1, s); });

	m_swpm.reset();
	m_swps.reset();

	// MIDI IN の線は何も来ていないとき High
	m_cpu->sci_rx_w<0>(1);
	m_cpu->sci_rx_w<1>(1);

	start_devices();

	for (auto &d : m_config.m_devices)
		d->device_reset();
}


void mu2000::run_cycles(u64 n)
{
	// 前回はみ出した分を先に返す
	if (m_overrun >= n) { m_overrun -= n; return; }
	n -= m_overrun;
	m_overrun = 0;

	// MAME ではスケジューラがやっていたこと。周辺の予定を跨がないように区切る。
	// MAME は予定の時刻ちょうどで CPU を止めてタイマを鳴らし、そのあと再開する。
	// 周辺がレジスタ書き込みに反応して新しい予定を入れた場合は、CPU が
	// abort_timeslice() でその場で戻ってくるので、ここで組み直す
	int idle = 0;
	while (n) {
		m_loops++;
		const u64 now = m_cpu->total_cycles();
		m_machine.set_cycles(now);

		// MAME のスケジューラが持っていたタイマ（SCI4 の送受信など）
		const u64 tmr = m_machine.next_timer_cycles();
		if (tmr <= now) {
			m_timer_fires++;
			m_machine.run_timers(now);
			m_machine.set_cycles(now);
			continue;
		}

		const u64 ev  = m_cpu->event_cycles();

		if (ev && now >= ev) {
			m_event_fires++;
			m_cpu->event_tick();
			if (m_cpu->event_cycles() == ev && ++idle > 2)
				break;          // 予定が動かない。放っておくと止まる
			continue;
		}
		idle = 0;

		// MIDI のビット送出も跨がないように
		midi_step(now);

		u64 chunk = n;
		if (ev && ev - now < chunk)
			chunk = ev - now;
		if (tmr != ~u64(0) && tmr - now < chunk)
			chunk = tmr - now;
		if (m_midi_bit >= 0 || !m_midi_queue.empty()) {
			const u64 left = m_midi_next > now ? m_midi_next - now : 1;
			if (left < chunk)
				chunk = left;
		}

		const int done = m_cpu->run_cycles(int(chunk));
		if (done <= 0) {
			if (m_cpu->event_cycles() == ev)
				break;
			continue;
		}
		// 命令の途中では止まれないので、頼まれた数より少し多く走ることがある。
		// 出た分は捨てずに次の呼び出しから引く（捨てると CPU が音より速くなる）
		if (u64(done) >= n) {
			m_overrun += u64(done) - n;
			n = 0;
		} else
			n -= u64(done);
	}
}

void mu2000::midi_step(u64 now)
{
	if (m_midi_bit < 0) {
		// 直前のバイトのストップビットぶんは空けてから次を出す
		if (m_midi_queue.empty() || now < m_midi_next)
			return;
		m_midi_cur = m_midi_queue.front();
		m_midi_queue.pop_front();
		m_midi_bit  = 0;
		m_midi_next = now + MIDI_BIT_CYCLES;
		logerror("midi in %02x @ %llu\n", m_midi_cur, (unsigned long long)now);
		m_cpu->sci_rx_w<0>(0);          // スタートビット
		return;
	}

	if (now < m_midi_next)
		return;

	m_midi_bit++;
	m_midi_next = now + MIDI_BIT_CYCLES;
	if (m_midi_bit <= 8)
		m_cpu->sci_rx_w<0>((m_midi_cur >> (m_midi_bit - 1)) & 1);   // 下位ビットから
	else {
		m_cpu->sci_rx_w<0>(1);          // ストップビット
		m_midi_bit = -1;
	}
}


void mu2000::run_sample(s32 &left, s32 &right)
{
	// SWP30 は 44100Hz で 1 サンプル。CPU はその間に 28MHz/44100 ≒ 634.9 サイクル
	m_cycle_debt += 28000000;
	const u64 cycles = m_cycle_debt / 44100;
	m_cycle_debt -= cycles * 44100;

	run_cycles(cycles);

	// マスタとスレーブを 1 サンプルずつ進める。
	// 別スレッドが空いていればスレーブをそちらに投げ、同時に走らせる
	s32 lm = 0, rm = 0, ls = 0, rs = 0;
	if (m_slave_thread.joinable()) {
		const u64 tag = m_slave_go.load(std::memory_order_relaxed) + 1;
		m_slave_go.store(tag, std::memory_order_release);
		m_slave_go.notify_one();   // 眠っていたら起こす。起きていれば素通り
		m_swpm.run_sample(lm, rm);
		while (m_slave_done.load(std::memory_order_acquire) != tag)
			_mm_pause();
		ls = m_slave_l;
		rs = m_slave_r;
	} else {
		m_swpm.run_sample(lm, rm);
		m_swps.run_sample(ls, rs);
	}

	// 2 個の SWP30 は MELO/MELI のシリアルで相互に結ばれている。
	// スレーブの声は自分の DAC には出ず、この線でマスタのミキサに入る。
	// 結線は MAME の mu1000_state::mu1000() と同じ:
	//   スレーブ 出力 4..17 -> マスタ  入力 0..13
	//   マスタ   出力 4..13 -> スレーブ 入力 0..9
	// 相互に繋がっているので 1 サンプル遅れで渡す（MAME も同じ）
	for (int i = 0; i < 14; i++)
		m_swpm.set_meli(i, m_swps.melo(i));
	for (int i = 0; i < 10; i++)
		m_swps.set_meli(i, m_swpm.melo(i));

	// スピーカーに出るのはマスタの DAC だけ。
	// スレーブの DAC はどこにも繋がっていない
	left  = lm;
	right = rm;
}
