// license:BSD-3-Clause
// copyright-holders:Sandro Ronco
//
// MAME の hd44780.cpp から、レジスタとアドレスカウンタの扱いをそのまま取った。
// 画面生成は入れていない（hd44780.h の説明を参照）。

#include "hd44780.h"

#include <cstring>

void hd44780_device::reset()
{
	std::memset(m_ddram, 0x20, sizeof(m_ddram));
	std::memset(m_cgram, 0, sizeof(m_cgram));
	m_ac = 0;
	m_active_ram = DDRAM;
	m_direction = 1;
	m_disp_shift = 0;
	m_num_line = 1;
	m_char_size = 8;
	m_data_len = 8;
	m_shift_on = false;
	m_display_on = m_cursor_on = m_blink_on = false;
	m_nibble = false;
	m_ir = m_dr = 0;
	m_busy_until = 0;
	set_busy(410);      // 電源投入直後はしばらく忙しい
}

void hd44780_device::correct_ac()
{
	if (m_active_ram == DDRAM) {
		const int max_ac = (m_num_line == 1) ? 0x4f : 0x67;
		if (m_ac > max_ac)
			m_ac -= max_ac + 1;
		else if (m_ac < 0)
			m_ac = max_ac;
		else if (m_num_line == 2 && m_ac > 0x27 && m_ac < 0x40)
			m_ac = 0x40 + (m_ac - 0x28);
	} else
		m_ac &= 0x3f;
}

void hd44780_device::update_ac(int direction)
{
	if (m_active_ram == DDRAM && m_num_line == 2 && direction == -1 && m_ac == 0x40)
		m_ac = 0x27;
	else
		m_ac += direction;
	correct_ac();
}

void hd44780_device::shift_display(int direction)
{
	m_disp_shift += direction;
	if (m_disp_shift == 0x50)
		m_disp_shift = 0;
	else if (m_disp_shift == -1)
		m_disp_shift = 0x4f;
}

void hd44780_device::control_w(u8 data)
{
	if (m_data_len == 4) {
		if (m_nibble) { m_ir = data & 0xf0; return; }
		m_ir |= (data >> 4) & 0x0f;
	} else
		m_ir = data;

	if (BIT(m_ir, 7)) {                 // DDRAM アドレス設定
		m_active_ram = DDRAM;
		m_ac = m_ir & 0x7f;
		correct_ac();
		set_busy(10);
		return;
	}
	if (BIT(m_ir, 6)) {                 // CGRAM アドレス設定
		m_active_ram = CGRAM;
		m_ac = m_ir & 0x3f;
		set_busy(10);
		return;
	}
	if (BIT(m_ir, 5)) {                 // ファンクションセット
		if (BIT(m_ir, 3))
			m_char_size = 8;            // 2 行のときは 5x10 が使えない
		else
			m_char_size = BIT(m_ir, 2) ? 10 : 8;
		m_data_len = BIT(m_ir, 4) ? 8 : 4;
		m_num_line = BIT(m_ir, 3) + 1;
		correct_ac();
		set_busy(10);
		return;
	}
	if (BIT(m_ir, 4)) {                 // カーソル/表示のシフト
		const int direction = BIT(m_ir, 2) ? +1 : -1;
		if (BIT(m_ir, 3))
			shift_display(direction);
		else
			update_ac(direction);
		set_busy(10);
	} else if (BIT(m_ir, 3)) {          // 表示のオンオフ
		m_display_on = BIT(m_ir, 2);
		m_cursor_on  = BIT(m_ir, 1);
		m_blink_on   = BIT(m_ir, 0);
		set_busy(10);
	} else if (BIT(m_ir, 2)) {          // エントリモード
		m_direction = BIT(m_ir, 1) ? +1 : -1;
		m_shift_on  = BIT(m_ir, 0);
		set_busy(10);
	} else if (BIT(m_ir, 1)) {          // ホーム
		m_ac = 0;
		m_active_ram = DDRAM;
		m_direction = 1;
		m_disp_shift = 0;
		set_busy(410);
	} else if (BIT(m_ir, 0)) {          // 表示クリア
		m_ac = 0;
		m_active_ram = DDRAM;
		m_direction = 1;
		m_disp_shift = 0;
		std::memset(m_ddram, 0x20, sizeof(m_ddram));
		set_busy(410);
	}
}

u8 hd44780_device::control_r() const
{
	if (m_data_len == 4) {
		if (m_nibble)
			return u8((busy() ? 0x80 : 0) | (m_ac & 0x70));
		return u8((m_ac << 4) & 0xf0);
	}
	return u8((busy() ? 0x80 : 0) | (m_ac & 0x7f));
}

void hd44780_device::data_w(u8 data)
{
	if (m_data_len == 4) {
		if (m_nibble) { m_dr = data & 0xf0; return; }
		m_dr |= (data >> 4) & 0x0f;
	} else
		m_dr = data;

	if (m_active_ram == DDRAM)
		m_ddram[m_ac] = m_dr;
	else
		m_cgram[m_ac] = m_dr;

	set_busy(10);
	update_ac(m_direction);
	if (m_shift_on)
		shift_display(m_direction);
}

u8 hd44780_device::data_r()
{
	u8 data = (m_active_ram == DDRAM) ? m_ddram[m_ac] : m_cgram[m_ac];

	if (m_data_len == 4) {
		if (m_nibble)
			return data & 0xf0;
		data = u8((data << 4) & 0xf0);
	}

	set_busy(10);
	update_ac(m_direction);
	return data;
}
