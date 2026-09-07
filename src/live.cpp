// Windows の MIDI 入力を受けて、そのまま音を鳴らす。
//
//   live --list                          MIDI 入力の一覧
//   live <rom ディレクトリ> [--midi 番号] [--frames 数] [--buffers 数]
//
// 設計の要点は doc/design.md にあるとおりで、**時計を自分で持たない**こと。
// 音声デバイスが「N サンプルくれ」と要求した分だけ CPU と音源を進める。
// こうするとホストとずれようがない。MAME が外部同期で破綻したのはここの違い。
//
// 音声は WinMM の waveOut を使っている。素直で壊れにくいが待ち時間はやや長い。
// もっと詰めるなら WASAPI の共有モードに差し替える（そのとき触るのは
// このファイルだけで済むようにしてある）。

#include "mu2000.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <windows.h>
#include <mmsystem.h>

namespace {

constexpr u32 RATE = 44100;

// ---- MIDI 入力から音声スレッドへバイトを渡す輪。
// 書くのは MIDI のコールバック、読むのは音声スレッドの一本ずつなので、
// 添字を atomic にしておけば錠は要らない。音声スレッドで錠を待つのは禁物。
class byte_ring
{
public:
	void push(u8 v)
	{
		const size_t w = m_write.load(std::memory_order_relaxed);
		const size_t next = (w + 1) & MASK;
		if (next == m_read.load(std::memory_order_acquire))
			return;                       // 溢れ。実機の受信バッファ溢れと同じ
		m_buf[w] = v;
		m_write.store(next, std::memory_order_release);
	}

	bool pop(u8 &v)
	{
		const size_t r = m_read.load(std::memory_order_relaxed);
		if (r == m_write.load(std::memory_order_acquire))
			return false;
		v = m_buf[r];
		m_read.store((r + 1) & MASK, std::memory_order_release);
		return true;
	}

private:
	static constexpr size_t SIZE = 4096, MASK = SIZE - 1;
	u8 m_buf[SIZE] = {};
	std::atomic<size_t> m_read{0}, m_write{0};
};

byte_ring g_midi;
std::atomic<u64> g_midi_bytes{0};

void CALLBACK midi_cb(HMIDIIN, UINT msg, DWORD_PTR, DWORD_PTR p1, DWORD_PTR)
{
	if (msg == MIM_DATA) {
		const u8 status = u8(p1);
		if (status < 0x80)
			return;
		// 長さは種別で決まる。プログラムチェンジとチャンネルプレッシャだけ 1 バイト
		int n = 3;
		const u8 kind = status & 0xf0;
		if (kind == 0xc0 || kind == 0xd0) n = 2;
		if (status >= 0xf8) n = 1;                       // リアルタイム
		else if (status == 0xf1 || status == 0xf3) n = 2;
		else if (status == 0xf2) n = 3;
		else if (status >= 0xf4 && status < 0xf8) n = 1;

		g_midi.push(status);
		if (n > 1) g_midi.push(u8(p1 >> 8));
		if (n > 2) g_midi.push(u8(p1 >> 16));
		g_midi_bytes += n;
	} else if (msg == MIM_LONGDATA) {
		MIDIHDR *h = reinterpret_cast<MIDIHDR *>(p1);
		for (DWORD i = 0; i < h->dwBytesRecorded; i++)
			g_midi.push(u8(h->lpData[i]));
		g_midi_bytes += h->dwBytesRecorded;
	}
}

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

} // namespace


