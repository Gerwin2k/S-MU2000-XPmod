// license:BSD-3-Clause
//
// CoreAudio output for macOS. Same interface as audio_out.cpp (WASAPI).
//
// The rule from doc/design.md carries over unchanged: **we own no clock**.
// CoreAudio asks for N frames and we make exactly those N. An output AudioUnit
// expresses that directly, so this file is mostly plumbing. There is no worker
// thread here because CoreAudio already calls us on its real-time HAL thread.

#include "audio_out.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <mach/mach_time.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace ui {

namespace {

// mach_absolute_time ticks to seconds. 24MHz on Apple silicon, but read the
// timebase rather than assuming anything
double ticks_per_sec()
{
	mach_timebase_info_data_t tb;
	mach_timebase_info(&tb);
	return 1e9 * double(tb.denom) / double(tb.numer);
}

AudioDeviceID default_output_device()
{
	// AudioObjectPropertyAddress is { selector, scope, element } in that order
	AudioObjectPropertyAddress addr = {
		kAudioHardwarePropertyDefaultOutputDevice,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMain
	};
	AudioDeviceID dev = kAudioObjectUnknown;
	UInt32 size = sizeof(dev);
	if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr,
	                               &size, &dev) != noErr)
		return kAudioObjectUnknown;
	return dev;
}

// Does this device have anything to play through? The device list also holds
// input-only devices, and offering those as an output would be a lie
bool has_output(AudioDeviceID dev)
{
	AudioObjectPropertyAddress addr = {
		kAudioDevicePropertyStreams,
		kAudioObjectPropertyScopeOutput,
		kAudioObjectPropertyElementMain
	};
	UInt32 size = 0;
	if (AudioObjectGetPropertyDataSize(dev, &addr, 0, nullptr, &size) != noErr)
		return false;
	return size >= sizeof(AudioStreamID);
}

// Named rather than called device_name(), which would collide with the member
// function of the same name wherever one is in scope
std::string name_of(AudioDeviceID dev)
{
	AudioObjectPropertyAddress addr = {
		kAudioObjectPropertyName,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMain
	};
	CFStringRef name = nullptr;
	UInt32 size = sizeof(name);
	if (AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &size, &name) != noErr || !name)
		return {};
	char buf[256] = {};
	const bool ok = CFStringGetCString(name, buf, sizeof(buf), kCFStringEncodingUTF8);
	CFRelease(name);
	return ok ? std::string(buf) : std::string();
}

std::vector<AudioDeviceID> output_devices()
{
	AudioObjectPropertyAddress addr = {
		kAudioHardwarePropertyDevices,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMain
	};
	UInt32 size = 0;
	if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &addr, 0, nullptr, &size) != noErr)
		return {};
	std::vector<AudioDeviceID> devs(size / sizeof(AudioDeviceID));
	if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &addr, 0, nullptr, &size,
	                               devs.data()) != noErr)
		return {};
	devs.erase(std::remove_if(devs.begin(), devs.end(),
	                          [](AudioDeviceID d) { return !has_output(d); }),
	           devs.end());
	return devs;
}

// Best effort: ask the device for a buffer matching the requested latency.
// CoreAudio only accepts a value inside the device's range, and it may round, so
// read back what we actually got. Returning 0 means "leave it at the default".
u32 set_device_buffer_frames(AudioDeviceID dev, int latency_ms)
{
	AudioObjectPropertyAddress addr = {
		kAudioDevicePropertyBufferFrameSize,
		kAudioObjectPropertyScopeGlobal,
		kAudioObjectPropertyElementMain
	};

	UInt32 wanted = UInt32((u64(AUDIO_RATE) * u64(latency_ms > 0 ? latency_ms : 0)) / 1000);
	if (wanted < 32)
		wanted = 32;

	AudioValueRange range{};
	UInt32 size = sizeof(range);
	addr.mSelector = kAudioDevicePropertyBufferFrameSizeRange;
	if (AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &size, &range) == noErr) {
		if (wanted < UInt32(range.mMinimum)) wanted = UInt32(range.mMinimum);
		if (wanted > UInt32(range.mMaximum)) wanted = UInt32(range.mMaximum);
	}

	addr.mSelector = kAudioDevicePropertyBufferFrameSize;
	UInt32 got = wanted;
	size = sizeof(got);
	if (AudioObjectSetPropertyData(dev, &addr, 0, nullptr, size, &wanted) != noErr)
		return 0;
	if (AudioObjectGetPropertyData(dev, &addr, 0, nullptr, &size, &got) != noErr)
		return wanted;
	return got;
}

} // namespace

