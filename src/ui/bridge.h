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

#include "panel.h"

#include <atomic>
#include <cstring>

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
	std::atomic<u64>      m_buttons{0};
	std::atomic<int>      m_wheel{0};
	std::atomic<float>    m_gain{1.0f};
	std::atomic<unsigned> m_seq{0};
	snapshot              m_snap;
};

} // namespace ui

#endif // S_MU2000_UI_BRIDGE_H
