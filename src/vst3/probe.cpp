// license:BSD-3-Clause
//
// VST3 プラグインを DAW 無しで動かしてみる小さなホスト。
//
//   vst3probe <S-MU2000.vst3 の DLL>                         名乗りだけ見る
//   vst3probe <DLL> <MIDI ファイル> <出力 wav> [--rate 48000] [--block 512]
//
// DAW に入れる前にここで確かめる。工場が名乗るか、インターフェースが揃うか、
// MIDI を受けて音が出るか、標本化周波数の変換が効いているか。

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

#include "smf.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <windows.h>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

void print16(const char16 *s)
{
	for (int i = 0; i < 64 && s[i]; i++)
		std::putchar(s[i] < 128 ? char(s[i]) : '?');
}

// ---- ホスト側の入れ物。プラグインに渡すためだけの最小限

class param_queue : public IParamValueQueue
{
public:
	param_queue(ParamID id) : m_id(id) {}
	tresult PLUGIN_API queryInterface(const TUID, void **obj) override
	{ *obj = this; return kResultOk; }
	uint32 PLUGIN_API addRef() override  { return 1; }
	uint32 PLUGIN_API release() override { return 1; }

	ParamID PLUGIN_API getParameterId() override { return m_id; }
	int32 PLUGIN_API getPointCount() override { return int32(m_pts.size()); }
	tresult PLUGIN_API getPoint(int32 i, int32 &off, ParamValue &v) override
	{
		if (i < 0 || i >= int32(m_pts.size())) return kResultFalse;
		off = m_pts[i].first; v = m_pts[i].second; return kResultOk;
	}
	tresult PLUGIN_API addPoint(int32 off, ParamValue v, int32 &idx) override
	{ idx = int32(m_pts.size()); m_pts.push_back({ off, v }); return kResultOk; }

	void add(int32 off, ParamValue v) { m_pts.push_back({ off, v }); }
	void clear() { m_pts.clear(); }
	bool empty() const { return m_pts.empty(); }

private:
	ParamID m_id;
	std::vector<std::pair<int32, ParamValue>> m_pts;
};

class param_changes : public IParameterChanges
{
public:
	tresult PLUGIN_API queryInterface(const TUID, void **obj) override
	{ *obj = this; return kResultOk; }
	uint32 PLUGIN_API addRef() override  { return 1; }
	uint32 PLUGIN_API release() override { return 1; }

	int32 PLUGIN_API getParameterCount() override { return int32(m_live.size()); }
	IParamValueQueue *PLUGIN_API getParameterData(int32 i) override
	{ return (i >= 0 && i < int32(m_live.size())) ? m_live[i] : nullptr; }
	IParamValueQueue *PLUGIN_API addParameterData(const ParamID &id, int32 &idx) override
	{ idx = 0; return get(id); }

	param_queue *get(ParamID id)
	{
		auto it = m_all.find(id);
		if (it == m_all.end())
			it = m_all.emplace(id, new param_queue(id)).first;
		param_queue *q = it->second;
		if (q->empty())
			m_live.push_back(q);
		return q;
	}
	void clear()
	{
		for (param_queue *q : m_live) q->clear();
		m_live.clear();
	}

private:
	std::map<ParamID, param_queue *> m_all;
	std::vector<param_queue *> m_live;
};

class event_list : public IEventList
{
public:
	tresult PLUGIN_API queryInterface(const TUID, void **obj) override
	{ *obj = this; return kResultOk; }
	uint32 PLUGIN_API addRef() override  { return 1; }
	uint32 PLUGIN_API release() override { return 1; }

	int32 PLUGIN_API getEventCount() override { return int32(m_ev.size()); }
	tresult PLUGIN_API getEvent(int32 i, Event &e) override
	{
		if (i < 0 || i >= int32(m_ev.size())) return kResultFalse;
		e = m_ev[i]; return kResultOk;
	}
	tresult PLUGIN_API addEvent(Event &e) override { m_ev.push_back(e); return kResultOk; }

	void clear() { m_ev.clear(); m_sysex.clear(); }
	std::vector<Event> m_ev;
	std::vector<std::vector<uint8>> m_sysex;   // 領域の持ち主
};

