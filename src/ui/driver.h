// license:BSD-3-Clause
//
// パネルの操作を音源に反映し、画面へ写しを返す。音声スレッドから呼ぶ。
// exe（gui.exe）と VST3 の両方が同じものを使うので、押し方も見え方も揃う。

#ifndef S_MU2000_UI_DRIVER_H
#define S_MU2000_UI_DRIVER_H

#pragma once

#include "bridge.h"
#include "mu2000.h"

#include <cstdio>

namespace ui {

class driver
{
public:
	// 1 ブロックの頭で。画面から押されているボタンを音源へ
	void apply_buttons(mu2000 &mu, const bridge &br)
	{
		const u64 want = br.buttons();
		if (want == m_applied)
			return;
		for (int i = 0; i < int(mu2000::button::count); i++)
			if (((want ^ m_applied) >> i) & 1)
				mu.set_button(mu2000::button(i), ((want >> i) & 1) != 0);
		m_applied = want;
	}

	// 1 サンプルごとに。ホイールで回された分を VALUE の叩きに崩す。
	// 押し 30ms、離し 20ms。実機を指で連打するのと同じ速さ
	void tick_wheel(mu2000 &mu, bridge &br, u32 rate)
	{
		if (m_tap_left > 0) {
			if (--m_tap_left == 0) {
				mu.set_button(m_tap_button, false);
				m_gap_left = int(0.020 * rate);
			}
			return;
		}
		if (m_gap_left > 0) {
			--m_gap_left;
			return;
		}
		const int step = br.take_turn();
		if (!step)
			return;
		m_tap_button = (step > 0) ? mu2000::button::value_plus : mu2000::button::value_minus;
		mu.set_button(m_tap_button, true);
		m_tap_left = int(0.030 * rate);
	}

	// ブロックの終わりで。25ms ごとに LCD と LED を画面へ渡す
	void publish(mu2000 &mu, bridge &br, u32 frames, u32 rate,
	             bool ready, const char *message)
	{
		m_since += frames;
		if (m_since < rate / 40)
			return;
		m_since = 0;
		publish_now(mu, br, ready, message);
	}

	static void publish_now(mu2000 &mu, bridge &br, bool ready, const char *message)
	{
		snapshot s;
		hd44780_device &lcd = mu.lcd();
		const u8 *img = lcd.render();
		const int cols = lcd.line_size();
		for (int row = 0; row < LCD_ROWS; row++)
			for (int col = 0; col < LCD_COLS; col++)
				for (int y = 0; y < CELL_H; y++)
					s.dots[(row * LCD_COLS + col) * CELL_H + y] =
						img[16 * (row * cols + col) + y];
		s.leds   = mu.leds();
		s.lcd_on = lcd.display_on();
		s.ready  = ready;
		if (!ready && message)
			std::snprintf(s.message, sizeof(s.message), "%s", message);
		br.publish(s);
	}

	// 音源が無いとき（起動前、ROM が無い）の写し
	static void publish_message(bridge &br, const char *message)
	{
		snapshot s;
		std::snprintf(s.message, sizeof(s.message), "%s", message ? message : "");
		br.publish(s);
	}

private:
	u64 m_applied = 0;
	u64 m_since = 0;
	int m_tap_left = 0, m_gap_left = 0;
	mu2000::button m_tap_button = mu2000::button::count;
};

} // namespace ui

#endif // S_MU2000_UI_DRIVER_H
