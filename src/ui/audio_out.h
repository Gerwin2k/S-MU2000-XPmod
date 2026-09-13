// license:BSD-3-Clause
//
// WASAPI 共有モードの音声出力を、自分のスレッドで回す。
//
// live.exe で詰めた形をそのまま切り出したもの。要点は
// **時計を自分で持たないこと**。デバイスが「N サンプルくれ」と言った分だけ
// fill を呼ぶ。GUI がある側ではメッセージループを止められないので、
// ここはスレッドに分かれている必要がある。

#ifndef S_MU2000_UI_AUDIO_OUT_H
#define S_MU2000_UI_AUDIO_OUT_H

#pragma once

#include "compat/mamecompat.h"

#include <atomic>
#include <functional>
#include <string>

#if defined(__APPLE__)
#include <memory>
#else
#include <thread>
#endif

namespace ui {

constexpr u32 AUDIO_RATE = 44100;

#if defined(__APPLE__)

// macOS: a CoreAudio DefaultOutput AudioUnit calls the render callback on its
// own real-time HAL thread, so unlike the Windows side there is no worker thread
// here. The public shape is identical, so live and the GUI do not know which
// implementation they are talking to.
class audio_out
{
public:
	// 16bit 2ch インタリーブで frames サンプルぶん書く
	using fill_fn = std::function<void(s16 *out, u32 frames)>;

	// Both of these are declared here and defined in the .cpp. With a pimpl that
	// is not optional: the compiler otherwise generates them here, where impl is
	// still incomplete, and unique_ptr refuses to delete an incomplete type
	audio_out();
	~audio_out();

	bool start(int latency_ms, fill_fn fill, std::string &err);
	void stop();

	// Progress. Written on the audio thread, safe to read from anywhere.
	u32 buffer_frames() const;
	u64 produced() const;
	u64 starved() const;
	// The CoreAudio render callback already runs at real-time priority, so this
	// is the counterpart of registering with MMCSS on Windows
	bool mmcss() const;
	double cpu_percent() const;
	double worst_ms() const;

private:
	struct impl;
	std::unique_ptr<impl> m_impl;
};

#else
class audio_out
{
public:
	// 16bit 2ch インタリーブで frames サンプルぶん書く
	using fill_fn = std::function<void(s16 *out, u32 frames)>;

	~audio_out() { stop(); }

	bool start(int latency_ms, fill_fn fill, std::string &err);
	void stop();

	// 具合。すべて音声スレッドが書き、他所から読んでよい
	u32 buffer_frames() const { return m_buffer_frames.load(); }
	u64 produced() const      { return m_produced.load(); }
	u64 starved() const       { return m_starved.load(); }
	// MMCSS（Pro Audio）に登録できたか。だめだと途切れやすくなる
	bool mmcss() const        { return m_mmcss.load(); }
	double cpu_percent() const;
	double worst_ms() const;

private:
	void run(int latency_ms);

	fill_fn           m_fill;
	std::thread       m_thread;
	std::atomic<bool> m_quit{false};
	std::atomic<bool> m_running{false};
	std::string       m_err;

	std::atomic<u32> m_buffer_frames{0};
	std::atomic<u64> m_produced{0}, m_starved{0};
	std::atomic<u64> m_busy_ticks{0}, m_worst_ticks{0};
	std::atomic<bool> m_mmcss{false};
	s64 m_qpc_freq = 1;
};

#endif // __APPLE__

} // namespace ui

#endif // S_MU2000_UI_AUDIO_OUT_H
