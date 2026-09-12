// license:BSD-3-Clause

#include "audio_out.h"
#include "resampler.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>

#include <algorithm>
#include <cstdio>
#include <vector>

namespace ui {

namespace {

// デバイスが言ってきた形式が float か（WAVE_FORMAT_EXTENSIBLE の下も見る）
bool is_float(const WAVEFORMATEX *f)
{
	if (f->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
		return true;
	if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
		const WAVEFORMATEXTENSIBLE *e = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(f);
		return e->SubFormat.Data1 == 3;   // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT
	}
	return false;
}

bool is_pcm16(const WAVEFORMATEX *f)
{
	if (f->wBitsPerSample != 16)
		return false;
	if (f->wFormatTag == WAVE_FORMAT_PCM)
		return true;
	if (f->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
		const WAVEFORMATEXTENSIBLE *e = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(f);
		return e->SubFormat.Data1 == 1;   // KSDATAFORMAT_SUBTYPE_PCM
	}
	return false;
}

} // namespace


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

	m_start_state.store(0);
	m_thread = std::thread([this, latency_ms] {
		run(latency_ms);
		m_start_state.store(m_running.load() ? 1 : 2);
	});

	// 開始に失敗したかどうかだけ待つ。だめなら理由を返す
	for (int i = 0; i < 400 && m_start_state.load() == 0 && !m_running.load(); i++)
		Sleep(5);
	if (!m_running.load() && m_start_state.load() == 2) {
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
	const double audio = double(done) / double(m_dev_rate.load());
	const double busy  = double(m_busy_ticks.load()) / double(m_qpc_freq);
	return 100.0 * busy / audio;
}

double audio_out::worst_ms() const
{
	return 1000.0 * double(m_worst_ticks.load()) / double(m_qpc_freq);
}

double audio_out::buffer_ms() const
{
	const u32 r = m_dev_rate.load();
	return r ? 1000.0 * double(m_buffer_frames.load()) / double(r) : 0.0;
}

std::string audio_out::format_line() const
{
	char buf[160];
	std::snprintf(buf, sizeof(buf),
	              "%u Hz %u ch %s / 溜め %.1f ms（%u フレーム）/ 周期 %.1f ms / 変換 %s",
	              m_dev_rate.load(), m_dev_channels.load(),
	              m_dev_float.load() ? "float" : "16bit",
	              buffer_ms(), m_buffer_frames.load(), m_period_ms.load(),
	              m_converting.load() ? "自前 sinc" : "無し");
	return buf;
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
	WAVEFORMATEX *mix = nullptr;
	UINT32 buf_frames = 0;
	DWORD  mmcss_index = 0;
	HANDLE mmcss = nullptr;

	// 変換と受け渡しの入れ物。音声スレッドだけが触る
	resampler rs;
	std::vector<s16>   stage;     // 音源から受ける 44100Hz 16bit 2ch
	std::vector<float> mixbuf;    // 変換した後の 2ch float
	bool dev_float = false;
	u32  dev_ch = 2, dev_rate = AUDIO_RATE;
	// 1 回に変換する上限。輪の大きさに収まる範囲で区切る
	const UINT32 CHUNK = 480;

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
		// デバイスが言ってくる形式で開く。
		//
		// **44100Hz 16bit を頼んで Windows に変換させてはいけない。**
		// 既定の品質の変換器を通され、溜めもその分増える。今どきの
		// デバイスは 48000Hz の float を言ってくるので、そのまま受けて
		// 44100 から 48000 への変換を自分でやる（ui::resampler）
		hr = client->GetMixFormat(&mix);
		if (FAILED(hr) || !mix) { fail("形式の取得", hr); goto done; }

		WAVEFORMATEX fallback{};
		const bool usable = is_float(mix) || is_pcm16(mix);
		if (!usable) {
			// 見たことのない形式。昔どおり 44100/16bit を頼んで
			// Windows に変換させる（音は出る、という側に倒す）
			fallback.wFormatTag      = WAVE_FORMAT_PCM;
			fallback.nChannels       = 2;
			fallback.nSamplesPerSec  = AUDIO_RATE;
			fallback.wBitsPerSample  = 16;
			fallback.nBlockAlign     = 4;
			fallback.nAvgBytesPerSec = AUDIO_RATE * 4;
		}
		const WAVEFORMATEX *use = usable ? mix : &fallback;
		dev_float = usable && is_float(mix);
		dev_ch    = use->nChannels;
		dev_rate  = use->nSamplesPerSec;

		REFERENCE_TIME def_period = 0, min_period = 0;
		if (SUCCEEDED(client->GetDevicePeriod(&def_period, &min_period)))
			m_period_ms.store(def_period / 10000.0);

		const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
		                    (usable ? 0u : (AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
		                                    AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY));
		const REFERENCE_TIME dur = REFERENCE_TIME(latency_ms) * 10000;
		hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, dur, 0, use, nullptr);
		if (FAILED(hr)) { fail("音声の開始準備", hr); goto done; }

