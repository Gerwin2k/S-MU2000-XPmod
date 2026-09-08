// license:BSD-3-Clause
//
// S-MU2000 の VST3 プラグイン。
//
// Steinberg の SDK のうち **インターフェース定義（pluginterfaces, MIT）だけ**
// を使い、土台（public.sdk, GPLv3）は使っていない。だから配線は全部ここにある。
// third_party/vst3/README.md に経緯がある。
//
// 作りは単一コンポーネント（single component effect）。IComponent と
// IEditController を 1 つのクラスが両方受け持つ。画面は持たない。
//
// MIDI の入り方は 2 通りある。VST3 はここが独特で、
//   ・ノートオン／オフ、ポリプレッシャ、システムエクスクルーシブ … イベント
//   ・コントロールチェンジ、ピッチベンド、プログラムチェンジ  … パラメータ
// になる。後者のために IMidiMapping で「チャンネル×番号 → パラメータ番号」を
// 教えてやる必要がある。受け取った側でまた MIDI のバイト列に組み直して音源へ渡す。

#include "engine.h"

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstmidicontrollers.h"
#include "pluginterfaces/vst/ivstparameterchanges.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include <windows.h>

using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

// ---- このプラグインを表す番号。一度決めたら変えられない
//      （変えるとホストが別物とみなし、保存した曲から見つからなくなる）
static const FUID kProcessorUID(0x5D2E4B70, 0xA1C34F92, 0x8B0E7A61, 0x4D553000);

constexpr const char *kPlugName   = "S-MU2000";
constexpr const char *kVendor     = "tarboh";
constexpr const char *kVersion    = "0.1.0.0";

// MIDI のコントロール番号は 0-127 のほか、
//   128 チャンネルプレッシャ / 129 ピッチベンド / 130 プログラムチェンジ
// まである。チャンネルごとにこれだけのパラメータを並べる
constexpr int32 kCtrlCount = 131;
constexpr int32 kChannels  = 16;
constexpr int32 kParamCount = kChannels * kCtrlCount;

ParamID param_of(int32 ch, int32 ctrl) { return ParamID(ch * kCtrlCount + ctrl); }

// パラメータの初期値。ホストが起動時にこれを送ってくることがあるので、
// 「初期値と同じ値が来たら何も送らない」ようにするための表でもある
double default_of(int32 ctrl)
{
	switch (ctrl) {
	case 7:   return 100.0 / 127.0;   // ボリューム
	case 10:  return  64.0 / 127.0;   // パン（中央）
	case 11:  return 1.0;             // エクスプレッション
	case 129: return 0.5;             // ピッチベンド（中央）
	default:  return 0.0;
	}
}

void set_str(String128 dst, const char *ascii)
{
	int i = 0;
	for (; ascii[i] && i < 127; i++)
		dst[i] = TChar(uint8(ascii[i]));
	dst[i] = 0;
}


// ---- 本体

