// license:BSD-3-Clause
//
// A small host that loads and runs S-MU2000's Audio Unit v2 with no DAW around
// it.
//
//   aubprobe <bundle> [<MIDI> [<out.wav>]] [--rate 48000] [--block 512]
//                                          [--tail 3] [--torture] [--list]
//   aubprobe - [<MIDI> ...]        use whatever AU is installed
//
// Same job as vst3probe (src/vst3/probe.cpp), reached differently: that one
// looks for GetPluginFactory through CFBundle, while this one puts the factory
// into AudioComponentRegister and opens the unit with AudioComponentInstanceNew
// -- **the road a host actually drives down**, so a spelling mistake in
// Info.plist's factoryFunction shows up right here.
//
// Sound is made the same way as vst3probe: an event's time is turned into an
// offset within the block and handed to MusicDeviceMIDIEvent. By AU's rules that
// offset points into the block about to be rendered, so the plug-in injects it
// at that moment inside Render (src/au/plugin.cpp) -- which is why comparing the
// two renders shows up timing differences.

#include "compat/console.h"
#include "smf.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreFoundation/CoreFoundation.h>

// The editor check below is written against the Objective-C runtime rather than
// AppKit: it has to make a view, and aubprobe is plain C++
#include <objc/message.h>
#include <objc/runtime.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr OSType kType         = 'aumu';
constexpr OSType kSubtype      = 'SMU2';
constexpr OSType kManufacturer = 'Trbh';

// These two are in libobjc but not declared in the SDK's headers. The view a
// host is handed is autoreleased, so without a pool every editor it makes
// complains on the way out
extern "C" void *objc_autoreleasePoolPush(void);
extern "C" void  objc_autoreleasePoolPop(void *pool);