		m_dev_rate.store(dev_rate);
		m_dev_channels.store(dev_ch);
		m_dev_float.store(dev_float);
		m_dev_bits.store(use->wBitsPerSample);

		rs.configure(double(AUDIO_RATE), double(dev_rate));
		m_converting.store(!rs.direct());
		stage.resize(size_t(CHUNK + 64) * 2);
		mixbuf.resize(size_t(CHUNK) * 2);
	}

	hr = client->SetEventHandle(ev);
	if (FAILED(hr)) { fail("イベントの登録", hr); goto done; }

	hr = client->GetBufferSize(&buf_frames);
	if (FAILED(hr)) { fail("バッファ長の取得", hr); goto done; }

	hr = client->GetService(__uuidof(IAudioRenderClient), (void **)&render);
	if (FAILED(hr)) { fail("書き込み口の取得", hr); goto done; }

	m_buffer_frames.store(buf_frames);

	// 音声を作るスレッドは優先度を上げる。取りこぼすと音が切れる。
	//
	// **SetThreadPriority だけでは足りない**。Windows の割り当ては
	// MMCSS（マルチメディア用の割り当て）が別に持っていて、"Pro Audio" で
	// 登録しておかないと、他の仕事のために数十ミリ秒まとめて止められる
	// ことがある。平均の負荷に余裕があっても、そこで音が途切れる
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
	mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &mmcss_index);
	if (mmcss)
		AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_CRITICAL);
	m_mmcss.store(mmcss != nullptr);

	{
		BYTE *data = nullptr;

		// デバイスへ frames ぶん書く。2ch を超える分は黙らせる
		auto write_frames = [&](BYTE *dst, UINT32 frames) {
			UINT32 at = 0;
			while (at < frames) {
				const UINT32 n = std::min<UINT32>(CHUNK, frames - at);
				const int need = rs.input_needed(int(n));
				if (need > 0) {
					if (stage.size() < size_t(need) * 2)
						stage.resize(size_t(need) * 2);
					m_fill(stage.data(), u32(need));
					rs.push(stage.data(), need);
				}
				rs.pull(mixbuf.data(), int(n));

				if (dev_float) {
					float *out = reinterpret_cast<float *>(dst) + size_t(at) * dev_ch;
					for (UINT32 i = 0; i < n; i++) {
						out[i * dev_ch + 0] = mixbuf[i * 2 + 0];
						if (dev_ch > 1)
							out[i * dev_ch + 1] = mixbuf[i * 2 + 1];
						for (u32 c = 2; c < dev_ch; c++)
							out[i * dev_ch + c] = 0.0f;
					}
				} else {
					s16 *out = reinterpret_cast<s16 *>(dst) + size_t(at) * dev_ch;
					for (UINT32 i = 0; i < n; i++) {
						for (u32 s = 0; s < 2 && s < dev_ch; s++) {
							const float v = std::clamp(mixbuf[i * 2 + s], -1.0f, 1.0f);
							out[i * dev_ch + s] = s16(v * 32767.0f);
						}
						for (u32 c = 2; c < dev_ch; c++)
							out[i * dev_ch + c] = 0;
					}
				}
				at += n;
			}
		};

		// 最初に一杯まで埋めてから走らせる
		if (SUCCEEDED(render->GetBuffer(buf_frames, &data))) {
			write_frames(data, buf_frames);
			m_produced.fetch_add(buf_frames);
			render->ReleaseBuffer(buf_frames, 0);
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

			if (FAILED(render->GetBuffer(want, &data)))
				break;

			LARGE_INTEGER t0, t1;
			QueryPerformanceCounter(&t0);
			write_frames(data, want);
			QueryPerformanceCounter(&t1);

			const u64 took = u64(t1.QuadPart - t0.QuadPart);
			m_busy_ticks.fetch_add(took);
			if (took > m_worst_ticks.load())
				m_worst_ticks.store(took);
			m_produced.fetch_add(want);

			render->ReleaseBuffer(want, 0);
		}

		client->Stop();
	}

done:
	if (mmcss)
		AvRevertMmThreadCharacteristics(mmcss);
	m_running.store(false);
	if (mix) CoTaskMemFree(mix);
	if (render) render->Release();
	if (client) client->Release();
	if (dev) dev->Release();
	if (en) en->Release();
	if (ev) CloseHandle(ev);
	CoUninitialize();
}

} // namespace ui