class mu_plugin : public IComponent, public IAudioProcessor,
                  public IEditController, public IMidiMapping
{
public:
	mu_plugin()
	{
		for (int32 i = 0; i < kParamCount; i++)
			m_value[i] = default_of(i % kCtrlCount);
		m_msgs.reserve(8192);
		m_engine.set_output_rate(smu2000::vst3::NATIVE_RATE);
		LARGE_INTEGER f;
		QueryPerformanceFrequency(&f);
		m_qpc_freq = f.QuadPart;
	}

	virtual ~mu_plugin() = default;

	// ---- FUnknown

	tresult PLUGIN_API queryInterface(const TUID _iid, void **obj) override
	{
		if (FUnknownPrivate::iidEqual(_iid, FUnknown::iid) ||
		    FUnknownPrivate::iidEqual(_iid, IComponent::iid)) {
			addRef(); *obj = static_cast<IComponent *>(this); return kResultOk;
		}
		if (FUnknownPrivate::iidEqual(_iid, IPluginBase::iid)) {
			// IComponent と IEditController の両方が IPluginBase を持つので、
			// どちらの側か決めてやらないと曖昧になる
			addRef(); *obj = static_cast<IPluginBase *>(static_cast<IComponent *>(this));
			return kResultOk;
		}
		if (FUnknownPrivate::iidEqual(_iid, IAudioProcessor::iid)) {
			addRef(); *obj = static_cast<IAudioProcessor *>(this); return kResultOk;
		}
		if (FUnknownPrivate::iidEqual(_iid, IEditController::iid)) {
			addRef(); *obj = static_cast<IEditController *>(this); return kResultOk;
		}
		if (FUnknownPrivate::iidEqual(_iid, IMidiMapping::iid)) {
			addRef(); *obj = static_cast<IMidiMapping *>(this); return kResultOk;
		}
		*obj = nullptr;
		return kNoInterface;
	}

	uint32 PLUGIN_API addRef() override
	{ return uint32(FUnknownPrivate::atomicAdd(m_refs, 1)); }

	uint32 PLUGIN_API release() override
	{
		if (FUnknownPrivate::atomicAdd(m_refs, -1) == 0) { delete this; return 0; }
		return uint32(m_refs);
	}

	// ---- IPluginBase

	tresult PLUGIN_API initialize(FUnknown *) override
	{
		// ROM 読みと起動（音にして 4 秒ぶんの空回し）は時間がかかるので、
		// ここでは走らせるだけ。終わるまでは無音を返す
		m_engine.start();
		return kResultOk;
	}

	tresult PLUGIN_API terminate() override { return kResultOk; }

	// ---- IComponent

	tresult PLUGIN_API getControllerClassId(TUID) override
	{
		// 単一コンポーネント。ホストはこの同じ物から IEditController を取る
		return kNotImplemented;
	}

	tresult PLUGIN_API setIoMode(IoMode) override { return kNotImplemented; }

	int32 PLUGIN_API getBusCount(MediaType type, BusDirection dir) override
	{
		if (type == kAudio) return dir == kOutput ? 1 : 0;
		if (type == kEvent) return dir == kInput  ? 1 : 0;
		return 0;
	}

	tresult PLUGIN_API getBusInfo(MediaType type, BusDirection dir, int32 index,
	                              BusInfo &bus) override
	{
		if (index != 0)
			return kInvalidArgument;
		if (type == kAudio && dir == kOutput) {
			bus.mediaType    = kAudio;
			bus.direction    = kOutput;
			bus.channelCount = 2;
			set_str(bus.name, "Stereo Out");
			bus.busType = kMain;
			bus.flags   = BusInfo::kDefaultActive;
			return kResultOk;
		}
		if (type == kEvent && dir == kInput) {
			bus.mediaType    = kEvent;
			bus.direction    = kInput;
			bus.channelCount = 16;
			set_str(bus.name, "MIDI In");
			bus.busType = kMain;
			bus.flags   = BusInfo::kDefaultActive;
			return kResultOk;
		}
		return kInvalidArgument;
	}

	tresult PLUGIN_API getRoutingInfo(RoutingInfo &, RoutingInfo &) override
	{ return kNotImplemented; }

	tresult PLUGIN_API activateBus(MediaType, BusDirection, int32, TBool) override
	{ return kResultOk; }

	tresult PLUGIN_API setActive(TBool state) override
	{
		if (state) {
			m_engine.start();
		} else {
			m_hush.store(true);
			report();
		}
		return kResultOk;
	}

	// 画面が無いので、間に合っていたかどうかは止めるときに記録へ書く
	void report()
	{
		if (!m_produced)
			return;
		const double audio = double(m_produced) / m_rate;
		const double busy  = double(m_busy_ticks) / double(m_qpc_freq);
		char line[256];
		std::snprintf(line, sizeof(line),
		              "%.0f Hz で %.0f 秒ぶん: CPU %.1f%%、1 ブロックの最悪 %.2f ms、"
		              "間に合わなかった回数 %llu",
		              m_rate, audio, 100.0 * busy / audio,
		              1000.0 * double(m_worst_ticks) / double(m_qpc_freq),
		              (unsigned long long)m_late);
		m_engine.log_line(line);
		m_busy_ticks = m_produced = m_worst_ticks = m_late = 0;
	}

	tresult PLUGIN_API setState(IBStream *) override { return kResultOk; }

	tresult PLUGIN_API getState(IBStream *stream) override
	{
		// 音源の中身（RAM や音色）はまだ保存できない。版番号だけ書いておく
		if (!stream)
			return kResultFalse;
		const int32 version = 1;
		int32 written = 0;
		stream->write(const_cast<int32 *>(&version), sizeof(version), &written);
		return kResultOk;
	}

	// ---- IAudioProcessor

	tresult PLUGIN_API setBusArrangements(SpeakerArrangement *, int32 numIns,
	                                      SpeakerArrangement *outputs, int32 numOuts) override
	{
		if (numIns == 0 && numOuts == 1 && outputs[0] == SpeakerArr::kStereo)
			return kResultTrue;
		return kResultFalse;
	}

	tresult PLUGIN_API getBusArrangement(BusDirection dir, int32 index,
	                                     SpeakerArrangement &arr) override
	{
		if (dir == kOutput && index == 0) { arr = SpeakerArr::kStereo; return kResultOk; }
		return kInvalidArgument;
	}

	tresult PLUGIN_API canProcessSampleSize(int32 size) override
	{ return size == kSample32 ? kResultTrue : kResultFalse; }

	uint32 PLUGIN_API getLatencySamples() override { return m_engine.latency_samples(); }

	tresult PLUGIN_API setupProcessing(ProcessSetup &setup) override
	{
		m_rate = setup.sampleRate;
		m_engine.set_output_rate(m_rate);
		return kResultOk;
	}

	tresult PLUGIN_API setProcessing(TBool state) override
	{
		if (!state)
			m_hush.store(true);
		return kResultOk;
	}

	uint32 PLUGIN_API getTailSamples() override
	{
		// 残響がある。4 秒みておく
		return uint32(m_rate * 4.0);
	}

	tresult PLUGIN_API process(ProcessData &data) override;

	// ---- IEditController

	tresult PLUGIN_API setComponentState(IBStream *) override { return kResultOk; }

	int32 PLUGIN_API getParameterCount() override { return kParamCount; }

	tresult PLUGIN_API getParameterInfo(int32 index, ParameterInfo &info) override
	{
		if (index < 0 || index >= kParamCount)
			return kInvalidArgument;
		const int32 ch = index / kCtrlCount, ctrl = index % kCtrlCount;

		char name[64];
		if (ctrl < 128)      std::snprintf(name, sizeof(name), "Ch%d CC%d", ch + 1, ctrl);
		else if (ctrl == 128) std::snprintf(name, sizeof(name), "Ch%d Aftertouch", ch + 1);
		else if (ctrl == 129) std::snprintf(name, sizeof(name), "Ch%d Pitch Bend", ch + 1);
		else                  std::snprintf(name, sizeof(name), "Ch%d Program", ch + 1);

		std::memset(&info, 0, sizeof(info));
		info.id = param_of(ch, ctrl);
		set_str(info.title, name);
		set_str(info.shortTitle, name);
		info.stepCount = (ctrl == 129) ? 0 : 127;   // ピッチベンドだけ連続
		info.defaultNormalizedValue = default_of(ctrl);
		info.unitId = 0;   // kRootUnitId
		// 2096 本もあるので、一覧に並べさせない
		info.flags = ParameterInfo::kCanAutomate | ParameterInfo::kIsHidden;
		return kResultOk;
	}

	tresult PLUGIN_API getParamStringByValue(ParamID id, ParamValue v, String128 str) override
	{
		if (id >= ParamID(kParamCount))
			return kInvalidArgument;
		char buf[32];
		if (id % kCtrlCount == 129)
			std::snprintf(buf, sizeof(buf), "%+d", int(std::lround(v * 16383.0)) - 8192);
		else
			std::snprintf(buf, sizeof(buf), "%d", int(std::lround(v * 127.0)));
		set_str(str, buf);
		return kResultOk;
	}

	tresult PLUGIN_API getParamValueByString(ParamID id, TChar *str, ParamValue &v) override
	{
		if (id >= ParamID(kParamCount) || !str)
			return kInvalidArgument;
		char buf[32];
		int i = 0;
		for (; i < 31 && str[i]; i++)
			buf[i] = char(str[i]);
		buf[i] = 0;
		const double plain = std::atof(buf);
		v = (id % kCtrlCount == 129) ? std::clamp((plain + 8192.0) / 16383.0, 0.0, 1.0)
		                             : std::clamp(plain / 127.0, 0.0, 1.0);
		return kResultOk;
	}

	ParamValue PLUGIN_API normalizedParamToPlain(ParamID id, ParamValue v) override
	{
		return (id % kCtrlCount == 129) ? std::lround(v * 16383.0) - 8192.0
		                                : std::lround(v * 127.0);
	}

	ParamValue PLUGIN_API plainParamToNormalized(ParamID id, ParamValue plain) override
	{
		return (id % kCtrlCount == 129) ? std::clamp((plain + 8192.0) / 16383.0, 0.0, 1.0)
		                                : std::clamp(plain / 127.0, 0.0, 1.0);
	}

	ParamValue PLUGIN_API getParamNormalized(ParamID id) override
	{ return id < ParamID(kParamCount) ? m_value[id] : 0.0; }

	tresult PLUGIN_API setParamNormalized(ParamID id, ParamValue v) override
	{
		if (id >= ParamID(kParamCount))
			return kInvalidArgument;
		m_value[id] = v;
		return kResultOk;
	}

	tresult PLUGIN_API setComponentHandler(IComponentHandler *) override { return kResultOk; }

	IPlugView *PLUGIN_API createView(FIDString) override { return nullptr; }

	// ---- IMidiMapping

	tresult PLUGIN_API getMidiControllerAssignment(int32 busIndex, int16 channel,
	                                               CtrlNumber ctrl, ParamID &id) override
	{
		if (busIndex != 0 || channel < 0 || channel >= kChannels)
			return kResultFalse;
		if (ctrl < 0 || ctrl >= kCtrlCount)
			return kResultFalse;
		id = param_of(channel, ctrl);
		return kResultTrue;
	}

private:
	// process の中で時刻順に並べ直すための入れ物。
	// 短いものは中に持ち、システムエクスクルーシブはホストの領域を指す
	struct msg {
		int32          off;
		int32          seq;
		uint8          n;
		uint8          b[3];
		const uint8   *sysex;
		uint32         sysex_len;
	};

	void queue(int32 off, uint8 a, uint8 b = 0, uint8 c = 0, int n = 3)
	{
		if (m_msgs.size() >= m_msgs.capacity())
			return;
		m_msgs.push_back({ off, int32(m_msgs.size()), uint8(n), { a, b, c }, nullptr, 0 });
	}

	smu2000::vst3::engine m_engine;
	std::vector<msg>      m_msgs;
	double                m_rate = smu2000::vst3::NATIVE_RATE;
	double                m_value[kParamCount] = {};
	std::atomic<bool>     m_hush{false};
	// 間に合っているかの記録。音声スレッドだけが触る
	uint64                m_busy_ticks = 0, m_produced = 0, m_worst_ticks = 0, m_late = 0;
	int64                 m_qpc_freq = 1;
	int32                 m_refs = 1;
};


