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

	// エディタから送られた MIDI を音源へ。echo には MIDI 出力の口を渡す
	// （実機の THRU と同じで、画面から出したものも外へ出る）
	template <typename F>
	void pump_midi(mu2000 &mu, bridge &br, F &&echo)
	{
		u8 b;
		while (br.take_midi(b)) {
			mu.midi_in(b);
			echo(b);
		}
		// パラメータの層の問い合わせ。外へは流さない
		while (br.take_ask(b))
			mu.midi_in(b);
	}

	void pump_midi(mu2000 &mu, bridge &br)
	{
		pump_midi(mu, br, [](u8) {});
	}

	// ブロックの終わりで。音源が MIDI OUT から送り出したものを画面へ渡す。
	// echo には外の MIDI OUT の口を渡す
	template <typename F>
	void pump_out(mu2000 &mu, bridge &br, F &&echo)
	{
		u8 b;
		while (mu.midi_out_take(b)) {
			br.put_out(b);
			echo(b);
		}
	}

	void pump_out(mu2000 &mu, bridge &br)
	{
		pump_out(mu, br, [](u8) {});
	}

	// ホイールで回された分をダイヤルへ。実機と同じロータリーエンコーダなので、
	// 目盛りを渡すだけでよい（位相は音源が自分で進める）
	void pump_wheel(mu2000 &mu, bridge &br)
	{
		for (int step = br.take_turn(); step; step = br.take_turn())
			mu.turn_encoder(step);
	}

	// ブロックの終わりで。25ms ごとに LCD と LED を画面へ渡す
	void publish(mu2000 &mu, bridge &br, u32 frames, u32 rate,
	             bool ready, const char *message)
	{
		br.advance_clock(frames, rate);
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
};

} // namespace ui

#endif // S_MU2000_UI_DRIVER_H