struct audio_out::impl
{
	AudioUnit unit = nullptr;
	fill_fn   fill;

	std::atomic<bool> running{false};
	std::atomic<u32>  buffer_frames{0};
	std::atomic<u64>  produced{0}, starved{0};
	std::atomic<u64>  busy_ticks{0}, worst_ticks{0};
	std::atomic<bool> realtime{false};
	double            tps = 1.0;

	// Scratch for the (unusual) non-interleaved case. Allocated up front so the
	// render callback itself never allocates
	std::vector<s16> scratch;

	// The callback has to be a member: the nested impl is private, so a free
	// function could not name it to cast the refCon
	static OSStatus render_cb(void *ref, AudioUnitRenderActionFlags *, const AudioTimeStamp *,
	                          UInt32, UInt32 frames, AudioBufferList *io)
	{
		auto *self = static_cast<impl *>(ref);
		if (self)
			self->render(frames, io);
		return noErr;
	}

	void render(UInt32 frames, AudioBufferList *io)
	{
		if (!fill)
			return;

		const u64 t0 = mach_absolute_time();

		if (io->mNumberBuffers == 1) {
			// The normal path: one interleaved buffer, exactly what we asked for
			auto *out = static_cast<s16 *>(io->mBuffers[0].mData);
			if (out)
				fill(out, frames);
		} else {
			// Split channels. Make one interleaved block, then fan it out
			if (scratch.size() < size_t(frames) * 2)
				scratch.resize(size_t(frames) * 2);
			fill(scratch.data(), frames);
			for (UInt32 b = 0; b < io->mNumberBuffers && b < 2; b++) {
				auto *dst = static_cast<s16 *>(io->mBuffers[b].mData);
				if (!dst)
					continue;
				for (UInt32 i = 0; i < frames; i++)
					dst[i] = scratch[size_t(i) * 2 + b];
			}
		}

		const u64 took = mach_absolute_time() - t0;
		busy_ticks.fetch_add(took, std::memory_order_relaxed);
		u64 worst = worst_ticks.load(std::memory_order_relaxed);
		while (took > worst &&
		       !worst_ticks.compare_exchange_weak(worst, took, std::memory_order_relaxed)) {
		}
		produced.fetch_add(frames, std::memory_order_relaxed);

		// CoreAudio has no "frames still queued" number the way WASAPI does, so
		// an underrun cannot be read off directly. Use the same proxy live was
		// already tracking: a fill that took longer than the block it was making
		// means the device would have run dry.
		if (double(took) / tps > double(frames) / AUDIO_RATE)
			starved.fetch_add(1, std::memory_order_relaxed);
	}
};

std::vector<std::string> audio_out::list()
{
	std::vector<std::string> names;
	for (AudioDeviceID d : output_devices()) {
		const std::string n = name_of(d);
		if (!n.empty())
			names.push_back(n);
	}
	return names;
}

audio_out::audio_out() = default;

audio_out::~audio_out()
{
	stop();
}