tresult PLUGIN_API mu_plugin::process(ProcessData &data)
{
	// 64bit 浮動小数は受けないと答えてある。それでも来たら音を出さない
	// （倍精度の配列を単精度として書けば壊れる）
	AudioBusBuffers *out = (data.numOutputs > 0 &&
	                        data.symbolicSampleSize == kSample32) ? &data.outputs[0] : nullptr;
	float *left  = (out && out->numChannels > 0) ? out->channelBuffers32[0] : nullptr;
	float *right = (out && out->numChannels > 1) ? out->channelBuffers32[1] : left;

	LARGE_INTEGER t0;
	QueryPerformanceCounter(&t0);

	if (m_hush.exchange(false))
		m_engine.all_notes_off();

	// ---- まず、この区間に来た MIDI を全部集める

	m_msgs.clear();

	if (IParameterChanges *changes = data.inputParameterChanges) {
		const int32 nq = changes->getParameterCount();
		for (int32 q = 0; q < nq; q++) {
			IParamValueQueue *pq = changes->getParameterData(q);
			if (!pq)
				continue;
			const ParamID id = pq->getParameterId();
			if (id >= ParamID(kParamCount))
				continue;
			const int32 ch = int32(id) / kCtrlCount, ctrl = int32(id) % kCtrlCount;
			const int32 np = pq->getPointCount();
			for (int32 p = 0; p < np; p++) {
				int32 off = 0;
				ParamValue v = 0.0;
				if (pq->getPoint(p, off, v) != kResultOk)
					continue;
				m_value[id] = v;
				// ここで「前と同じ値だから」と捨ててはいけない。RPN/NRPN は
				// CC101=0, CC100=0, CC6=n のように同じ値を続けて送ることに
				// 意味があり、捨てるとピッチベンド幅などが化ける

				if (ctrl < 128) {
					queue(off, uint8(0xb0 | ch), uint8(ctrl),
					      uint8(std::clamp(int(std::lround(v * 127.0)), 0, 127)));
				} else if (ctrl == 128) {
					queue(off, uint8(0xd0 | ch),
					      uint8(std::clamp(int(std::lround(v * 127.0)), 0, 127)), 0, 2);
				} else if (ctrl == 129) {
					const int bend = std::clamp(int(std::lround(v * 16383.0)), 0, 16383);
					queue(off, uint8(0xe0 | ch), uint8(bend & 127), uint8(bend >> 7));
				} else {
					queue(off, uint8(0xc0 | ch),
					      uint8(std::clamp(int(std::lround(v * 127.0)), 0, 127)), 0, 2);
				}
			}
		}
	}

	if (IEventList *events = data.inputEvents) {
		const int32 n = events->getEventCount();
		for (int32 i = 0; i < n; i++) {
			Event e;
			if (events->getEvent(i, e) != kResultOk)
				continue;
			const int32 off = e.sampleOffset;
			switch (e.type) {
			case Event::kNoteOnEvent: {
				const int v = std::clamp(int(std::lround(e.noteOn.velocity * 127.0)), 1, 127);
				queue(off, uint8(0x90 | (e.noteOn.channel & 15)),
				      uint8(e.noteOn.pitch & 127), uint8(v));
				break;
			}
			case Event::kNoteOffEvent: {
				const int v = std::clamp(int(std::lround(e.noteOff.velocity * 127.0)), 0, 127);
				queue(off, uint8(0x80 | (e.noteOff.channel & 15)),
				      uint8(e.noteOff.pitch & 127), uint8(v));
				break;
			}
			case Event::kPolyPressureEvent: {
				const int v = std::clamp(int(std::lround(e.polyPressure.pressure * 127.0)), 0, 127);
				queue(off, uint8(0xa0 | (e.polyPressure.channel & 15)),
				      uint8(e.polyPressure.pitch & 127), uint8(v));
				break;
			}
			case Event::kDataEvent:
				if (e.data.type == DataEvent::kMidiSysEx && e.data.bytes && e.data.size &&
				    m_msgs.size() < m_msgs.capacity())
					m_msgs.push_back({ off, int32(m_msgs.size()), 0, { 0, 0, 0 },
					                   e.data.bytes, e.data.size });
				break;
			default:
				break;
			}
		}
	}

	std::sort(m_msgs.begin(), m_msgs.end(), [](const msg &a, const msg &b) {
		return a.off != b.off ? a.off < b.off : a.seq < b.seq;
	});

	// ---- 時刻順に、音を作りながら流し込む

	const int32 n = data.numSamples;
	int32 done = 0;
	for (const msg &m : m_msgs) {
		const int32 at = std::clamp(m.off, done, n);
		if (at > done && left) {
			m_engine.fill(left + done, right + done, at - done);
			done = at;
		} else if (at > done) {
			done = at;
		}
		if (m.sysex)
			m_engine.midi(m.sysex, m.sysex_len);
		else
			m_engine.midi(m.b, m.n);
	}
	if (left && done < n)
		m_engine.fill(left + done, right + done, n - done);

	// 間に合っているかの目安。setActive(false) のときに記録へ書く
	LARGE_INTEGER t1;
	QueryPerformanceCounter(&t1);
	const uint64 took = uint64(t1.QuadPart - t0.QuadPart);
	m_busy_ticks += took;
	m_produced   += uint64(n);
	if (took > m_worst_ticks) m_worst_ticks = took;
	if (double(took) / double(m_qpc_freq) > double(n) / m_rate) m_late++;

	if (out) {
		out->silenceFlags = 0;
		// 片側しか無いホストのために、右が左と同じ配列でも困らないようにしてある
		if (out->numChannels > 2)
			for (int32 c = 2; c < out->numChannels; c++)
				std::memset(out->channelBuffers32[c], 0, size_t(n) * sizeof(float));
	}
	return kResultOk;
}