long long now_ms()
{
	return std::chrono::duration_cast<std::chrono::milliseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

void write_wav(const std::string &path, const std::vector<int16_t> &pcm, uint32_t rate)
{
	std::FILE *f = std::fopen(path.c_str(), "wb");
	if (!f) {
		std::fprintf(stderr, "書けない: %s\n", path.c_str());
		return;
	}
	const uint32_t data = uint32_t(pcm.size() * 2);
	const uint32_t riff = 36 + data;
	const uint16_t ch = 2, bits = 16;
	const uint32_t byte_rate = rate * ch * bits / 8;
	const uint16_t align = uint16_t(ch * bits / 8);
	auto u32 = [&](uint32_t v) { std::fwrite(&v, 4, 1, f); };
	auto u16 = [&](uint16_t v) { std::fwrite(&v, 2, 1, f); };
	std::fwrite("RIFF", 1, 4, f); u32(riff);
	std::fwrite("WAVEfmt ", 1, 8, f); u32(16); u16(1); u16(ch);
	u32(rate); u32(byte_rate); u16(align); u16(bits);
	std::fwrite("data", 1, 4, f); u32(data);
	if (!pcm.empty())
		std::fwrite(pcm.data(), 2, pcm.size(), f);
	std::fclose(f);
}

AudioStreamBasicDescription float_format(double rate, uint32_t channels)
{
	AudioStreamBasicDescription f{};
	f.mSampleRate       = rate;
	f.mFormatID         = kAudioFormatLinearPCM;
	// The two flags come from different anonymous enums, so the or needs a cast
	f.mFormatFlags      = AudioFormatFlags(kAudioFormatFlagsNativeFloatPacked) |
	                      AudioFormatFlags(kAudioFormatFlagIsNonInterleaved);
	f.mChannelsPerFrame = channels;
	f.mBitsPerChannel   = 32;
	f.mFramesPerPacket  = 1;
	f.mBytesPerFrame    = 4;
	f.mBytesPerPacket   = 4;
	return f;
}

// Read the bundle and assemble the component the way a host does. An empty
// bundle means "find whichever AU is already installed", which is the road
// auval takes
AudioComponent find_component(const std::string &bundle, std::string &err)
{
	AudioComponentDescription desc{};
	desc.componentType = kType;
	desc.componentSubType = kSubtype;
	desc.componentManufacturer = kManufacturer;
	desc.componentFlags = 0;
	desc.componentFlagsMask = 0;

	if (bundle.empty() || bundle == "-")
		return AudioComponentFindNext(nullptr, &desc);

	// Open it with CFBundle. CFBundleCreate succeeds because Info.plist is there
	CFURLRef url = CFURLCreateFromFileSystemRepresentation(
		nullptr, reinterpret_cast<const UInt8 *>(bundle.c_str()), bundle.size(), true);
	CFBundleRef bun = url ? CFBundleCreate(nullptr, url) : nullptr;
	if (url)
		CFRelease(url);
	if (!bun) {
		err = "バンドルを開けない: " + bundle;
		return nullptr;
	}
	if (!CFBundleLoadExecutable(bun)) {
		err = "バンドルを読めない: " + bundle;
		return nullptr;
	}
	auto entry = reinterpret_cast<bool (*)(CFBundleRef)>(
		CFBundleGetFunctionPointerForName(bun, CFSTR("bundleEntry")));
	if (entry)
		entry(bun);

	auto factory = reinterpret_cast<AudioComponentFactoryFunction>(
		CFBundleGetFunctionPointerForName(bun, CFSTR("SMU2000AUFactory")));
	if (!factory) {
		err = "SMU2000AUFactory が無い（Info.plist の factoryFunction と綴りを合わせる）";
		return nullptr;
	}
	// CFSTR needs a literal, so the name is spelled out rather than shared
	return AudioComponentRegister(&desc, CFSTR("S-MU2000"), 0, factory);
}

// Hand a run of MIDI bytes to the MusicDevice entry point one message at a
// time. An SMF event can carry several messages back to back, so running status
// has to be followed to split them up
void send_midi(AudioUnit unit, const std::vector<u8> &bytes, UInt32 offset)
{
	size_t i = 0;
	UInt8 status = 0;
	while (i < bytes.size()) {
		const UInt8 b = bytes[i];
		if (b >= 0xf8) {                    // real-time: complete in one byte
			MusicDeviceMIDIEvent(unit, b, 0, 0, offset);
			i++;
			continue;
		}
		if (b == 0xf0) {
			// By AU's rules SysEx is handed over whole, F0 through F7
			size_t end = i + 1;
			while (end < bytes.size() && bytes[end] != 0xf7)
				end++;
			if (end < bytes.size())
				end++;
			MusicDeviceSysEx(unit, bytes.data() + i, UInt32(end - i));
			i = end;
			status = 0;
			continue;
		}
		if (b >= 0x80) {
			status = b;
			i++;
		} else if (status == 0) {
			i++;                            // stray data with no status: drop it
			continue;
		}
		if (status >= 0xf0) {
			i++;
			continue;
		}
		const UInt8 cmd = status & 0xf0;
		const int n = (cmd == 0xc0 || cmd == 0xd0) ? 1 : 2;
		const UInt32 d1 = (i < bytes.size()) ? bytes[i] : 0;
		const UInt32 d2 = (i + 1 < bytes.size()) ? bytes[i + 1] : 0;
		MusicDeviceMIDIEvent(unit, status, d1, d2, offset);
		i += size_t(n);
	}
}

// AudioBufferList declares one buffer; a second has to be carried alongside it
// and the two have to stay adjacent, which is what this is for
struct audio_buffers_2 {
	AudioBufferList list;
	AudioBuffer extra;
};

} // namespace


// ---------------------------------------------------------------------------

