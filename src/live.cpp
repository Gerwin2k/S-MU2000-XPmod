// license:BSD-3-Clause
//
// Windows の MIDI 入力を受けて、そのまま音を鳴らす。
//
//   live --list                          MIDI 入力の一覧
//   live <rom ディレクトリ> [--midi 番号] [--latency ミリ秒]
//   live <rom ディレクトリ> --waveout    古い方式（WinMM）で鳴らす
//
// 設計の要点は doc/design.md にあるとおりで、**時計を自分で持たない**こと。
// 音声デバイスが「N サンプルくれ」と要求した分だけ CPU と音源を進める。
// こうするとホストとずれようがない。MAME が外部同期で破綻したのはここの違い。
//
// 音声出力は 2 通り持っている。
//   WASAPI 共有モード（既定）  イベント駆動。待ち時間は Windows の周期に従う
//   WinMM waveOut (--waveout)  素直だが、この環境では 1 枚 23ms を割ると
//                              供給が追いつかず細切れになる（合計 70ms 必要）

#include "mu2000.h"
#include "ui/audio_out.h"
#include "ui/midi_in.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <mmsystem.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>

namespace {

constexpr u32 RATE = 44100;

// ---- MIDI 入力。輪っかも SysEx の受け皿も ui::midi_in が持っている
// （gui.exe と同じもの。**SysEx の入れ物を Windows へ渡す**のもそちら）

ui::midi_in g_midi;

void list_midi_inputs()
{
	const UINT n = midiInGetNumDevs();
	if (!n) {
		std::printf("MIDI 入力が見つからない\n");
		return;
	}
	std::printf("MIDI 入力:\n");
	for (UINT i = 0; i < n; i++) {
		MIDIINCAPSA caps{};
		if (midiInGetDevCapsA(i, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
			std::printf("  %u: %s\n", i, caps.szPname);
	}
}

// 音声スレッドを MMCSS へ登録する。SetThreadPriority だけでは、
// 他の仕事のために数十ミリ秒まとめて止められることがある
struct mmcss_guard {
	HANDLE h = nullptr;
	mmcss_guard()
	{
		DWORD idx = 0;
		h = AvSetMmThreadCharacteristicsW(L"Pro Audio", &idx);
		if (h) AvSetMmThreadPriority(h, AVRT_PRIORITY_CRITICAL);
	}
	~mmcss_guard() { if (h) AvRevertMmThreadCharacteristics(h); }
};


// ---- 音を作る側。どちらの出力方式からもこれを呼ぶ

struct generator {
	mu2000 &mu;
	std::vector<s16> *rec;          // 確認用の録音。要らなければ nullptr
	LARGE_INTEGER freq{};
	u64 busy_ticks = 0, produced = 0, late = 0, worst_ticks = 0;
	// 取りこぼしの判定に使う「一杯ぶん」の長さ。出力方式が決める
	u32 cushion_frames = 0;
	u64 starved = 0;      // デバイスの残量がゼロになった回数（本当の枯渇）

	generator(mu2000 &m, std::vector<s16> *r) : mu(m), rec(r)
	{
		QueryPerformanceFrequency(&freq);
	}

	// n サンプルぶん作って out に書く（16bit 2ch のインタリーブ）
	void fill(s16 *out, u32 n)
	{
		LARGE_INTEGER t0, t1;
		QueryPerformanceCounter(&t0);

		// 溜まっている MIDI を音源へ。実機と同じく 31250bps の直列で流れる
		u8 b;
		while (g_midi.pop(b))
			mu.midi_in(b);

		for (u32 i = 0; i < n; i++) {
			s32 l = 0, r = 0;
			mu.run_sample(l, r);
			l = l * 32768 / mu2000::DAC_FULL_SCALE;
			r = r * 32768 / mu2000::DAC_FULL_SCALE;
			out[i * 2 + 0] = s16(l < -32768 ? -32768 : l > 32767 ? 32767 : l);
			out[i * 2 + 1] = s16(r < -32768 ? -32768 : r > 32767 ? 32767 : r);
		}

		QueryPerformanceCounter(&t1);
		const u64 one = u64(t1.QuadPart - t0.QuadPart);
		busy_ticks += one;
		if (one > worst_ticks) worst_ticks = one;
		// 取りこぼすのは、1 回の生成が「溜めてある量」を超えたとき。
		// 頼まれた n は回ごとに変わるので、n と比べても意味がない
		if (cushion_frames && double(one) / freq.QuadPart > double(cushion_frames) / RATE)
			late++;

		if (rec)
			rec->insert(rec->end(), out, out + size_t(n) * 2);
		produced += n;
	}

	void report(u32 period_frames) const
	{
		const double audio = double(produced) / RATE;
		const double busy  = double(busy_ticks) / freq.QuadPart;
		std::printf("  %.0f 秒経過  MIDI %llu バイト  CPU 使用率 %.1f%%\n",
		            audio, (unsigned long long)g_midi.bytes(), 100.0 * busy / audio);
		std::printf("     枯渇 %llu 回（残量ゼロ）、生成の最悪 %.1f ms（溜めは %.1f ms ぶん）\n",
		            (unsigned long long)starved, 1000.0 * worst_ticks / freq.QuadPart,
		            1000.0 * cushion_frames / RATE);
	}
};

// ---- WASAPI 共有モード。イベント駆動

// ---- WASAPI 共有モード。ui::audio_out に任せる
//
// 以前はここに WASAPI の手順を直に書いていたが、gui.exe 側（ui::audio_out）と
// 二重になっていた。**標本化周波数の変換を自分でやる**ようにした分が
// 片方にしか入らないのは困るので、こちらもそちらを使う。

int run_wasapi(generator &gen, double seconds, int latency_ms)
{
	ui::audio_out out;
	std::string err;
	if (!out.start(latency_ms, [&gen](s16 *o, u32 n) { gen.fill(o, n); }, err)) {
		std::fprintf(stderr, "%s\n", err.c_str());
		return 1;
	}

	std::printf("WASAPI 共有モード  %s\n", out.format_line().c_str());
	std::printf("MMCSS: %s\n", out.mmcss() ? "Pro Audio で登録した"
	                                        : "登録できず（途切れやすい）");
	if (seconds > 0.0)
		std::printf("%.1f 秒で終了\n", seconds);
	else
		std::printf("Ctrl+C で終了\n");

	// 取りこぼしの判定に使う「一杯ぶん」。デバイス側の長さを 44100 側に直す
	gen.cushion_frames = u32(out.buffer_ms() * RATE / 1000.0);

	u64 shown = 0;
	while (seconds <= 0.0 || gen.produced < u64(seconds * RATE)) {
		Sleep(20);
		if (!out.running()) {
			const std::string e = out.error();
			if (!e.empty())
				std::fprintf(stderr, "%s\n", e.c_str());
			break;
		}
		if (gen.produced - shown >= u64(RATE) * 5) {
			shown = gen.produced;
			gen.starved = out.starved();
			gen.report(gen.cushion_frames);
		}
	}

	gen.starved = out.starved();
	out.stop();
	return 0;
}

// ---- WinMM waveOut。素直だが待ち時間を詰められない

int run_waveout(generator &gen, double seconds, int frames, int buffers)
{
	WAVEFORMATEX fmt{};
	fmt.wFormatTag      = WAVE_FORMAT_PCM;
	fmt.nChannels       = 2;
	fmt.nSamplesPerSec  = RATE;
	fmt.wBitsPerSample  = 16;
	fmt.nBlockAlign     = 4;
	fmt.nAvgBytesPerSec = RATE * 4;

	HANDLE done = CreateEventA(nullptr, FALSE, FALSE, nullptr);
	HWAVEOUT hwo = nullptr;
	if (waveOutOpen(&hwo, WAVE_MAPPER, &fmt, DWORD_PTR(done), 0,
	                CALLBACK_EVENT) != MMSYSERR_NOERROR) {
		std::fprintf(stderr, "音声デバイスを開けない\n");
		return 1;
	}

	std::vector<std::vector<s16>> pcm(buffers, std::vector<s16>(size_t(frames) * 2));
	std::vector<WAVEHDR> hdr(buffers);
	for (int i = 0; i < buffers; i++) {
		hdr[i] = {};
		hdr[i].lpData         = reinterpret_cast<LPSTR>(pcm[i].data());
		hdr[i].dwBufferLength = DWORD(pcm[i].size() * 2);
		waveOutPrepareHeader(hwo, &hdr[i], sizeof(WAVEHDR));
	}

	std::printf("waveOut  待ち時間 %.1f ms（%d サンプル × %d 枚）\n",
	            1000.0 * frames * buffers / RATE, frames, buffers);
	if (frames < 1024)
		std::printf("警告: 1 枚が %.1f ms しかない。waveOut は 20ms 前後の間隔でしか\n"
		            "      回収しないので、これより短いと供給が追いつかず細切れになる\n",
		            1000.0 * frames / RATE);
	if (seconds > 0.0)
		std::printf("%.1f 秒で終了\n", seconds);
	else
		std::printf("Ctrl+C で終了\n");

	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
	mmcss_guard mmcss;
	gen.cushion_frames = u32(frames) * u32(buffers);

	// 先に全枚を投入し、以後は投入した順に完了を待つ。空きを探し回ると、
	// waveOutWrite の直後でまだ WHDR_INQUEUE が立っていない枚を選び直して
	// 再生中の枚を上書きしうる
	auto emit = [&](int i) {
		gen.fill(pcm[i].data(), u32(frames));
		waveOutWrite(hwo, &hdr[i], sizeof(WAVEHDR));
	};
	for (int i = 0; i < buffers; i++)
		emit(i);

	int next = 0;
	while (seconds <= 0.0 || gen.produced < u64(seconds * RATE)) {
		while (!(hdr[next].dwFlags & WHDR_DONE))
			WaitForSingleObject(done, 100);
		emit(next);
		next = (next + 1) % buffers;
		if (gen.produced % (RATE * 5) < u64(frames))
			gen.report(u32(frames));
	}

	waveOutReset(hwo);
	for (int i = 0; i < buffers; i++)
		waveOutUnprepareHeader(hwo, &hdr[i], sizeof(WAVEHDR));
	waveOutClose(hwo);
	CloseHandle(done);
	return 0;
}

void write_wav(const char *path, const std::vector<s16> &pcm)
{
	std::FILE *f = std::fopen(path, "wb");
	if (!f)
		return;
	const u32 bytes = u32(pcm.size() * 2);
	auto w32 = [&](u32 v) { u8 b[4] = { u8(v), u8(v >> 8), u8(v >> 16), u8(v >> 24) };
	                        std::fwrite(b, 1, 4, f); };
	auto w16 = [&](u16 v) { u8 b[2] = { u8(v), u8(v >> 8) }; std::fwrite(b, 1, 2, f); };
	std::fwrite("RIFF", 1, 4, f); w32(36 + bytes); std::fwrite("WAVE", 1, 4, f);
	std::fwrite("fmt ", 1, 4, f); w32(16); w16(1); w16(2);
	w32(RATE); w32(RATE * 4); w16(4); w16(16);
	std::fwrite("data", 1, 4, f); w32(bytes);
	std::fwrite(pcm.data(), 1, bytes, f);
	std::fclose(f);
	std::printf("録音を書き出した: %s\n", path);
}

} // namespace


int main(int argc, char **argv)
{
	SetConsoleOutputCP(CP_UTF8);   // 既定の CP932 だと表示が化ける

	int  midi_dev = -1;
	int  frames = 1024;     // waveOut のときの 1 枚（23.2ms）
	int  buffers = 3;
	// WASAPI の待ち時間。0 を渡すと Windows の最小周期（この環境で 23.5ms）に
	// なるが、それだと音源の山で 25 秒に 1 回ほど枯渇する。30ms なら 0 回。
	// これ以上詰めたければ音源をもっと速くするしかない
	int  latency_ms = 30;
	double seconds = 0.0;   // 0 なら Ctrl+C まで
	bool nomidi = false, use_waveout = false, single = false;
	const char *wav = nullptr;
	std::string dir;

	for (int i = 1; i < argc; i++) {
		if (!std::strcmp(argv[i], "--list")) { list_midi_inputs(); return 0; }
		else if (!std::strcmp(argv[i], "--midi") && i + 1 < argc) midi_dev = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frames = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--buffers") && i + 1 < argc) buffers = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--latency") && i + 1 < argc) latency_ms = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = std::atof(argv[++i]);
		else if (!std::strcmp(argv[i], "--wav") && i + 1 < argc) wav = argv[++i];
		else if (!std::strcmp(argv[i], "--waveout")) use_waveout = true;
		else if (!std::strcmp(argv[i], "--nomidi")) nomidi = true;
		else if (!std::strcmp(argv[i], "--single"))
			single = true;
		else if (!std::strcmp(argv[i], "-v")) smu2000::g_verbose = true;
		else if (dir.empty()) dir = argv[i];
	}
	if (dir.empty()) {
		std::fprintf(stderr,
			"使い方: live <rom ディレクトリ> [--midi 番号] [--latency ミリ秒]\n"
			"        live <rom ディレクトリ> --waveout [--frames 数] [--buffers 数]\n"
			"        live --list        MIDI 入力の一覧\n");
		return 1;
	}

