// license:BSD-3-Clause
//
// 画面と音源のあいだ。触れ合うのはこの 2 本だけ。
//
//   ボタン   画面 → 音源。押している間 1 のビット
//   写し     音源 → 画面。LCD の点と LED
//
// 音源は音声スレッドが回しているので、画面から直接触ってはいけない。
// 写しは seqlock で渡す（読み手は待たない。途中の絵を読んだら読み直す）。

#ifndef S_MU2000_UI_BRIDGE_H
#define S_MU2000_UI_BRIDGE_H

#pragma once

#include "snapshot.h"
#include "mu2000.h"

#include <atomic>
#include <cstring>
#include <mutex>

namespace ui {

class bridge
{
public:
	// ---- 画面から

	void press(mu2000::button b, bool down)
	{
		const u64 bit = u64(1) << int(b);
		u64 cur = m_buttons.load(std::memory_order_relaxed), next;
		do {
			next = down ? (cur | bit) : (cur & ~bit);
		} while (!m_buttons.compare_exchange_weak(cur, next, std::memory_order_relaxed));
	}

	void release_all() { m_buttons.store(0, std::memory_order_relaxed); }

	// ダイヤルを回した分。音源側が 1 つずつ VALUE を叩いて消化する
	void turn(int steps) { m_wheel.fetch_add(steps, std::memory_order_relaxed); }

	// 画面から音源へ MIDI を送る（エディタのつまみ、MIDI ファイルの再生）。輪に積むだけ。
	//
	// **1 回に 1 通以上の完成したメッセージを渡すこと。** 書き終えてから 1 回で
	// 書き込み位置を進めるので、音源側からは途中までのメッセージが見えない。
	// 前は 1 バイトずつ進めていたので、音声の糸がブロックの境目で前半だけ読み、
	// 次のブロックで別の口のメッセージがその途中に挟まることがあった。
	//
	// 書き手は 2 本ある（画面の糸と、MIDI ファイルを流す糸）。輪は書き手 1 本が
	// 前提なので、**書き手どうしは錠で順番にする**。どちらも音声の糸ではないので
	// 待ってよい。読み手（音声の糸）は錠に触らない。
	// 入りきらなければ丸ごと捨てて false
	bool send(const u8 *bytes, size_t n)
	{
		std::lock_guard<std::mutex> lock(m_send_lock);
		const size_t w = m_mw.load(std::memory_order_relaxed);
		const size_t r = m_mr.load(std::memory_order_acquire);
		const size_t room = (r - w - 1) & MIDI_MASK;
		if (n > room)
			return false;
		for (size_t i = 0; i < n; i++)
			m_midi[(w + i) & MIDI_MASK] = bytes[i];
		m_mw.store((w + n) & MIDI_MASK, std::memory_order_release);
		return true;
	}

	void set_gain(float g) { m_gain.store(g, std::memory_order_relaxed); }
	float gain() const     { return m_gain.load(std::memory_order_relaxed); }

	void read(snapshot &out) const
	{
		for (;;) {
			const unsigned a = m_seq.load(std::memory_order_acquire);
			if (a & 1)
				continue;                       // 書いている最中
			std::memcpy(&out, &m_snap, sizeof(out));
			if (m_seq.load(std::memory_order_acquire) == a)
				return;
		}
	}

	// ---- 音源から

	u64 buttons() const { return m_buttons.load(std::memory_order_relaxed); }

	// 音源側から。溜まっている MIDI を 1 バイトずつ
	bool take_midi(u8 &v)
	{
		const size_t r = m_mr.load(std::memory_order_relaxed);
		if (r == m_mw.load(std::memory_order_acquire))
			return false;
		v = m_midi[r];
		m_mr.store((r + 1) & MIDI_MASK, std::memory_order_release);
		return true;
	}

	int take_turn()
	{
		int v = m_wheel.load(std::memory_order_relaxed);
		if (!v)
			return 0;
		const int step = (v > 0) ? 1 : -1;
		m_wheel.fetch_sub(step, std::memory_order_relaxed);
		return step;
	}

	void publish(const snapshot &s)
	{
		m_seq.fetch_add(1, std::memory_order_release);
		std::memcpy(&m_snap, &s, sizeof(m_snap));
		m_seq.fetch_add(1, std::memory_order_release);
	}

private:
	static constexpr size_t MIDI_SIZE = 4096, MIDI_MASK = MIDI_SIZE - 1;
	u8 m_midi[MIDI_SIZE] = {};
	std::atomic<size_t>   m_mr{0}, m_mw{0};
	std::mutex            m_send_lock;    // 書き手どうしだけが使う
	std::atomic<u64>      m_buttons{0};
	std::atomic<int>      m_wheel{0};
	std::atomic<float>    m_gain{1.0f};
	std::atomic<unsigned> m_seq{0};
	snapshot              m_snap;
};

} // namespace ui

#endif // S_MU2000_UI_BRIDGE_H