namespace {

int show_info(AudioUnit unit)
{
	std::printf("パラメータ:\n");
	UInt32 size = 0;
	if (AudioUnitGetPropertyInfo(unit, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0,
	                             &size, nullptr) == noErr) {
		std::vector<AudioUnitParameterID> ids(size / sizeof(AudioUnitParameterID));
		if (AudioUnitGetProperty(unit, kAudioUnitProperty_ParameterList, kAudioUnitScope_Global, 0,
		                         ids.data(), &size) == noErr) {
			for (AudioUnitParameterID id : ids) {
				AudioUnitParameterInfo info{};
				UInt32 is = sizeof(info);
				if (AudioUnitGetProperty(unit, kAudioUnitProperty_ParameterInfo,
				                         kAudioUnitScope_Global, id, &info, &is) == noErr) {
					char name[64] = {};
					if ((info.flags & kAudioUnitParameterFlag_HasCFNameString) && info.cfNameString)
						CFStringGetCString(info.cfNameString, name, sizeof(name), kCFStringEncodingUTF8);
					AudioUnitParameterValue v = 0;
					AudioUnitGetParameter(unit, id, kAudioUnitScope_Global, 0, &v);
					std::printf("  [%u] %-20s %.3f - %.3f（今 %.3f）\n", unsigned(id), name,
					            double(info.minValue), double(info.maxValue), double(v));
				}
			}
		}
	}

	Float64 latency = 0.0;
	size = sizeof(latency);
	if (AudioUnitGetProperty(unit, kAudioUnitProperty_Latency, kAudioUnitScope_Global, 0,
	                         &latency, &size) == noErr)
		std::printf("遅れ %.6f 秒\n", latency);

	CFStringRef iname = nullptr;
	size = sizeof(iname);
	if (AudioUnitGetProperty(unit, kMusicDeviceProperty_InstrumentName, kAudioUnitScope_Global, 0,
	                         &iname, &size) == noErr && iname) {
		char buf[128] = {};
		CFStringGetCString(iname, buf, sizeof(buf), kCFStringEncodingUTF8);
		std::printf("楽器名 %s\n", buf);
		CFRelease(iname);
	}
	return 0;
}

int run_render(AudioUnit unit, double rate, int block, double extra,
               const std::string &mid, const std::string &wav)
{
	std::vector<smf::event> events;
	std::string err;
	if (!smf::load(mid, events, err)) {
		std::fprintf(stderr, "MIDI を読めない: %s\n", err.c_str());
		return 1;
	}
	double last = 0.0;
	for (const smf::event &e : events)
		last = std::max(last, e.time);
	std::printf("MIDI: %zu イベント、%.2f 秒\n", events.size(), last);

	const AudioStreamBasicDescription fmt = float_format(rate, 2);
	if (AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output,
	                         0, &fmt, sizeof(fmt)) != noErr) {
		std::fprintf(stderr, "標本化周波数を入れられない\n");
		return 1;
	}
	const UInt32 maxf = UInt32(block);
	AudioUnitSetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global,
	                     0, &maxf, sizeof(maxf));

	if (AudioUnitInitialize(unit) != noErr) {
		std::fprintf(stderr, "初期化できない\n");
		return 1;
	}

	std::printf("起動待ち...");
	std::fflush(stdout);
	const long long t_wait = now_ms();
	for (;;) {
		AudioUnitParameterValue v = 0;
		AudioUnitGetParameter(unit, 1 /* status */, kAudioUnitScope_Global, 0, &v);
		if (v >= 1.0f)
			break;
		if (now_ms() - t_wait > 60000) {
			std::printf(" 60 秒待っても始まらない\n");
			break;
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(50));
	}
	std::printf(" %ld ms\n", long(now_ms() - t_wait));

	const int64_t total = int64_t((last + extra) * rate);
	std::vector<float> l;
	std::vector<float> r;
	l.resize(size_t(block));
	r.resize(size_t(block));
	std::vector<int16_t> pcm;
	pcm.reserve(size_t(total) * 2);

	size_t next = 0;
	int64_t pos = 0;
	const long long t0 = now_ms();
	while (pos < total) {
		const UInt32 n = UInt32(std::min<int64_t>(block, total - pos));

		// Hand over the MIDI that falls in this block, turned into an offset
		// within it
		while (next < events.size() && events[next].time * rate < double(pos + n)) {
			const double at = events[next].time * rate;
			const UInt32 off = UInt32(std::clamp<double>(at - double(pos), 0.0, double(n > 0 ? n - 1 : 0)));
			send_midi(unit, events[next].bytes, off);
			next++;
		}

		audio_buffers_2 bufs{};
		bufs.list.mNumberBuffers = 2;
		bufs.list.mBuffers[0].mNumberChannels = 1;
		bufs.list.mBuffers[0].mDataByteSize = n * 4;
		bufs.list.mBuffers[0].mData = l.data();
		bufs.list.mBuffers[1].mNumberChannels = 1;
		bufs.list.mBuffers[1].mDataByteSize = n * 4;
		bufs.list.mBuffers[1].mData = r.data();

		AudioTimeStamp ts{};
		ts.mSampleTime = double(pos);
		ts.mFlags = kAudioTimeStampSampleTimeValid;

		AudioUnitRenderActionFlags flags = 0;
		const OSStatus st = AudioUnitRender(unit, &flags, &ts, 0, n, &bufs.list);
		if (st != noErr) {
			std::fprintf(stderr, "Render が失敗: %d（%lld サンプル目）\n", int(st), (long long)pos);
			break;
		}

		for (UInt32 i = 0; i < n; i++) {
			const float lv = std::clamp(l[i], -1.0f, 1.0f);
			const float rv = std::clamp(r[i], -1.0f, 1.0f);
			pcm.push_back(int16_t(std::lround(lv * 32767.0f)));
			pcm.push_back(int16_t(std::lround(rv * 32767.0f)));
		}
		pos += n;
	}
	const long long t1 = now_ms();

	AudioUnitUninitialize(unit);

	write_wav(wav, pcm, uint32_t(rate));
	double peak = 0.0, sum = 0.0;
	for (int16_t v : pcm) { peak = std::max(peak, std::fabs(double(v))); sum += std::fabs(double(v)); }
	std::printf("書き出した: %s（%.1f 秒 / %.0f Hz、実時間 %.1f 秒）\n",
	            wav.c_str(), double(total) / rate, rate, (t1 - t0) / 1000.0);
	std::printf("最大 %.0f  平均 %.1f\n", peak, sum / std::max<size_t>(pcm.size(), 1));
	return 0;
}

} // namespace


