// 1 ブロックを作るのに何 ms かかるかを測る。音声デバイスは使わない。
//
//   blocktime <rom ディレクトリ> <MIDI> <ブロックのフレーム数> [秒数]
//
// 待ち時間の下限は「1 ブロックの最悪値 < ブロックの長さ」で決まるので、
// ここで出る最悪値が溜めをどこまで詰められるかの答えになる。
#include "mu2000.h"
#include "smf.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <windows.h>

int main(int argc, char **argv)
{
	if (argc < 4) {
		std::fprintf(stderr, "blocktime <rom> <midi> <frames> [秒]\n");
		return 1;
	}
	const std::string dir = argv[1];
	const int block = std::atoi(argv[3]);
	const double seconds = argc > 4 ? std::atof(argv[4]) : 20.0;
	const u32 RATE = 44100;

	std::vector<smf::event> events;
	std::string err;
	if (!smf::load(argv[2], events, err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }

	mu2000 mu;
	if (!mu.load_program(dir + "/mu2000_flash.bin")) { std::fprintf(stderr, "%s\n", mu.error().c_str()); return 1; }
	if (!mu.load_wave(dir + "/dump")) { std::fprintf(stderr, "%s\n", mu.error().c_str()); return 1; }
	mu.load_sintab(dir + "/standin/sin-table.bin");
	mu.set_threaded(true);
	mu.reset();

	// 起動を待つ（ここは測らない）
	for (u32 i = 0; i < 30 * RATE && !mu.midi_ready(); i++) { s32 l = 0, r = 0; mu.run_sample(l, r); }

	LARGE_INTEGER f; QueryPerformanceFrequency(&f);
	const double tick = 1000.0 / double(f.QuadPart);   // ms

	std::vector<double> ms;
	const u64 total = u64(seconds * RATE);
	size_t next = 0;
	u64 done = 0;
	// 曲は繰り返す。20 秒ぶん回す
	double loop_at = events.empty() ? 0.0 : events.back().time + 0.5;
	double base = 0.0;
	while (done < total) {
		const int n = int(std::min<u64>(u64(block), total - done));
		LARGE_INTEGER t0, t1;
		QueryPerformanceCounter(&t0);
		for (int i = 0; i < n; i++) {
			const double t = double(done + i) / RATE - base;
			while (next < events.size() && events[next].time <= t) {
				for (u8 b : events[next].bytes)
					mu.midi_in(b, events[next].port ? 1 : 0);
				next++;
			}
			if (loop_at > 0.0 && t >= loop_at) { next = 0; base = double(done + i) / RATE; }
			s32 l = 0, r = 0;
			mu.run_sample(l, r);
		}
		QueryPerformanceCounter(&t1);
		ms.push_back(double(t1.QuadPart - t0.QuadPart) * tick);
		done += n;
	}

	std::sort(ms.begin(), ms.end());
	auto pct = [&](double p) { return ms[size_t(p * (ms.size() - 1))]; };
	const double span = 1000.0 * block / RATE;
	double sum = 0.0;
	for (double v : ms) sum += v;
	int over = 0;
	for (double v : ms) if (v > span) over++;
	std::printf("ブロック %d フレーム（%.2f ms ぶん）を %zu 回\n", block, span, ms.size());
	std::printf("  平均 %.2f ms  中央 %.2f  95%% %.2f  99%% %.2f  最悪 %.2f ms\n",
	            sum / ms.size(), pct(0.5), pct(0.95), pct(0.99), ms.back());
	std::printf("  実時間に対する余裕: 平均 %.0f%%  最悪 %.0f%%\n",
	            100.0 * (sum / ms.size()) / span, 100.0 * ms.back() / span);
	std::printf("  ブロックの長さを超えた回数: %d / %zu\n", over, ms.size());
	return 0;
}