// ---- 工場。ホストはまずこれを取りに来る

class factory : public IPluginFactory3
{
public:
	tresult PLUGIN_API queryInterface(const TUID _iid, void **obj) override
	{
		if (FUnknownPrivate::iidEqual(_iid, FUnknown::iid) ||
		    FUnknownPrivate::iidEqual(_iid, IPluginFactory::iid) ||
		    FUnknownPrivate::iidEqual(_iid, IPluginFactory2::iid) ||
		    FUnknownPrivate::iidEqual(_iid, IPluginFactory3::iid)) {
			addRef(); *obj = static_cast<IPluginFactory3 *>(this); return kResultOk;
		}
		*obj = nullptr;
		return kNoInterface;
	}

	uint32 PLUGIN_API addRef() override  { return uint32(FUnknownPrivate::atomicAdd(m_refs, 1)); }
	uint32 PLUGIN_API release() override { return uint32(FUnknownPrivate::atomicAdd(m_refs, -1)); }

	tresult PLUGIN_API getFactoryInfo(PFactoryInfo *info) override
	{
		if (!info)
			return kInvalidArgument;
		std::memset(static_cast<void *>(info), 0, sizeof(*info));
		std::strncpy(info->vendor, kVendor, PFactoryInfo::kNameSize - 1);
		std::strncpy(info->url, "https://github.com/tarboh/S-MU2000", PFactoryInfo::kURLSize - 1);
		info->flags = PFactoryInfo::kUnicode;
		return kResultOk;
	}