// ---------------------------------------------------------------------------

namespace {

// The editor, walked the way a host walks it: read kAudioUnitProperty_CocoaUI,
// resolve the class it names, and ask that class for a view. The parts a DAW
// would notice first are exactly these -- the property naming a class that
// exists, the name matching, and the class building a panel.
//
// There is no window in here, so what ends up on screen is still a DAW's word;
// that the panel is painted correctly is checked in doc/porting-macos.md
int check_editor(AudioUnit u, const std::string &bundle_path)
{
	UInt32 size = 0;
	Boolean writable = true;
	if (AudioUnitGetPropertyInfo(u, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0,
	                             &size, &writable) != noErr) {
		std::printf("NG: CocoaUI に答えない\n");
		return 1;
	}
	int bad = 0;
	if (size != sizeof(AudioUnitCocoaViewInfo) || writable) {
		std::printf("NG: CocoaUI の大きさが %u（%zu のはず）、読み取り専用は %d\n",
		            unsigned(size), sizeof(AudioUnitCocoaViewInfo), int(writable));
		bad++;
	}

	AudioUnitCocoaViewInfo info{};
	UInt32 got = sizeof(info);
	if (AudioUnitGetProperty(u, kAudioUnitProperty_CocoaUI, kAudioUnitScope_Global, 0,
	                         &info, &got) != noErr) {
		std::printf("NG: CocoaUI を読めない\n");
		return bad + 1;
	}

	char url_path[4096] = {};
	char class_name[256] = {};
	const bool have_url = info.mCocoaAUViewBundleLocation &&
	    CFURLGetFileSystemRepresentation(info.mCocoaAUViewBundleLocation, true,
	                                     reinterpret_cast<UInt8 *>(url_path), sizeof(url_path));
	if (info.mCocoaAUViewClass[0])
		CFStringGetCString(info.mCocoaAUViewClass[0], class_name, sizeof(class_name),
		                   kCFStringEncodingUTF8);

	if (!have_url || !class_name[0]) {
		std::printf("NG: CocoaUI がバンドルとクラス名を返さない\n");
		return bad + 1;
	}
	// The AU publishes an absolute URL, which is what a host needs; the bundle
	// this probe was handed may be a relative path
	char resolved[4096] = {};
	const char *want_path = realpath(bundle_path.c_str(), resolved) ? resolved
	                                                                : bundle_path.c_str();
	if (std::string(url_path) != want_path) {
		std::printf("NG: CocoaUI のバンドルが違う: %s（%s のはず）\n", url_path, want_path);
		bad++;
	}

	Class cls = objc_getClass(class_name);
	if (!cls) {
		std::printf("NG: クラス %s が無い（CocoaUI の綴りと @interface を合わせる）\n", class_name);
		return bad + 1;
	}

	void *pool = objc_autoreleasePoolPush();

	id factory = ((id (*)(id, SEL))objc_msgSend)((id)cls, sel_registerName("alloc"));
	factory = ((id (*)(id, SEL))objc_msgSend)(factory, sel_registerName("init"));
	const unsigned version = ((unsigned (*)(id, SEL))objc_msgSend)(
	    factory, sel_registerName("interfaceVersion"));
	if (version != 0) {
		std::printf("NG: interfaceVersion が %u（0 のはず）\n", version);
		bad++;
	}

	// NSSize is two doubles
	struct ns_size { double width, height; };
	const ns_size want{ 1400.0, 360.0 };
	id view = ((id (*)(id, SEL, AudioUnit, ns_size))objc_msgSend)(
	    factory, sel_registerName("uiViewForAudioUnit:withSize:"), u, want);
	if (!view) {
		std::printf("NG: エディタが画面を作れない\n");
		objc_autoreleasePoolPop(pool);
		return bad + 1;
	}

	id subs = ((id (*)(id, SEL))objc_msgSend)(view, sel_registerName("subviews"));
	const unsigned long panels = subs ? ((unsigned long (*)(id, SEL))objc_msgSend)(
	    subs, sel_registerName("count")) : 0;
	if (panels < 1) {
		std::printf("NG: エディタの中にパネルが無い\n");
		bad++;
	}
	std::printf("OK: エディタ %s が画面を作った（下位ビュー %lu 枚）\n",
	            object_getClassName(view), panels);

	objc_autoreleasePoolPop(pool);
	return bad;
}


int run_torture(AudioComponent comp, const std::string &bundle_path)
{
	std::printf("\n---- 乱暴に扱ってみる ----\n");
	int bad = 0;

	// 1. create and dispose without ever initialising
	for (int i = 0; i < 8; i++) {
		AudioUnit u = nullptr;
		if (AudioComponentInstanceNew(comp, &u) != noErr || !u) {
			std::printf("NG: 作れない\n");
			return 1;
		}
		AudioComponentInstanceDispose(u);
	}
	std::printf("OK: 初期化せずに 8 個作って捨てた\n");

	// 2. initialize / uninitialize round trips
	{
		AudioUnit u = nullptr;
		AudioComponentInstanceNew(comp, &u);
		for (int i = 0; i < 3; i++) {
			AudioUnitInitialize(u);
			AudioUnitUninitialize(u);
		}
		AudioComponentInstanceDispose(u);
		std::printf("OK: initialize/uninitialize を 3 往復\n");
	}

	// 3. every sample rate and block length, rendering without listening
	{
		const double rates[] = { 22050, 32000, 44100, 48000, 96000, 192000 };
		std::vector<float> l(1024), r(1024);
		for (double rate : rates) {
			AudioUnit u = nullptr;
			AudioComponentInstanceNew(comp, &u);
			const AudioStreamBasicDescription fmt = float_format(rate, 2);
			const OSStatus sf = AudioUnitSetProperty(u, kAudioUnitProperty_StreamFormat,
			                                         kAudioUnitScope_Output, 0, &fmt, sizeof(fmt));
			if (sf != noErr) {
				std::printf("NG: %.0f Hz を受けない\n", rate);
				bad++;
			}
			const UInt32 maxf = 1024;
			AudioUnitSetProperty(u, kAudioUnitProperty_MaximumFramesPerSlice,
			                     kAudioUnitScope_Global, 0, &maxf, sizeof(maxf));
			AudioUnitInitialize(u);

			audio_buffers_2 bufs{};
			bufs.list.mNumberBuffers = 2;
			for (int i = 0; i < 2; i++) {
				bufs.list.mBuffers[i].mNumberChannels = 1;
				bufs.list.mBuffers[i].mDataByteSize = 1024 * 4;
				bufs.list.mBuffers[i].mData = i == 0 ? (void *)l.data() : (void *)r.data();
			}
			AudioTimeStamp ts{};
			ts.mFlags = kAudioTimeStampSampleTimeValid;

			// a zero-length render, then ordinary ones
			AudioUnitRenderActionFlags flags = 0;
			AudioUnitRender(u, &flags, &ts, 0, 0, &bufs.list);
			for (int blk = 0; blk < 8; blk++) {
				flags = 0;
				const OSStatus st = AudioUnitRender(u, &flags, &ts, 0, 256, &bufs.list);
				if (st != noErr) {
					std::printf("NG: %.0f Hz の Render が %d\n", rate, int(st));
					bad++;
					break;
				}
				ts.mSampleTime += 256;
			}
			AudioUnitUninitialize(u);
			AudioComponentInstanceDispose(u);
		}
		std::printf("OK: %.0f から 192000 まで setup と Render\n", rates[0]);
	}

	// 4. ask which properties answer, and how
	{
		AudioUnit u = nullptr;
		AudioComponentInstanceNew(comp, &u);
		// Asked with the scope included: the same number in another scope is a
		// different question
		struct prop_test { AudioUnitPropertyID id; AudioUnitScope scope; };
		const prop_test props[] = {
			{ kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global },
			{ kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global },
			{ kAudioUnitProperty_Latency, kAudioUnitScope_Global },
			{ kAudioUnitProperty_ParameterList, kAudioUnitScope_Global },
			{ kAudioUnitProperty_ParameterInfo, kAudioUnitScope_Global },
			// Global, not Output. Apple's header says Global for this one, and
			// DLSMusicDevice -- Apple's own aumu -- refuses it for both Input and
			// Output. Asking for it in Output made this test fail a unit that
			// answers exactly the way the reference does
			{ kAudioUnitProperty_SupportedNumChannels, kAudioUnitScope_Global },
			{ kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output },
			{ kAudioUnitProperty_ElementCount, kAudioUnitScope_Input },
			// The input bus is the A/D INPUT, and this unit has one (the VST3 build
			// has the same bus), so its format is answered rather than refused
			{ kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input },
		};
		for (const prop_test &p : props) {
			UInt32 size = 0;
			Boolean writable = false;
			const OSStatus st = AudioUnitGetPropertyInfo(u, p.id, p.scope, 0, &size, &writable);
			if (st != noErr) {
				std::printf("NG: プロパティ %u（スコープ %u）に答えない\n",
				            unsigned(p.id), unsigned(p.scope));
				bad++;
			}
		}

		// The other half of the same question. A property that answers in every
		// scope gets read as if the value belonged to that scope, so refusing the
		// wrong ones matters as much as answering the right ones. Each pair below
		// is one DLSMusicDevice also refuses -- checked against it, not assumed
		const prop_test wrong_scope[] = {
			{ kAudioUnitProperty_SupportedNumChannels,  kAudioUnitScope_Output },
			{ kAudioUnitProperty_Latency,               kAudioUnitScope_Output },
			{ kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Output },
			{ kAudioUnitProperty_SetRenderCallback,     kAudioUnitScope_Output },
			{ kAudioUnitProperty_ClassInfo,             kAudioUnitScope_Output },
		};
		for (const prop_test &p : wrong_scope) {
			UInt32 size = 0;
			if (AudioUnitGetPropertyInfo(u, p.id, p.scope, 0, &size, nullptr) == noErr) {
				std::printf("NG: プロパティ %u にスコープ %u で答えてしまう\n",
				            unsigned(p.id), unsigned(p.scope));
				bad++;
			}
		}

		// An unknown number must be answered with "don't know"
		UInt32 size = 0;
		if (AudioUnitGetPropertyInfo(u, 0x7fff, kAudioUnitScope_Global, 0, &size, nullptr) == noErr) {
			std::printf("NG: 知らないプロパティに noErr を返した\n");
			bad++;
		}
		std::printf("OK: プロパティの受け答え\n");
		AudioComponentInstanceDispose(u);
	}

	// 5. saving the state and reading it back
	{
		AudioUnit u = nullptr;
		AudioComponentInstanceNew(comp, &u);
		AudioUnitInitialize(u);

		// There is nothing inside until boot finishes. As vst3probe does, wait
		// until the blob has grown
		CFPropertyListRef plist = nullptr;
		CFIndex blob_size = 0;
		for (int t = 0; t < 300; t++) {
			if (plist)
				CFRelease(plist);
			plist = nullptr;
			UInt32 size = sizeof(plist);
			if (AudioUnitGetProperty(u, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0,
			                         &plist, &size) != noErr || !plist)
				break;
			CFDataRef d = static_cast<CFDataRef>(const_cast<void *>(
				CFDictionaryGetValue(static_cast<CFDictionaryRef>(plist), CFSTR("S-MU2000"))));
			blob_size = d ? CFDataGetLength(d) : 0;
			if (blob_size >= 1000)
				break;
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}

		if (!plist) {
			std::printf("NG: ClassInfo を読めない\n");
			bad++;
		} else if (blob_size < 1000) {
			std::printf("NG: ClassInfo が %ld バイトしかない（機械の中身が入っていない）\n",
			            long(blob_size));
			bad++;
		} else {
			UInt32 size = sizeof(plist);
			if (AudioUnitSetProperty(u, kAudioUnitProperty_ClassInfo, kAudioUnitScope_Global, 0,
			                         &plist, size) != noErr) {
				std::printf("NG: ClassInfo を書き戻せない\n");
				bad++;
			} else {
				std::printf("OK: 状態を %ld バイトで保存して読み戻した\n", long(blob_size));
			}
		}
		if (plist)
			CFRelease(plist);
		AudioUnitUninitialize(u);
		AudioComponentInstanceDispose(u);
	}

	// 6. four at once
	{
		const int N = 4;
		AudioUnit us[N] = {};
		for (int i = 0; i < N; i++)
			AudioComponentInstanceNew(comp, &us[i]);

		std::vector<float> l(512), r(512);
		for (int i = 0; i < N; i++) {
			const AudioStreamBasicDescription fmt = float_format(48000.0, 2);
			AudioUnitSetProperty(us[i], kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output,
			                     0, &fmt, sizeof(fmt));
			AudioUnitInitialize(us[i]);
		}
		std::printf("4 枚ぶん起動を待つ...\n");
		std::this_thread::sleep_for(std::chrono::milliseconds(9000));

		audio_buffers_2 bufs{};
		bufs.list.mNumberBuffers = 2;
		for (int i = 0; i < 2; i++) {
			bufs.list.mBuffers[i].mNumberChannels = 1;
			bufs.list.mBuffers[i].mDataByteSize = 512 * 4;
			bufs.list.mBuffers[i].mData = i == 0 ? (void *)l.data() : (void *)r.data();
		}
		AudioTimeStamp ts{};
		ts.mFlags = kAudioTimeStampSampleTimeValid;

		const long long t0 = now_ms();
		double peak = 0.0;
		for (int blk = 0; blk < 200; blk++)
			for (int i = 0; i < N; i++) {
				AudioUnitRenderActionFlags flags = 0;
				AudioUnitRender(us[i], &flags, &ts, 0, 512, &bufs.list);
				for (float v : l) peak = std::max(peak, std::fabs(double(v)));
			}
		const long long t1 = now_ms();
		const double audio = 200.0 * 512.0 / 48000.0;
		std::printf(" 4 枚同時に %.2f 秒ぶん作って実時間 %.2f 秒（1 枚あたり CPU %.0f%%）\n",
		            audio, (t1 - t0) / 1000.0, 100.0 * (t1 - t0) / 1000.0 / audio / 4.0);

		for (int i = 0; i < N; i++) {
			AudioUnitUninitialize(us[i]);
			AudioComponentInstanceDispose(us[i]);
		}
		std::printf("OK: 4 枚同時に作って捨てた\n");
	}

	// 7. the editor a host would put in its window
	{
		AudioUnit u = nullptr;
		AudioComponentInstanceNew(comp, &u);
		bad += check_editor(u, bundle_path);
		AudioComponentInstanceDispose(u);
	}

	std::printf("---- 悪いところ %d 件 ----\n", bad);
	return bad ? 1 : 0;
}

} // namespace


