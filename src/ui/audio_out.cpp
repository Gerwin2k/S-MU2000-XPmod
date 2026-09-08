// license:BSD-3-Clause

#include "audio_out.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>

namespace ui {

bool audio_out::start(int latency_ms, fill_fn fill, std::string &err)
{
	if (m_thread.joinable())
		return true;

	LARGE_INTEGER f;
	QueryPerformanceFrequency(&f);
	m_qpc_freq = f.QuadPart;

	m_fill = std::move(fill);
	m_quit.store(false);
	m_err.clear();

	std::atomic<int> state{0};   // 0 待ち / 1 動いた / 2 だめ
	m_thread = std::thread([this, latency_ms, &state] {
		run(latency_ms);
		state.store(m_running.load() ? 1 : 2);
	});

	// 開始に失敗したかどうかだけ待つ。だめなら理由を返す
	for (int i = 0; i < 400 && state.load() == 0 && !m_running.load(); i++)
		Sleep(5);
	if (!m_running.load() && state.load() == 2) {
		m_thread.join();
		err = m_err.empty() ? "音声デバイスを開けない" : m_err;
		return false;
	}
	return true;
}

void audio_out::stop()
{
	m_quit.store(true);
	if (m_thread.joinable())
		m_thread.join();
	m_running.store(false);
}

double audio_out::cpu_percent() const
{
	const u64 done = m_produced.load();
	if (!done)
		return 0.0;
	const double audio = double(done) / AUDIO_RATE;
	const double busy  = double(m_busy_ticks.load()) / double(m_qpc_freq);
	return 100.0 * busy / audio;
}

double audio_out::worst_ms() const
{
	return 1000.0 * double(m_worst_ticks.load()) / double(m_qpc_freq);
}

void audio_out::run(int latency_ms)
{
	if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {
		m_err = "COM を初期化できない";
		return;
	}

	IMMDeviceEnumerator *en = nullptr;
	IMMDevice *dev = nullptr;
	IAudioClient *client = nullptr;
	IAudioRenderClient *render = nullptr;
	HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
	UINT32 buf_frames = 0;

	auto fail = [this](const char *what, HRESULT hr) {
		char buf[128];
		std::snprintf(buf, sizeof(buf), "%s に失敗 (0x%08lx)", what, (unsigned long)hr);
		m_err = buf;
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
		fmt.nSamplesPerSec  = AUDIO_RATE;
		fmt.wBitsPerSample  = 16;
		fmt.nBlockAlign     = 4;
		fmt.nAvgBytesPerSec = AUDIO_RATE * 4;

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

	m_buffer_frames.store(buf_frames);

	// 音声を作るスレッドは優先度を上げる。取りこぼすと音が切れる
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

	{
		// 最初に一杯まで埋めてから走らせる
		BYTE *data = nullptr;
		if (SUCCEEDED(render->GetBuffer(buf_frames, &data))) {
			m_fill(reinterpret_cast<s16 *>(data), buf_frames);
			m_produced.fetch_add(buf_frames);
			render->ReleaseBuffer(buf_frames, 0);
		}
	}

	hr = client->Start();
	if (FAILED(hr)) { fail("再生の開始", hr); goto done; }
	m_running.store(true);

	while (!m_quit.load()) {
		if (WaitForSingleObject(ev, 2000) != WAIT_OBJECT_0) {
			m_err = "音声デバイスからの合図が来ない";
			break;
		}

		UINT32 padding = 0;
		if (FAILED(client->GetCurrentPadding(&padding)))
			break;
		// 残量がゼロなら、デバイスは前の分を鳴らし終えて待たされた。
		// これが本当の音切れ。鳴らし始めの 1 杯目は数えない
		if (padding == 0 && m_produced.load() > buf_frames)
			m_starved.fetch_add(1);

		const UINT32 want = buf_frames - padding;
		if (!want)
			continue;

		BYTE *data = nullptr;
		if (FAILED(render->GetBuffer(want, &data)))
			break;

		LARGE_INTEGER t0, t1;
		QueryPerformanceCounter(&t0);
		m_fill(reinterpret_cast<s16 *>(data), want);
		QueryPerformanceCounter(&t1);

		const u64 took = u64(t1.QuadPart - t0.QuadPart);
		m_busy_ticks.fetch_add(took);
		if (took > m_worst_ticks.load())
			m_worst_ticks.store(took);
		m_produced.fetch_add(want);

		render->ReleaseBuffer(want, 0);
	}

	client->Stop();

done:
	m_running.store(false);
	if (render) render->Release();
	if (client) client->Release();
	if (dev) dev->Release();
	if (en) en->Release();
	if (ev) CloseHandle(ev);
	CoUninitialize();
}

} // namespace ui
