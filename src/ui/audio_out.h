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
#include <thread>

namespace ui {

constexpr u32 AUDIO_RATE = 44100;

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

} // namespace ui

#endif // S_MU2000_UI_AUDIO_OUT_H