int main(int argc, char **argv)
{
	smu2000::init_console_utf8();

	if (argc < 2) {
		std::fprintf(stderr,
			"使い方: aubprobe <バンドル|-> [<MIDI> [<出力 wav>]] [--rate 48000] [--block 512]\n"
			"                                 [--tail 3] [--torture] [--list]\n");
		return 1;
	}

	std::string bundle = argv[1], mid, wav;
	double rate = 48000.0;
	int block = 512;
	double extra = 3.0;
	bool torture = false, list = false;
	for (int i = 2; i < argc; i++) {
		if (!std::strcmp(argv[i], "--rate") && i + 1 < argc) rate = std::atof(argv[++i]);
		else if (!std::strcmp(argv[i], "--block") && i + 1 < argc) block = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--tail") && i + 1 < argc) extra = std::atof(argv[++i]);
		else if (!std::strcmp(argv[i], "--torture")) torture = true;
		else if (!std::strcmp(argv[i], "--list")) list = true;
		else if (mid.empty()) mid = argv[i];
		else if (wav.empty()) wav = argv[i];
	}

	std::string err;
	AudioComponent comp = find_component(bundle, err);
	if (!comp) {
		std::fprintf(stderr, "%s\n", err.empty() ? "AU が見つからない" : err.c_str());
		return 1;
	}

	AudioComponentDescription got{};
	AudioComponentGetDescription(comp, &got);
	CFStringRef cname = nullptr;
	AudioComponentCopyName(comp, &cname);
	char name[256] = {};
	if (cname) {
		CFStringGetCString(cname, name, sizeof(name), kCFStringEncodingUTF8);
		CFRelease(cname);
	}
	std::printf("見つけた: %s\n", name);
	std::printf("  種別 %c%c%c%c / 番号 %c%c%c%c / 作り手 %c%c%c%c\n",
	            char(got.componentType >> 24), char(got.componentType >> 16),
	            char(got.componentType >> 8), char(got.componentType),
	            char(got.componentSubType >> 24), char(got.componentSubType >> 16),
	            char(got.componentSubType >> 8), char(got.componentSubType),
	            char(got.componentManufacturer >> 24), char(got.componentManufacturer >> 16),
	            char(got.componentManufacturer >> 8), char(got.componentManufacturer));

	{
		AudioUnit u = nullptr;
		if (AudioComponentInstanceNew(comp, &u) != noErr || !u) {
			std::fprintf(stderr, "開けない\n");
			return 1;
		}
		std::printf("開けた\n");
		if (list)
			show_info(u);
		AudioComponentInstanceDispose(u);
	}

	int rc = 0;
	if (!mid.empty() && !wav.empty()) {
		AudioUnit u = nullptr;
		if (AudioComponentInstanceNew(comp, &u) != noErr) {
			std::fprintf(stderr, "開けない\n");
			return 1;
		}
		rc = run_render(u, rate, block, extra, mid, wav);
		AudioComponentInstanceDispose(u);
	} else if (!torture && !list) {
		std::printf("音は出していない（MIDI と出力先を渡すと鳴らす）\n");
	}

	if (torture && run_torture(comp, bundle))
		rc = 1;
	return rc;
}