	mu2000 mu;
	if (!mu.load_program(dir + "/mu2000_flash.bin")) {
		std::fprintf(stderr, "%s\n", mu.error().c_str()); return 1;
	}
	if (!mu.load_wave(dir + "/dump")) {
		std::fprintf(stderr, "%s\n", mu.error().c_str()); return 1;
	}
	if (!mu.load_sintab(dir + "/standin/sin-table.bin"))
		std::fprintf(stderr, "警告: %s\n", mu.error().c_str());

	mu.set_threaded(!single);
	mu.reset();

	// 起動を待つ。実機と同じで、ここを待たないと音色指定が捨てられる
	std::printf("起動中...");
	std::fflush(stdout);
	{
		const size_t limit = size_t(30.0 * RATE);
		size_t i = 0;
		s32 l, r;
		for (; i < limit && !mu.midi_ready(); i++)
			mu.run_sample(l, r);
		if (i >= limit) {
			std::fprintf(stderr, "\n起動しなかった\n");
			return 1;
		}
		std::printf(" %.2f 秒\n", double(i) / RATE);
	}

	// ---- MIDI 入力
	if (nomidi) midi_dev = -1;
	else if (midi_dev < 0 && midiInGetNumDevs() > 0)
		midi_dev = 0;
	if (midi_dev >= 0) {
		std::string merr;
		if (!g_midi.open(midi_dev, merr)) {
			std::fprintf(stderr, "MIDI 入力 %d: %s\n", midi_dev, merr.c_str());
			return 1;
		}
		std::printf("MIDI 入力: %d: %s\n", midi_dev, g_midi.device_name().c_str());
	} else
		std::printf("MIDI 入力なし（音は出るが何も鳴らない）\n");

	std::vector<s16> rec;
	generator gen(mu, wav ? &rec : nullptr);

	const int rc = use_waveout ? run_waveout(gen, seconds, frames, buffers)
	                           : run_wasapi(gen, seconds, latency_ms);

	if (wav && !rec.empty())
		write_wav(wav, rec);
	g_midi.close();

	const double audio = double(gen.produced) / RATE;
	const double busy  = double(gen.busy_ticks) / gen.freq.QuadPart;
	std::printf("終了。%.1f 秒ぶんを %.2f 秒で生成（CPU 使用率 %.1f%%）  MIDI %llu バイト\n",
	            audio, busy, audio > 0 ? 100.0 * busy / audio : 0.0,
	            (unsigned long long)g_midi.bytes());
	return rc;
}
