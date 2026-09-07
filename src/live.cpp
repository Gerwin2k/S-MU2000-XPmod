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
		            audio, (unsigned long long)g_midi_bytes.load(), 100.0 * busy / audio);
		std::printf("     枯渇 %llu 回（残量ゼロ）、生成の最悪 %.1f ms（溜めは %.1f ms ぶん）\n",
		            (unsigned long long)starved, 1000.0 * worst_ticks / freq.QuadPart,
		            1000.0 * cushion_frames / RATE);
	}
};

// ---- WASAPI 共有モード。イベント駆動

int run_wasapi(generator &gen, double seconds, int latency_ms)
{
	if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
		std::fprintf(stderr, "COM を初期化できない\n");
		return 1;
	}

	IMMDeviceEnumerator *en = nullptr;
	IMMDevice *dev = nullptr;
	IAudioClient *client = nullptr;
	IAudioRenderClient *render = nullptr;
	HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
	UINT32 buf_frames = 0;
	int rc = 1;

	auto fail = [](const char *what, HRESULT hr) {
		std::fprintf(stderr, "%s に失敗 (0x%08lx)\n", what, (unsigned long)hr);
	};

	HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
	                              __uuidof(IMMDeviceEnumerator), (void **)&en);
	if (FAILED(hr)) { fail("デバイス一覧の取得", hr); goto done; }

	hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
	if (FAILED(hr)) { fail("既定の音声デバイスの取得", hr); goto done; }

	hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&client);
	if (FAILED(hr)) { fail("音声デバイスの起動", hr); goto done; }

	{
		// こちらは 44100Hz 16bit ステレオで作る。デバイスの形式が違っても
		// AUTOCONVERTPCM を付けておけば Windows が変換してくれる
		WAVEFORMATEX fmt{};
		fmt.wFormatTag      = WAVE_FORMAT_PCM;
		fmt.nChannels       = 2;
		fmt.nSamplesPerSec  = RATE;
		fmt.wBitsPerSample  = 16;
		fmt.nBlockAlign     = 4;
		fmt.nAvgBytesPerSec = RATE * 4;

		const REFERENCE_TIME dur = REFERENCE_TIME(latency_ms) * 10000;
		hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
		                        AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
		                        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
		                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
		                        dur, 0, &fmt, nullptr);
		if (FAILED(hr)) { fail("音声の開始準備", hr); goto done; }
	}

	hr = client->SetEventHandle(ev);
	if (FAILED(hr)) { fail("イベントの登録", hr); goto done; }

	hr = client->GetBufferSize(&buf_frames);
	if (FAILED(hr)) { fail("バッファ長の取得", hr); goto done; }

	hr = client->GetService(__uuidof(IAudioRenderClient), (void **)&render);
	if (FAILED(hr)) { fail("書き込み口の取得", hr); goto done; }

	std::printf("WASAPI 共有モード  待ち時間 %.1f ms（%u サンプル）\n",
	            1000.0 * buf_frames / RATE, buf_frames);
	if (seconds > 0.0)
		std::printf("%.1f 秒で終了\n", seconds);
	else
		std::printf("Ctrl+C で終了\n");

	// 音声を作るスレッドは優先度を上げる。取りこぼすと音が切れる
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
	gen.cushion_frames = buf_frames;

	{
		// 最初に一杯まで埋めてから走らせる
		BYTE *data = nullptr;
		if (SUCCEEDED(render->GetBuffer(buf_frames, &data))) {
			gen.fill(reinterpret_cast<s16 *>(data), buf_frames);
			render->ReleaseBuffer(buf_frames, 0);
		}
	}

	hr = client->Start();
	if (FAILED(hr)) { fail("再生の開始", hr); goto done; }

	while (seconds <= 0.0 || gen.produced < u64(seconds * RATE)) {
		if (WaitForSingleObject(ev, 2000) != WAIT_OBJECT_0) {
			std::fprintf(stderr, "音声デバイスからの合図が来ない\n");
			break;
		}

		UINT32 padding = 0;
		if (FAILED(client->GetCurrentPadding(&padding)))
			break;
		// 残量がゼロなら、デバイスは前の分を鳴らし終えて待たされた。
		// これが本当の音切れ。生成に何 ms かかったかより、こちらが答え
		if (padding == 0)
			gen.starved++;
		const UINT32 want = buf_frames - padding;
		if (!want)
			continue;

		BYTE *data = nullptr;
		if (FAILED(render->GetBuffer(want, &data)))
			break;
		gen.fill(reinterpret_cast<s16 *>(data), want);
		render->ReleaseBuffer(want, 0);

		if (gen.produced % (RATE * 5) < want)
			gen.report(buf_frames);
	}

	client->Stop();
	rc = 0;

done:
	if (render) render->Release();
	if (client) client->Release();
	if (dev) dev->Release();
	if (en) en->Release();
	if (ev) CloseHandle(ev);
	CoUninitialize();
	return rc;
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

	std::vector<s16> rec;
	generator gen(mu, wav ? &rec : nullptr);

	const int rc = use_waveout ? run_waveout(gen, seconds, frames, buffers)
	                           : run_wasapi(gen, seconds, latency_ms);

	if (wav && !rec.empty())
		write_wav(wav, rec);
	if (hmi) { midiInStop(hmi); midiInClose(hmi); }

	const double audio = double(gen.produced) / RATE;
	const double busy  = double(gen.busy_ticks) / gen.freq.QuadPart;
	std::printf("終了。%.1f 秒ぶんを %.2f 秒で生成（CPU 使用率 %.1f%%）  MIDI %llu バイト\n",
	            audio, busy, audio > 0 ? 100.0 * busy / audio : 0.0,
	            (unsigned long long)g_midi_bytes.load());
	return rc;
}