void write_wav(const std::string &path, const std::vector<int16_t> &pcm, uint32_t rate)
{
	std::FILE *f = std::fopen(path.c_str(), "wb");
	if (!f) return;
	const uint32_t bytes = uint32_t(pcm.size() * 2);
	auto u32w = [&](uint32_t v) { uint8_t b[4] = { uint8_t(v), uint8_t(v >> 8),
	                                               uint8_t(v >> 16), uint8_t(v >> 24) };
	                              std::fwrite(b, 1, 4, f); };
	auto u16w = [&](uint16_t v) { uint8_t b[2] = { uint8_t(v), uint8_t(v >> 8) };
	                              std::fwrite(b, 1, 2, f); };
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
	SetConsoleOutputCP(CP_UTF8);

	if (argc < 2) {
		std::fprintf(stderr,
			"使い方: vst3probe <DLL> [<MIDI> <出力 wav>] [--rate 48000] [--block 512]\n");
		return 1;
	}
	std::string dll = argv[1], mid, wav;
	double rate = 48000.0;
	int block = 512;
	double extra = 3.0;      // 曲の後ろに足す残響ぶん
	for (int i = 2; i < argc; i++) {
		if (!std::strcmp(argv[i], "--rate") && i + 1 < argc) rate = std::atof(argv[++i]);
		else if (!std::strcmp(argv[i], "--block") && i + 1 < argc) block = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--tail") && i + 1 < argc) extra = std::atof(argv[++i]);
		else if (mid.empty()) mid = argv[i];
		else if (wav.empty()) wav = argv[i];
	}

	HMODULE lib = LoadLibraryA(dll.c_str());
	if (!lib) {
		std::fprintf(stderr, "DLL を読めない: %s (エラー %lu)\n", dll.c_str(), GetLastError());
		return 1;
	}
	auto init = reinterpret_cast<bool (*)()>(GetProcAddress(lib, "InitDll"));
	auto getf = reinterpret_cast<IPluginFactory *(PLUGIN_API *)()>(
		GetProcAddress(lib, "GetPluginFactory"));
	if (!getf) {
		std::fprintf(stderr, "GetPluginFactory が無い\n");
		return 1;
	}
	if (init) init();

	IPluginFactory *fac = getf();
	if (!fac) { std::fprintf(stderr, "工場が空\n"); return 1; }

	PFactoryInfo fi{};
	fac->getFactoryInfo(&fi);
	std::printf("製作: %s  %s\n", fi.vendor, fi.url);
	std::printf("クラス数: %d\n", fac->countClasses());

	TUID cid{};
	for (int32 i = 0; i < fac->countClasses(); i++) {
		PClassInfo ci{};
		if (fac->getClassInfo(i, &ci) != kResultOk) continue;
		std::printf("  [%d] %s  種別 %s\n", i, ci.name, ci.category);
		if (!std::strcmp(ci.category, kVstAudioEffectClass))
			std::memcpy(cid, ci.cid, sizeof(TUID));
	}
	{
		IPluginFactory2 *f2 = nullptr;
		if (fac->queryInterface(IPluginFactory2::iid, (void **)&f2) == kResultOk && f2) {
			PClassInfo2 c2{};
			if (f2->getClassInfo2(0, &c2) == kResultOk)
				std::printf("  分類 %s  版 %s  SDK %s\n", c2.subCategories, c2.version,
				            c2.sdkVersion);
			f2->release();
		}
		IPluginFactory3 *f3 = nullptr;
		if (fac->queryInterface(IPluginFactory3::iid, (void **)&f3) == kResultOk && f3) {
			PClassInfoW cw{};
			if (f3->getClassInfoUnicode(0, &cw) == kResultOk) {
				std::printf("  Unicode 名 ");
				print16(cw.name);
				std::printf("\n");
			}
			f3->release();
		}
	}

	IComponent *comp = nullptr;
	if (fac->createInstance(reinterpret_cast<FIDString>(cid),
	                        reinterpret_cast<FIDString>(IComponent::iid.toTUID()),
	                        (void **)&comp) != kResultOk || !comp) {
		std::fprintf(stderr, "IComponent を作れない\n");
		return 1;
	}
	std::printf("IComponent を作れた\n");

	IAudioProcessor *proc = nullptr;
	IEditController *ctrl = nullptr;
	IMidiMapping    *map  = nullptr;
	comp->queryInterface(IAudioProcessor::iid, (void **)&proc);
	comp->queryInterface(IEditController::iid, (void **)&ctrl);
	comp->queryInterface(IMidiMapping::iid, (void **)&map);
	std::printf("IAudioProcessor %s / IEditController %s / IMidiMapping %s\n",
	            proc ? "あり" : "なし", ctrl ? "あり" : "なし", map ? "あり" : "なし");
	if (!proc) return 1;

	comp->initialize(nullptr);
	std::printf("音声出力バス %d / MIDI 入力バス %d / パラメータ %d\n",
	            comp->getBusCount(kAudio, kOutput), comp->getBusCount(kEvent, kInput),
	            ctrl ? ctrl->getParameterCount() : 0);

	if (map) {
		ParamID id = 0;
		if (map->getMidiControllerAssignment(0, 0, 7, id) == kResultTrue)
			std::printf("Ch1 CC7 -> パラメータ %u\n", unsigned(id));
	}

	SpeakerArrangement out_arr = SpeakerArr::kStereo;
	proc->setBusArrangements(nullptr, 0, &out_arr, 1);
	comp->activateBus(kAudio, kOutput, 0, true);
	comp->activateBus(kEvent, kInput, 0, true);

	ProcessSetup setup{};
	setup.processMode        = kOffline;
	setup.symbolicSampleSize = kSample32;
	setup.maxSamplesPerBlock  = block;
	setup.sampleRate         = rate;
	if (proc->setupProcessing(setup) != kResultOk) {
		std::fprintf(stderr, "setupProcessing に失敗\n");
		return 1;
	}
	comp->setActive(true);
	proc->setProcessing(true);
	std::printf("遅れ %u サンプル / 残響 %u サンプル\n",
	            proc->getLatencySamples(), proc->getTailSamples());

	if (mid.empty() || wav.empty()) {
		std::printf("音は出していない（MIDI と出力先を渡すと鳴らす）\n");
		proc->setProcessing(false);
		comp->setActive(false);
		comp->terminate();
		comp->release();
		return 0;
	}

	std::vector<smf::event> events;
	std::string err;
	if (!smf::load(mid, events, err)) {
		std::fprintf(stderr, "%s\n", err.c_str());
		return 1;
	}
	const double length = events.empty() ? 0.0 : events.back().time;
	std::printf("MIDI: %zu イベント、%.2f 秒\n", events.size(), length);

	// 起動が終わるまで無音を回す。プラグインは別スレッドで立ち上がっている
	std::vector<float> bl(block), br(block);
	float *chans[2] = { bl.data(), br.data() };
	AudioBusBuffers abuf{};
	abuf.numChannels = 2;
	abuf.silenceFlags = 0;
	abuf.channelBuffers32 = chans;

	event_list   elist;
	param_changes pchanges;

	ProcessData pd{};
	pd.processMode        = kOffline;
	pd.symbolicSampleSize = kSample32;
	pd.numSamples         = block;
	pd.numInputs          = 0;
	pd.numOutputs         = 1;
	pd.outputs            = &abuf;
	pd.inputEvents        = &elist;
	pd.inputParameterChanges = &pchanges;

	std::printf("起動待ち...");
	std::fflush(stdout);
	const DWORD t_wait = GetTickCount();
	for (;;) {
		Sleep(50);
		if (GetTickCount() - t_wait > 60000) {
			std::printf(" 60 秒待っても始まらない\n");
			break;
		}
		// パラメータの読み書きでは分からないので、鳴らして確かめる代わりに
		// 一定時間待つ。起動は実測 2 秒前後
		if (GetTickCount() - t_wait > 8000)
			break;
	}
	std::printf(" %lu ms\n", GetTickCount() - t_wait);

	const int64_t total = int64_t((length + extra) * rate);
	std::vector<int16_t> pcm;
	pcm.reserve(size_t(total) * 2);

	size_t next = 0;
	int64_t pos = 0;
	const DWORD t0 = GetTickCount();
	while (pos < total) {
		const int32 n = int32(std::min<int64_t>(block, total - pos));
		elist.clear();
		pchanges.clear();

		while (next < events.size() && events[next].time * rate < double(pos + n)) {
			const auto &e = events[next++];
			const int32 off = std::clamp(int32(e.time * rate - double(pos)), 0, n - 1);
			const std::vector<u8> &b = e.bytes;
			if (b.empty())
				continue;
			const uint8 st = b[0], ch = uint8(st & 15);
			if (st == 0xf0) {
				elist.m_sysex.push_back(std::vector<uint8>(b.begin(), b.end()));
				Event ev{};
				ev.busIndex = 0;
				ev.sampleOffset = off;
				ev.flags = Event::kIsLive;
				ev.type = Event::kDataEvent;
				ev.data.size = uint32(elist.m_sysex.back().size());
				ev.data.type = DataEvent::kMidiSysEx;
				ev.data.bytes = elist.m_sysex.back().data();
				elist.addEvent(ev);
			} else if ((st & 0xf0) == 0x90 && b.size() > 2 && b[2]) {
				Event ev{};
				ev.busIndex = 0; ev.sampleOffset = off; ev.flags = Event::kIsLive;
				ev.type = Event::kNoteOnEvent;
				ev.noteOn.channel = ch;
				ev.noteOn.pitch = int16(b[1]);
				ev.noteOn.velocity = float(b[2]) / 127.0f;
				ev.noteOn.noteId = -1;
				elist.addEvent(ev);
			} else if ((st & 0xf0) == 0x80 || ((st & 0xf0) == 0x90 && b.size() > 2)) {
				Event ev{};
				ev.busIndex = 0; ev.sampleOffset = off; ev.flags = Event::kIsLive;
				ev.type = Event::kNoteOffEvent;
				ev.noteOff.channel = ch;
				ev.noteOff.pitch = int16(b[1]);
				ev.noteOff.velocity = b.size() > 2 ? float(b[2]) / 127.0f : 0.0f;
				ev.noteOff.noteId = -1;
				elist.addEvent(ev);
			} else if ((st & 0xf0) == 0xa0 && b.size() > 2) {
				Event ev{};
				ev.busIndex = 0; ev.sampleOffset = off; ev.flags = Event::kIsLive;
				ev.type = Event::kPolyPressureEvent;
				ev.polyPressure.channel = ch;
				ev.polyPressure.pitch = int16(b[1]);
				ev.polyPressure.pressure = float(b[2]) / 127.0f;
				ev.polyPressure.noteId = -1;
				elist.addEvent(ev);
			} else if ((st & 0xf0) == 0xb0 && b.size() > 2) {
				pchanges.get(ParamID(ch * 131 + b[1]))->add(off, double(b[2]) / 127.0);
			} else if ((st & 0xf0) == 0xd0) {
				pchanges.get(ParamID(ch * 131 + 128))->add(off, double(b[1]) / 127.0);
			} else if ((st & 0xf0) == 0xe0 && b.size() > 2) {
				const int bend = b[1] | (int(b[2]) << 7);
				pchanges.get(ParamID(ch * 131 + 129))->add(off, double(bend) / 16383.0);
			} else if ((st & 0xf0) == 0xc0) {
				pchanges.get(ParamID(ch * 131 + 130))->add(off, double(b[1]) / 127.0);
			}
		}

		pd.numSamples = n;
		proc->process(pd);

		for (int32 i = 0; i < n; i++) {
			const float l = std::clamp(bl[i], -1.0f, 1.0f);
			const float r = std::clamp(br[i], -1.0f, 1.0f);
			pcm.push_back(int16_t(std::lround(l * 32767.0f)));
			pcm.push_back(int16_t(std::lround(r * 32767.0f)));
		}
		pos += n;
	}
	const DWORD t1 = GetTickCount();

	write_wav(wav, pcm, uint32_t(rate));
	double peak = 0.0, sum = 0.0;
	for (int16_t v : pcm) { peak = std::max(peak, std::fabs(double(v))); sum += std::fabs(double(v)); }
	std::printf("書き出した: %s（%.1f 秒 / %.0f Hz、実時間 %.1f 秒）\n",
	            wav.c_str(), double(total) / rate, rate, (t1 - t0) / 1000.0);
	std::printf("最大 %.0f  平均 %.1f\n", peak, sum / std::max<size_t>(pcm.size(), 1));

	proc->setProcessing(false);
	comp->setActive(false);
	comp->terminate();
	if (proc) proc->release();
	if (ctrl) ctrl->release();
	if (map)  map->release();
	comp->release();
	return peak > 0.0 ? 0 : 2;
}
