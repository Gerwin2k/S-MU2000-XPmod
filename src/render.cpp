// MIDI ファイルを食わせて WAV に書き出す。
//
//   render <rom ディレクトリ> <MIDI ファイル> <出力 wav> [秒数]
//
// 実機と同じく、MIDI は 31250bps の直列で MIDI IN A に流し込む。
// 出来た WAV は MAME の録音と突き合わせるためのもの。

#include "mu2000.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct midi_event {
	double  time;      // 秒
	std::vector<u8> bytes;
};

u32 be32(const u8 *p) { return (u32(p[0]) << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }
u16 be16(const u8 *p) { return u16((p[0] << 8) | p[1]); }

// SMF を (秒, バイト列) の並びに開く。format 0/1 の両方に対応する
bool load_smf(const std::string &path, std::vector<midi_event> &out, std::string &err)
{
	std::FILE *f = std::fopen(path.c_str(), "rb");
	if (!f) { err = "MIDI ファイルを開けない: " + path; return false; }
	std::fseek(f, 0, SEEK_END);
	std::vector<u8> d(size_t(std::ftell(f)));
	std::fseek(f, 0, SEEK_SET);
	if (std::fread(d.data(), 1, d.size(), f) != d.size()) {
		std::fclose(f); err = "MIDI ファイルを読めない"; return false;
	}
	std::fclose(f);

	if (d.size() < 14 || std::memcmp(d.data(), "MThd", 4)) {
		err = "MThd がない。標準 MIDI ファイルではないらしい"; return false;
	}
	const u16 ntrk = be16(&d[10]);
	const u16 div  = be16(&d[12]);
	if (div & 0x8000) { err = "SMPTE 単位の MIDI には未対応"; return false; }

	// まずは全トラックを (tick, バイト列) で集める
	struct raw { u64 tick; std::vector<u8> bytes; bool tempo; u32 usec; };
	std::vector<raw> all;

	size_t pos = 8 + be32(&d[4]);
	for (u16 t = 0; t < ntrk && pos + 8 <= d.size(); t++) {
		if (std::memcmp(&d[pos], "MTrk", 4)) break;
		const size_t len = be32(&d[pos + 4]);
		size_t p = pos + 8;
		const size_t end = std::min(p + len, d.size());
		pos = p + len;

		u64 tick = 0;
		u8  running = 0;
		while (p < end) {
			u64 delta = 0;                       // 可変長
			while (p < end) {
				delta = (delta << 7) | (d[p] & 0x7f);
				if (!(d[p++] & 0x80)) break;
			}
			tick += delta;
			if (p >= end) break;

			u8 status = d[p];
			if (status < 0x80) status = running;  // ランニングステータス
			else p++;

			if (status == 0xff) {                 // メタイベント
				const u8 type = d[p++];
				u64 l = 0;
				while (p < end) { l = (l << 7) | (d[p] & 0x7f); if (!(d[p++] & 0x80)) break; }
				if (type == 0x51 && l == 3)
					all.push_back({ tick, {}, true, (u32(d[p]) << 16) | (d[p+1] << 8) | d[p+2] });
				p += size_t(l);
				continue;
			}
			if (status == 0xf0 || status == 0xf7) {   // システムエクスクルーシブ
				u64 l = 0;
				while (p < end) { l = (l << 7) | (d[p] & 0x7f); if (!(d[p++] & 0x80)) break; }
				std::vector<u8> b;
				if (status == 0xf0) b.push_back(0xf0);
				b.insert(b.end(), d.begin() + p, d.begin() + std::min(p + size_t(l), end));
				p += size_t(l);
				all.push_back({ tick, std::move(b), false, 0 });
				continue;
			}

			running = status;
			const int n = ((status & 0xf0) == 0xc0 || (status & 0xf0) == 0xd0) ? 1 : 2;
			std::vector<u8> b{ status };
			for (int i = 0; i < n && p < end; i++) b.push_back(d[p++]);
			all.push_back({ tick, std::move(b), false, 0 });
		}
	}

	std::stable_sort(all.begin(), all.end(),
	                 [](const raw &a, const raw &b) { return a.tick < b.tick; });

	// テンポを追いながら秒に直す
	double sec = 0.0, us_per_beat = 500000.0;   // 既定は 120 BPM
	u64 last = 0;
	for (const raw &e : all) {
		sec += double(e.tick - last) * us_per_beat / (div * 1e6);
		last = e.tick;
		if (e.tempo) { us_per_beat = e.usec; continue; }
		out.push_back({ sec, e.bytes });
	}
	return true;
}

void write_wav(const std::string &path, const std::vector<s16> &pcm, u32 rate)
{
	std::FILE *f = std::fopen(path.c_str(), "wb");
	if (!f) return;
	const u32 bytes = u32(pcm.size() * 2);
	auto u32w = [&](u32 v) { u8 b[4] = { u8(v), u8(v >> 8), u8(v >> 16), u8(v >> 24) };
	                         std::fwrite(b, 1, 4, f); };
	auto u16w = [&](u16 v) { u8 b[2] = { u8(v), u8(v >> 8) }; std::fwrite(b, 1, 2, f); };
	std::fwrite("RIFF", 1, 4, f); u32w(36 + bytes); std::fwrite("WAVE", 1, 4, f);
	std::fwrite("fmt ", 1, 4, f); u32w(16); u16w(1); u16w(2);
	u32w(rate); u32w(rate * 4); u16w(4); u16w(16);
	std::fwrite("data", 1, 4, f); u32w(bytes);
	std::fwrite(pcm.data(), 1, bytes, f);
	std::fclose(f);
}

} // namespace


