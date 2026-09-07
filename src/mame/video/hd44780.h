// license:BSD-3-Clause
// copyright-holders:Sandro Ronco
//
// Hitachi HD44780 LCD コントローラ。
// MAME の src/devices/video/hd44780.* から、firmware が触る部分だけを取った。
//
// 画面を出す部分（CGROM のフォント、ピクセル生成、SVG のレイアウト）は入れて
// いない。音を出すのに要らないため。ただし**ビジーフラグは要る**。
// MU2000 の firmware は LCD にコマンドを送るたびにビジーが立つのを見ており、
// 常に「空いている」と返すと初期化の途中で先へ進まなくなる。
//
// MAME はビジーの計測に emu_timer を使っていたが、こちらは CPU のサイクル数で
// 数える。LCD の発振は 270kHz、命令は 10 サイクル（37us）、クリアと
// ホームだけ 410 サイクル（1.52ms）。

#ifndef S_MU2000_HD44780_H
#define S_MU2000_HD44780_H

#pragma once

#include "../../compat/mamecompat.h"

class hd44780_device
{
public:
	// cpu_hz: ビジーの残り時間を数えるための CPU 側の周波数
	hd44780_device(u32 cpu_hz = 28000000, u32 lcd_hz = 270000)
		: m_cpu_hz(cpu_hz), m_lcd_hz(lcd_hz) {}

	void reset();

	// 現在の CPU サイクル。読み書きの前に入れておく
	void set_now(u64 cycles) { m_now = cycles; }

	void control_w(u8 data);
	u8   control_r() const;
	void data_w(u8 data);
	u8   data_r();

	bool busy() const { return m_now < m_busy_until; }

	// 表示内容。4 行 20 桁ぶんを取り出す（UI を作るときに使う）
	const u8 *ddram() const { return m_ddram; }

private:
	void set_busy(u16 lcd_cycles)
	{
		m_busy_until = m_now + u64(lcd_cycles) * m_cpu_hz / m_lcd_hz;
	}
	void correct_ac();
	void update_ac(int direction);
	void shift_display(int direction);

	enum { DDRAM, CGRAM };

	u32 m_cpu_hz, m_lcd_hz;
	u64 m_now = 0, m_busy_until = 0;

	u8  m_ddram[0x80] = {};
	u8  m_cgram[0x40] = {};
	int m_ac = 0;
	int m_active_ram = DDRAM;
	int m_direction = 1;
	int m_disp_shift = 0;
	int m_num_line = 1;
	int m_char_size = 8;
	int m_data_len = 8;
	bool m_shift_on = false;
	bool m_display_on = false, m_cursor_on = false, m_blink_on = false;
	bool m_nibble = false;      // 4bit 接続のときの上位/下位
	u8  m_ir = 0, m_dr = 0;
};

#endif // S_MU2000_HD44780_H
