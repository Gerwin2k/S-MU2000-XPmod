// license:BSD-3-Clause
//
// WASAPI 共有モードの音声出力を、自分のスレッドで回す。
//
// live.exe で詰めた形をそのまま切り出したもの。要点は
// **時計を自分で持たないこと**。デバイスが「N サンプルくれ」と言った分だけ
// fill を呼ぶ。GUI がある側ではメッセージループを止められないので、
// ここはスレッドに分かれている必要がある。
//
// **標本化周波数の変換は自分でやる。** デバイスは 48000Hz の float を
// 言ってくることが多いが、MU2000 は 44100Hz より他では動かない。変換を
// Windows に任せると既定の品質の変換器を通されるので、窓関数付き sinc
// （ui::resampler）で自分で変換してから渡す。

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
	// 16bit 2ch インタリーブで frames サンプルぶん書く（44100Hz）
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
	// 動いているか。デバイスが黙った等で落ちたら偽になる
	bool running() const      { return m_running.load(); }
	// 落ちた理由（running() が偽のとき）
	std::string error() const { return m_err; }
	double cpu_percent() const;
	double worst_ms() const;

	// デバイスが言ってきた形式。開いた後に読む
	u32 device_rate() const     { return m_dev_rate.load(); }
	u32 device_channels() const { return m_dev_channels.load(); }
	// 自分で標本化周波数を変換しているか（デバイスが 44100 なら要らない）
	bool converting() const     { return m_converting.load(); }
	// デバイスの周期（ミリ秒）。溜めの下限を決めるもの
	double period_ms() const    { return m_period_ms.load(); }
	// 溜めの長さ（ミリ秒）。これが待ち時間の本体
	double buffer_ms() const;
	// 人が読む 1 行
	std::string format_line() const;

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
	// 立ち上がりの首尾。**メンバに置くこと。** スレッドは run() を抜けた後に
	// ここへ書くので、start() のローカルに置くと宙ぶらりんの参照になる
	std::atomic<int>  m_start_state{0};   // 0 待ち / 1 動いた / 2 だめ
	std::atomic<u32> m_dev_rate{AUDIO_RATE}, m_dev_channels{2};
	std::atomic<bool> m_converting{false};
	std::atomic<double> m_period_ms{0.0};
	std::atomic<int>  m_dev_bits{16};
	std::atomic<bool> m_dev_float{false};
	s64 m_qpc_freq = 1;
};

} // namespace ui

#endif // S_MU2000_UI_AUDIO_OUT_H