bool audio_out::start(int latency_ms, fill_fn fill, std::string &err)
{
	if (m_impl && m_impl->running.load())
		return true;
	stop();                       // drop any previous attempt

	auto up = std::make_unique<impl>();
	up->tps  = ticks_per_sec();
	up->fill = std::move(fill);

	auto dispose = [&](const char *why) {
		if (up->unit) {
			AudioUnitUninitialize(up->unit);
			AudioComponentInstanceDispose(up->unit);
			up->unit = nullptr;
		}
		err = why;
		return false;
	};

	AudioComponentDescription desc{};
	desc.componentType         = kAudioUnitType_Output;
	desc.componentSubType      = kAudioUnitSubType_DefaultOutput;
	desc.componentManufacturer = kAudioUnitManufacturer_Apple;
	AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
	if (!comp)
		return dispose("既定の音声出力が見つからない");
	if (AudioComponentInstanceNew(comp, &up->unit) != noErr || !up->unit)
		return dispose("AudioUnit を作れない");

	// Ask for the format we generate: 44100Hz, 16bit, stereo, interleaved. The
	// unit converts to whatever the device actually wants
	AudioStreamBasicDescription fmt{};
	fmt.mSampleRate       = AUDIO_RATE;
	fmt.mFormatID         = kAudioFormatLinearPCM;
	fmt.mFormatFlags      = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
	fmt.mFramesPerPacket  = 1;
	fmt.mChannelsPerFrame = 2;
	fmt.mBitsPerChannel   = 16;
	fmt.mBytesPerFrame    = 4;
	fmt.mBytesPerPacket   = 4;
	if (AudioUnitSetProperty(up->unit, kAudioUnitProperty_StreamFormat,
	                         kAudioUnitScope_Input, 0, &fmt, sizeof(fmt)) != noErr)
		return dispose("音声の形式を指定できない");

	AURenderCallbackStruct cb{};
	cb.inputProc       = impl::render_cb;
	cb.inputProcRefCon = up.get();
	if (AudioUnitSetProperty(up->unit, kAudioUnitProperty_SetRenderCallback,
	                         kAudioUnitScope_Input, 0, &cb, sizeof(cb)) != noErr)
		return dispose("音声の呼び出し口を繋げない");

	// Room for the largest slice we might be asked for in one go
	constexpr u32 MAX_SLICE = 4096;
	AudioUnitSetProperty(up->unit, kAudioUnitProperty_MaximumFramesPerSlice,
	                     kAudioUnitScope_Global, 0, &MAX_SLICE, sizeof(MAX_SLICE));
	up->scratch.resize(size_t(MAX_SLICE) * 2);

	// The device buffer size drives the latency. Without this CoreAudio would use
	// its default (often 512 frames, ~11.6ms)
	u32 buf = 0;
	const AudioDeviceID dev = default_output_device();
	if (dev != kAudioObjectUnknown)
		buf = set_device_buffer_frames(dev, latency_ms);
	if (!buf)
		buf = 512;
	up->buffer_frames.store(buf);

	if (AudioUnitInitialize(up->unit) != noErr)
		return dispose("音声を初期化できない");

	up->realtime.store(true);     // the HAL thread is already real-time
	if (AudioOutputUnitStart(up->unit) != noErr)
		return dispose("再生を開始できない");

	up->running.store(true);
	m_impl = std::move(up);
	return true;
}

void audio_out::stop()
{
	if (!m_impl)
		return;
	if (m_impl->unit) {
		// After Stop + Uninitialize no render callback is in flight, so the impl
		// (and the fill function it holds) can be torn down safely
		AudioOutputUnitStop(m_impl->unit);
		AudioUnitUninitialize(m_impl->unit);
		AudioComponentInstanceDispose(m_impl->unit);
		m_impl->unit = nullptr;
	}
	m_impl->running.store(false);
	m_impl.reset();
}

u32 audio_out::buffer_frames() const { return m_impl ? m_impl->buffer_frames.load() : 0; }
u64 audio_out::produced() const      { return m_impl ? m_impl->produced.load() : 0; }
u64 audio_out::starved() const       { return m_impl ? m_impl->starved.load() : 0; }
bool audio_out::mmcss() const        { return m_impl && m_impl->realtime.load(); }

double audio_out::cpu_percent() const
{
	if (!m_impl)
		return 0.0;
	const u64 done = m_impl->produced.load();
	if (!done)
		return 0.0;
	const double audio = double(done) / AUDIO_RATE;
	const double busy  = double(m_impl->busy_ticks.load()) / m_impl->tps;
	return 100.0 * busy / audio;
}

double audio_out::worst_ms() const
{
	if (!m_impl)
		return 0.0;
	return 1000.0 * double(m_impl->worst_ticks.load()) / m_impl->tps;
}

} // namespace ui