int main(int argc, char **argv)
{
	SetConsoleOutputCP(CP_UTF8);   // 既定の CP932 だと表示が化ける

	int  midi_dev = -1;
	// WinMM の waveOut は内部の周期がおよそ 10ms あり、それより短いバッファは
	// 1 枚ずつ捌けない。256 サンプル(5.8ms)にすると 1 周期に 1 枚しか進まず、
	// 再生が半分の速さになって細切れに聞こえる。既定は余裕をみて 512
	int  frames = 512;      // 1 回に作るサンプル数（11.6ms）
	int  buffers = 3;       // 用意する枚数
	double seconds = 0.0;   // 0 なら Ctrl+C まで
	bool nomidi = false;
	const char *wav = nullptr;   // 鳴らしたものを録っておく（確認用）
	std::string dir;

	for (int i = 1; i < argc; i++) {
		if (!std::strcmp(argv[i], "--list")) { list_midi_inputs(); return 0; }
		else if (!std::strcmp(argv[i], "--midi") && i + 1 < argc) midi_dev = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--frames") && i + 1 < argc) frames = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--buffers") && i + 1 < argc) buffers = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = std::atof(argv[++i]);
		else if (!std::strcmp(argv[i], "--nomidi")) nomidi = true;
		else if (!std::strcmp(argv[i], "--wav") && i + 1 < argc) wav = argv[++i];
		else if (!std::strcmp(argv[i], "-v")) smu2000::g_verbose = true;
		else if (dir.empty()) dir = argv[i];
	}
	if (dir.empty()) {
		std::fprintf(stderr,
			"使い方: live <rom ディレクトリ> [--midi 番号] [--frames 数] [--buffers 数]\n"
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
	HMIDIIN hmi = nullptr;
	if (nomidi) midi_dev = -1;
	else if (midi_dev < 0 && midiInGetNumDevs() > 0)
		midi_dev = 0;
	if (midi_dev >= 0) {
		if (midiInOpen(&hmi, UINT(midi_dev), DWORD_PTR(midi_cb), 0,
		               CALLBACK_FUNCTION) != MMSYSERR_NOERROR) {
			std::fprintf(stderr, "MIDI 入力 %d を開けない\n", midi_dev);
			return 1;
		}
		MIDIINCAPSA caps{};
		midiInGetDevCapsA(UINT(midi_dev), &caps, sizeof(caps));
		std::printf("MIDI 入力: %d: %s\n", midi_dev, caps.szPname);
		midiInStart(hmi);
	} else
		std::printf("MIDI 入力なし（音は出るが何も鳴らない）\n");

	// ---- 音声出力
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
	// 空きの判定は WHDR_INQUEUE が立っていないこと。waveOutWrite が立て、
	// 再生し終わるとドライバが下ろす。WHDR_DONE を自分で立てて見張ると、
	// 完了の取りこぼしで 1 バッファぶん余計に待ち、再生が半分の速さになる

	std::printf("待ち時間の目安 %.1f ms（%d サンプル × %d 枚）\n",
	            1000.0 * frames * buffers / RATE, frames, buffers);
	if (seconds > 0.0)
		std::printf("%.1f 秒で終了\n", seconds);
	else
		std::printf("Ctrl+C で終了\n");

	std::vector<s16> rec;
	// 音声を作るスレッドは優先度を上げる。取りこぼすと音が切れる
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

	u64 produced = 0;
	u64 write_errors = 0;
	// 1 枚を作るのにかかった時間。再生時間を超えた回数が間に合っていない回数
	u64 late = 0, worst_ticks = 0;
	// 生成にかけた時間を測る。実時間に対する割合が余力の目安になる
	LARGE_INTEGER freq, t0, t1;
	QueryPerformanceFrequency(&freq);
	u64 busy_ticks = 0;
	for (;;) {
		// 空いている枚を探す。無ければドライバの合図を待つ
		int next = -1;
		while (next < 0) {
			for (int i = 0; i < buffers; i++)
				if (!(hdr[i].dwFlags & WHDR_INQUEUE)) { next = i; break; }
			if (next < 0)
				WaitForSingleObject(done, 50);
		}

		// 溜まっている MIDI を音源へ。実機と同じく 31250bps の直列で流れる
		u8 b;
		while (g_midi.pop(b))
			mu.midi_in(b);

		QueryPerformanceCounter(&t0);
		s16 *out = pcm[next].data();
		for (int i = 0; i < frames; i++) {
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
		if (double(one) / freq.QuadPart > double(frames) / RATE) late++;

		if (wav)
			rec.insert(rec.end(), out, out + size_t(frames) * 2);

		if (waveOutWrite(hwo, &hdr[next], sizeof(WAVEHDR)) != MMSYSERR_NOERROR)
			write_errors++;

		produced += frames;
		if (seconds > 0.0 && produced >= u64(seconds * RATE))
			break;
		if (produced % (RATE * 5) < u64(frames)) {
			const double audio = double(produced) / RATE;
			const double busy  = double(busy_ticks) / freq.QuadPart;
			std::printf("  %.0f 秒経過  MIDI %llu バイト  CPU 使用率 %.1f%%%s\n",
			            audio, (unsigned long long)g_midi_bytes.load(), 100.0 * busy / audio,
			            write_errors ? "  ※書き込み失敗あり" : "");
			std::printf("     間に合わなかった枚 %llu、最悪 %.1f ms（1 枚は %.1f ms）\n",
			            (unsigned long long)late, 1000.0 * worst_ticks / freq.QuadPart,
			            1000.0 * frames / RATE);
		}
	}

	// 片付け。鳴らしかけの分を止めてから閉じる
	if (wav && !rec.empty()) {
		// 確認用の WAV。render の出力と突き合わせられる
		std::FILE *f = std::fopen(wav, "wb");
		if (f) {
			const u32 bytes = u32(rec.size() * 2);
			auto w32 = [&](u32 v) { u8 b[4] = { u8(v), u8(v >> 8), u8(v >> 16), u8(v >> 24) };
			                        std::fwrite(b, 1, 4, f); };
			auto w16 = [&](u16 v) { u8 b[2] = { u8(v), u8(v >> 8) }; std::fwrite(b, 1, 2, f); };
			std::fwrite("RIFF", 1, 4, f); w32(36 + bytes); std::fwrite("WAVE", 1, 4, f);
			std::fwrite("fmt ", 1, 4, f); w32(16); w16(1); w16(2);
			w32(RATE); w32(RATE * 4); w16(4); w16(16);
			std::fwrite("data", 1, 4, f); w32(bytes);
			std::fwrite(rec.data(), 1, bytes, f);
			std::fclose(f);
			std::printf("録音を書き出した: %s\n", wav);
		}
	}

	waveOutReset(hwo);
	for (int i = 0; i < buffers; i++)
		waveOutUnprepareHeader(hwo, &hdr[i], sizeof(WAVEHDR));
	waveOutClose(hwo);
	if (hmi) { midiInStop(hmi); midiInClose(hmi); }
	CloseHandle(done);
	{
		const double audio = double(produced) / RATE;
		const double busy  = double(busy_ticks) / freq.QuadPart;
		std::printf("終了。%.1f 秒ぶんを %.2f 秒で生成（CPU 使用率 %.1f%%）  MIDI %llu バイト\n",
		            audio, busy, 100.0 * busy / audio, (unsigned long long)g_midi_bytes.load());
	}
	return 0;
}