int main(int argc, char **argv)
{
	if (argc < 4) {
		std::fprintf(stderr,
			"使い方: render <rom ディレクトリ> <MIDI ファイル> <出力 wav> [秒数]\n");
		return 1;
	}
	const std::string dir = argv[1], mid = argv[2], wav = argv[3];
	double seconds = 0.0;
	const char *swptrace = nullptr;
	double boot = -1.0;     // 負なら firmware が受信を有効にするまで待つ
	const char *mu_dac_path = nullptr;
	u32 mu_dac_from = 0, mu_dac_count = 0;
	for (int i = 4; i < argc; i++) {
		if (!std::strcmp(argv[i], "--trace-swp") && i + 1 < argc)
			swptrace = argv[++i];
		else if (!std::strcmp(argv[i], "--boot") && i + 1 < argc)
			boot = std::atof(argv[++i]);
		else if (!std::strcmp(argv[i], "--dump-dac") && i + 3 < argc) {
			mu_dac_path = argv[++i];
			mu_dac_from = u32(std::strtoul(argv[++i], nullptr, 0));
			mu_dac_count = u32(std::strtoul(argv[++i], nullptr, 0));
		}
		else if (!std::strcmp(argv[i], "-v"))
			smu2000::g_verbose = true;
		else
			seconds = std::atof(argv[i]);
	}

	std::vector<midi_event> events;
	std::string err;
	if (!load_smf(mid, events, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
	std::printf("MIDI: %zu イベント、最後は %.2f 秒\n",
	            events.size(), events.empty() ? 0.0 : events.back().time);

	mu2000 mu;
	if (!mu.load_program(dir + "/mu2000_flash.bin")) {
		std::fprintf(stderr, "%s\n", mu.error().c_str()); return 1;
	}
	if (!mu.load_wave(dir + "/dump")) {
		std::fprintf(stderr, "%s\n", mu.error().c_str()); return 1;
	}
	if (!mu.load_sintab(dir + "/standin/sin-table.bin"))
		std::fprintf(stderr, "警告: %s\n", mu.error().c_str());

	std::FILE *tf = swptrace ? std::fopen(swptrace, "w") : nullptr;
	if (tf)
		mu.set_swp_trace(tf, true);

	if (mu_dac_path) {
		mu.swpm().m_dbg_dac = std::fopen(mu_dac_path, "w");
		mu.swpm().m_dbg_dac_from = mu_dac_from;
		mu.swpm().m_dbg_dac_count = mu_dac_count;
	}

	mu.reset();

	if (seconds <= 0.0)
		seconds = (events.empty() ? 0.0 : events.back().time) + 3.0;

	const u32 rate = 44100;
	std::vector<s16> pcm;

	// 起動を待つ。実機も電源投入から数秒は MIDI を受け付けない。
	// 待たずに流すと曲頭のリセットや音色指定が捨てられ、全パートが
	// 初期音色（ピアノ）で鳴り、発音数も足りなくなって音が抜ける。
	// firmware が受信を有効にした時点を印にする
	if (boot < 0.0) {
		const size_t limit = size_t(30.0 * rate);
		size_t i = 0;
		for (; i < limit && !mu.midi_ready(); i++) {
			s32 l = 0, r = 0;
			mu.run_sample(l, r);
			pcm.push_back(s16(std::clamp(l * 32768 / mu2000::DAC_FULL_SCALE, -32768, 32767)));
			pcm.push_back(s16(std::clamp(r * 32768 / mu2000::DAC_FULL_SCALE, -32768, 32767)));
		}
		boot = double(i) / rate;
		if (i >= limit) {
			std::fprintf(stderr, "起動を待ったが MIDI 受信が有効にならなかった\n");
			return 1;
		}
		std::printf("起動に %.2f 秒。ここから MIDI を流す\n", boot);
	}

	const size_t total = size_t((boot + seconds) * rate);
	pcm.reserve(total * 2);

	size_t next = 0;
	for (size_t i = pcm.size() / 2; i < total; i++) {
		const double t = double(i) / rate - boot;
		while (next < events.size() && events[next].time <= t) {
			for (u8 b : events[next].bytes)
				mu.midi_in(b);
			next++;
		}

		s32 l = 0, r = 0;
		mu.run_sample(l, r);
		// DAC の全振幅は 1<<17。16bit に落とす（MAME の 1<<17 目盛りと同じ）
		l = l * 32768 / mu2000::DAC_FULL_SCALE;
		r = r * 32768 / mu2000::DAC_FULL_SCALE;
		pcm.push_back(s16(std::clamp(l, -32768, 32767)));
		pcm.push_back(s16(std::clamp(r, -32768, 32767)));

		if (!(i % (rate * 5)))
			std::printf("  %5.1f 秒  PC=%08x\n", double(i) / rate - boot, mu.cpu().pc());
	}

	if (tf)
		std::fclose(tf);

	if (smu2000::g_verbose)
		std::printf("最大値  AWM2=%d  MEG=%d  DAC=%d\n",
		            mu.swpm().m_dbg_awm_max, mu.swpm().m_dbg_meg_max, mu.swpm().m_dbg_adc_max),
		std::printf("        MEG入力=%d  MELO=%d\n",
		            mu.swpm().m_dbg_megin_max, mu.swpm().m_dbg_melo_max);

	std::printf("CPU %llu サイクル / %zu サンプル = %.3f（あるべき値 %.3f）\n",
	            (unsigned long long)mu.cpu().total_cycles(), total,
	            double(mu.cpu().total_cycles()) / total, 28000000.0 / rate);

	mu.print_swp_widths();

	if (smu2000::g_verbose) {
		auto report = [](const char *name, swp30_device &d) {
			int silent = 0, weak = 0;
			for (auto [e, l] : d.m_dbg_notes) {
				if (!l || e == 0) silent++;
				else if (e / l < 50) weak++;
			}
			std::printf("%s: 発音 %zu 件 無音 %d 件 ごく小さい %d 件\n",
			            name, d.m_dbg_notes.size(), silent, weak);
			int bucket[8] = {};
			for (auto [e, l] : d.m_dbg_notes) {
				if (!l) continue;
				const u64 avg = e / l;
				int k = 0;
				while (k < 7 && avg >= (u64(20) << k)) k++;
				bucket[k]++;
			}
			std::printf("   平均振幅の分布 <20:%d <40:%d <80:%d <160:%d <320:%d <640:%d <1280:%d それ以上:%d\n",
			            bucket[0],bucket[1],bucket[2],bucket[3],bucket[4],bucket[5],bucket[6],bucket[7]);
		};
		report("マスタ", mu.swpm());
		report("スレーブ", mu.swps());
	}

	write_wav(wav, pcm, rate);
	std::printf("書き出した: %s（%.1f 秒）\n", wav.c_str(), double(total) / rate);
	return 0;
}
