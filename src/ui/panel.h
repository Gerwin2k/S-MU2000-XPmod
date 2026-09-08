// license:BSD-3-Clause
//
// フロントパネルの絵と当たり判定。
//
// exe（gui.exe）と VST3 の画面で同じものを使う。どちらも Windows なので
// 描画は GDI で済ませ、外からは HDC を 1 枚渡してもらうだけにしてある。
//
// 配置は論理座標（LOGICAL_W × LOGICAL_H）で持ち、窓の大きさに合わせて
// 一律に拡大縮小する。実機の寸法をそのまま写したものではなく、
// 実機の並び（LCD が左、音色カテゴリが真ん中、VALUE が右）に倣った配置。

#ifndef S_MU2000_UI_PANEL_H
#define S_MU2000_UI_PANEL_H

#pragma once

#include "mu2000.h"

#include <string>
#include <vector>

#include <windows.h>

namespace ui {

constexpr int LOGICAL_W = 1000;
constexpr int LOGICAL_H = 250;

// LCD の窓。firmware は 2 行 40 桁で使うが、出ているのは 24 桁ぶん
constexpr int LCD_ROWS = 2, LCD_COLS = 24;
constexpr int CELL_W = 5, CELL_H = 8;

// 音源から画面へ渡すもの。音声スレッドが作り、GUI スレッドが読む
struct snapshot {
	u8   dots[LCD_ROWS * LCD_COLS * CELL_H] = {};   // 各バイトの下位 5bit
	u16  leds = 0;
	bool lcd_on = false;
	bool ready = false;          // 起動が終わったか
	char message[96] = {};       // 起動中／ROM が無い等。空なら出さない
};

// 触れる場所
enum class spot_kind { none, button, wheel, volume };

struct spot {
	spot_kind     kind = spot_kind::none;
	mu2000::button button = mu2000::button::count;
	RECT          r{};
	const char   *label = "";
	const char   *sub   = "";     // 小さく添える字。無ければ空
};

class panel
{
public:
	panel();
	~panel();

	// 窓の大きさが変わったら呼ぶ
	void resize(int w, int h);
	int  width() const  { return m_w; }
	int  height() const { return m_h; }

	// 実際の窓の座標から、触れる場所を探す
	const spot *hit(int x, int y) const;

	// 描く。pressed は押されているボタン（ビット位置は mu2000::button の番号）
	// status は下に小さく出す 1 行（MIDI 入力や CPU 使用率）。無ければ空でよい
	void paint(HDC dc, const snapshot &s, u64 pressed, double volume,
	           int wheel_angle, const char *status) const;

	const std::vector<spot> &spots() const { return m_spots; }

private:
	RECT scale(double x, double y, double w, double h) const;
	void draw_lcd(HDC dc, const snapshot &s) const;
	void draw_button(HDC dc, const spot &sp, bool down) const;
	void draw_wheel(HDC dc, int angle) const;
	void draw_volume(HDC dc, double v) const;

	int m_w = LOGICAL_W, m_h = LOGICAL_H;
	double m_scale = 1.0;
	int m_ox = 0, m_oy = 0;      // 縦横比を保つための余白

	std::vector<spot> m_spots;
	RECT m_lcd{}, m_wheel{}, m_volume{}, m_status{}, m_hint{}, m_leds[6]{};

	HFONT m_font_label = nullptr, m_font_small = nullptr;
};

} // namespace ui

#endif // S_MU2000_UI_PANEL_H
