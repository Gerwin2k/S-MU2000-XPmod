// license:BSD-3-Clause
//
// The synth side of a front end: load the ROMs, boot, and render blocks on
// demand from the audio device.
//
// This used to live inside gui.cpp. It moved here when the macOS front end
// arrived, because the part worth sharing is not the boilerplate but the
// routing in fill(): which MIDI port feeds which part, and what gets echoed
// back out. Two copies of that is two things that can drift, and a drift there
// would show up as the two platforms sounding different.
//
// Nothing in here touches a window or an OS API, so it is the same on both
// platforms. See doc/porting-macos.md.

#ifndef S_MU2000_UI_ENGINE_H
#define S_MU2000_UI_ENGINE_H

#pragma once

#include "audio_out.h"
#include "bridge.h"
#include "driver.h"
#include "midi_in.h"
#include "midi_out.h"
#include "mu2000.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

namespace ui {

struct engine {
	mu2000 mu;
	bridge   &br;
	midi_in  &midi;        // MIDI IN A（パート 1-16）
	midi_in  *midi_b = nullptr;   // MIDI IN B（パート 17-32）
	midi_out *mout = nullptr;     // MIDI OUT A（A で受けたものを外へ）
	midi_out *mout_b = nullptr;   // MIDI OUT B（B で受けたものを外へ）

	std::atomic<int> state{0};        // 0 起動中 / 1 準備完了 / 2 だめ
	std::string      message = "起動中...";

	driver drv;

	engine(bridge &b, midi_in &m) : br(b), midi(m) {}

	bool load(const std::string &dir)
	{
		if (!mu.load_program(dir + "/mu2000_flash.bin")) { message = mu.error(); return false; }
		if (!mu.load_wave(dir + "/dump"))                { message = mu.error(); return false; }
		if (!mu.load_sintab(dir + "/standin/sin-table.bin"))
			std::fprintf(stderr, "警告: %s\n", mu.error().c_str());
		if (!mu.load_lcd_font(dir + "/hd44780u_b04.bin") &&
		    !mu.load_lcd_font(dir + "/standin/hd44780u_b04.bin"))
			std::fprintf(stderr, "警告: %s\n", mu.error().c_str());
		return true;
	}

	// 起動（実機と同じ空回し）。窓を出したあと別スレッドで進める
	bool boot()
	{
		mu.set_threaded(true);
		mu.reset();
		const size_t limit = size_t(30.0 * AUDIO_RATE);
		size_t i = 0;
		s32 l, r;
		for (; i < limit && !mu.midi_ready(); i++)
			mu.run_sample(l, r);
		if (i >= limit) {
			message = "起動しなかった";
			return false;
		}
		publish();
		return true;
	}

	void publish()
	{
		if (state.load() == 1)
			driver::publish_now(mu, br, true, nullptr);
		else
			driver::publish_message(br, message.c_str());
	}

	// 音声デバイスに頼まれた分だけ進める
	void fill(s16 *out, u32 n)
	{
		if (state.load() != 1) {
			std::memset(out, 0, size_t(n) * 4);
			return;
		}

		drv.apply_buttons(mu, br);
		// 画面から出したものも、外の MIDI 出力へ流す（実機の THRU）
		drv.pump_midi(mu, br, [this](u8 v) { if (mout) mout->send(v); });
		drv.pump_wheel(mu, br);

		u8 b;
		while (midi.pop(b)) {
			mu.midi_in(b, 0);
			if (mout) mout->send(b);
		}
		// B は実機の 2 つめの DIN（内蔵 SCI ch1）。パート 17-32 に届く。
		// THRU も口ごとに分ける。A で受けたものは MIDI OUT A、
		// B で受けたものは MIDI OUT B へ。混ぜると、外に繋いだ音源で
		// パートの割り振りが崩れる
		if (midi_b)
			while (midi_b->pop(b)) {
				mu.midi_in(b, 1);
				if (mout_b) mout_b->send(b);
			}

		const float g = br.gain();

		for (u32 i = 0; i < n; i++) {
			s32 l = 0, r = 0;
			mu.run_sample(l, r);
			l = s32(l * g) * 32768 / mu2000::DAC_FULL_SCALE;
			r = s32(r * g) * 32768 / mu2000::DAC_FULL_SCALE;
			out[i * 2 + 0] = s16(l < -32768 ? -32768 : l > 32767 ? 32767 : l);
			out[i * 2 + 1] = s16(r < -32768 ? -32768 : r > 32767 ? 32767 : r);
		}

		drv.publish(mu, br, n, AUDIO_RATE, true, nullptr);
	}
};

} // namespace ui

#endif // S_MU2000_UI_ENGINE_H