	int32 PLUGIN_API countClasses() override { return 1; }

	tresult PLUGIN_API getClassInfo(int32 index, PClassInfo *info) override
	{
		if (index != 0 || !info)
			return kInvalidArgument;
		std::memset(static_cast<void *>(info), 0, sizeof(*info));
		std::memcpy(info->cid, kProcessorUID.toTUID(), sizeof(TUID));
		info->cardinality = PClassInfo::kManyInstances;
		std::strncpy(info->category, kVstAudioEffectClass, PClassInfo::kCategorySize - 1);
		std::strncpy(info->name, kPlugName, PClassInfo::kNameSize - 1);
		return kResultOk;
	}

	tresult PLUGIN_API getClassInfo2(int32 index, PClassInfo2 *info) override
	{
		if (index != 0 || !info)
			return kInvalidArgument;
		std::memset(static_cast<void *>(info), 0, sizeof(*info));
		std::memcpy(info->cid, kProcessorUID.toTUID(), sizeof(TUID));
		info->cardinality = PClassInfo::kManyInstances;
		std::strncpy(info->category, kVstAudioEffectClass, PClassInfo::kCategorySize - 1);
		std::strncpy(info->name, kPlugName, PClassInfo::kNameSize - 1);
		info->classFlags = 0;
		std::strncpy(info->subCategories, PlugType::kInstrumentSynth,
		             PClassInfo2::kSubCategoriesSize - 1);
		std::strncpy(info->vendor, kVendor, PClassInfo2::kVendorSize - 1);
		std::strncpy(info->version, kVersion, PClassInfo2::kVersionSize - 1);
		std::strncpy(info->sdkVersion, kVstVersionString, PClassInfo2::kVersionSize - 1);
		return kResultOk;
	}

	tresult PLUGIN_API getClassInfoUnicode(int32 index, PClassInfoW *info) override
	{
		if (index != 0 || !info)
			return kInvalidArgument;
		std::memset(static_cast<void *>(info), 0, sizeof(*info));
		std::memcpy(info->cid, kProcessorUID.toTUID(), sizeof(TUID));
		info->cardinality = PClassInfo::kManyInstances;
		std::strncpy(info->category, kVstAudioEffectClass, PClassInfo::kCategorySize - 1);
		set_str16(info->name, kPlugName, PClassInfo::kNameSize);
		info->classFlags = 0;
		std::strncpy(info->subCategories, PlugType::kInstrumentSynth,
		             PClassInfo2::kSubCategoriesSize - 1);
		set_str16(info->vendor, kVendor, PClassInfo2::kVendorSize);
		set_str16(info->version, kVersion, PClassInfo2::kVersionSize);
		set_str16(info->sdkVersion, kVstVersionString, PClassInfo2::kVersionSize);
		return kResultOk;
	}

	tresult PLUGIN_API setHostContext(FUnknown *) override { return kResultOk; }

	tresult PLUGIN_API createInstance(FIDString cid, FIDString _iid, void **obj) override
	{
		if (!cid || !_iid || !obj)
			return kInvalidArgument;
		if (!FUnknownPrivate::iidEqual(cid, kProcessorUID.toTUID()))
			return kNoInterface;
		mu_plugin *p = new mu_plugin;
		const tresult r = p->queryInterface(_iid, obj);
		p->release();
		return r;
	}

private:
	static void set_str16(char16 *dst, const char *ascii, int max)
	{
		int i = 0;
		for (; ascii[i] && i < max - 1; i++)
			dst[i] = char16(uint8(ascii[i]));
		dst[i] = 0;
	}

	int32 m_refs = 1;
};

factory g_factory;

} // namespace


// ---- DLL の出口

extern "C" {

__declspec(dllexport) IPluginFactory *PLUGIN_API GetPluginFactory()
{
	g_factory.addRef();
	return &g_factory;
}

__declspec(dllexport) bool InitDll() { return true; }
__declspec(dllexport) bool ExitDll() { return true; }

}
