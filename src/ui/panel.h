// license:BSD-3-Clause
//
// 画面。実機のフロントパネルと、SOL2 風のエディタの 2 面を持つ。
//
// exe（gui.exe）と VST3 の画面で同じものを使う。どちらも Windows なので
// 描画は GDI で済ませ、外からは HDC を 1 枚渡してもらうだけにしてある。
//
// 配置は論理座標（LOGICAL_W × LOGICAL_H）で持ち、窓の大きさに合わせて
// 一律に拡大縮小する。実機の寸法をそのまま写したものではなく、
// 実機の並び（LCD が左、音色カテゴリが真ん中、VALUE が右）に倣った配置。
//
// 入力も面ごとに違うので、窓側は press/drag/release/wheel_at をそのまま
// 渡すだけでよい。中で MIDI が要るものは bridge に積む。

#ifndef S_MU2000_UI_PANEL_H
#define S_MU2000_UI_PANEL_H

#pragma once

#include "bridge.h"
#include "layout.h"
#include "snapshot.h"

#include <string>
#include <vector>

#include <windows.h>

namespace ui {

enum class page { front, editor, effects };

// つまみが何を動かすか。0-127 はそのままコントロールチェンジの番号
enum : int {
	CTL_NONE      = -1,
	CTL_PROGRAM   = 200,
	CTL_BANK_MSB  = 201,
	CTL_PART      = 300,   // +0..15
	CTL_TAB_FRONT = 400,
	CTL_TAB_EDIT  = 401,
	CTL_TAB_FX    = 404,
	CTL_XG_RESET  = 402,
	CTL_ALL_OFF   = 403,

	// エフェクト面。XG のシステムエフェクトと MU の インサーション 2 系統
	CTL_REV_TYPE  = 500, CTL_REV_RET  = 501,
	CTL_CHO_TYPE  = 502, CTL_CHO_RET  = 503,
	CTL_VAR_TYPE  = 504, CTL_VAR_CONN = 505, CTL_VAR_PART = 506,
	CTL_INS1_TYPE = 507, CTL_INS1_PART = 508,
	CTL_INS2_TYPE = 509, CTL_INS2_PART = 510,
	CTL_FX_SEND   = 511,
	CTL_FX_FIRST  = 500, CTL_FX_COUNT = 11,
};

// 触れる場所
enum class spot_kind { none, button, wheel, volume, knob, tab, action, part, list };

struct spot {
	spot_kind      kind = spot_kind::none;
	mu2000::button button = mu2000::button::count;
	int            ctl = CTL_NONE;
	RECT           r{};
	const char    *label = "";
	const char    *sub   = "";     // 小さく添える字。無ければ空
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

	page current_page() const { return m_page; }

	// 音量つまみの見え方。音源側の値をそのまま渡してもらう
	void set_volume(double v) { m_volume_now = v; }

	// 論理座標の方眼を重ねる。絵の位置を直すときの物差し（doc/panel-editing.md）
	void set_grid(bool on) { m_grid = on; }

	// 配置。**作り直さずに文字ファイルで直せる**（doc/panel-editing.md）。
	// 読み直したら resize() をやり直すこと
	layout       &lay()       { return m_lay; }
	const layout &lay() const { return m_lay; }

	// 実際の窓の座標から、触れる場所を探す
	const spot *hit(int x, int y) const;

	// パネルに描いてある MIDI IN A のジャック。窓側はここを押されたら
	// 入出力の口を選ぶ品書きを出す
	RECT midi_jack() const;
	bool on_midi_jack(int x, int y) const;

	// ---- 入力。窓からそのまま渡す。戻り値は「描き直しが要るか」

	bool press(int x, int y, bridge &br);
	bool drag(int x, int y, bridge &br);
	bool release(bridge &br);
	bool wheel_at(int x, int y, int delta, bridge &br);

	// 描く。status は下に小さく出す 1 行。無ければ空でよい
	void paint(HDC dc, const snapshot &s, u64 pressed, const char *status) const;

	const std::vector<spot> &spots() const { return m_spots; }

private:
	RECT scale(double x, double y, double w, double h) const;
	POINT at(double x, double y) const;
	void build_spots();
	void build_editor_spots();
	void build_effect_spots();
	void init_effect_values();
	void send_all_fx(bridge &br);
	void init_editor_values();

	void paint_front(HDC dc, const snapshot &s, u64 pressed, double volume,
	                 const char *status) const;
	void paint_editor(HDC dc, const char *status) const;
	void paint_effects(HDC dc, const char *status) const;

	void draw_lcd(HDC dc, const snapshot &s) const;
	void draw_grid(HDC dc) const;
	void draw_button(HDC dc, const spot &sp, bool down) const;
	void draw_wheel(HDC dc, int angle) const;
	void draw_volume(HDC dc, double v) const;
	void draw_tabs(HDC dc) const;
	void draw_knob(HDC dc, const spot &sp) const;
	void draw_list(HDC dc, const spot &sp) const;
	const char *fx_text(int ctl, int v) const;
	int  fx_limit(int ctl) const;
	void send_fx(int ctl, bridge &br);

	int  value_of(int ctl) const;
	void set_value(int ctl, int v, bridge &br);

	int m_w = LOGICAL_W, m_h = LOGICAL_H;
	double m_scale = 1.0;
	int m_ox = 0, m_oy = 0;      // 縦横比を保つための余白

	page m_page = page::front;
	std::vector<spot> m_spots;
	RECT m_lcd{}, m_wheel{}, m_volume{}, m_status{}, m_hint{}, m_leds[6]{};

	// 掴んでいるもの
	const spot *m_held = nullptr;
	int  m_drag_y = 0, m_drag_from = 0;
	int  m_wheel_angle = 0;
	double m_volume_now = 1.0;

	// ---- エディタが覚えている値。実機に問い合わせる術がないので、
	// 「この画面から送った値」を持っておく。SOL2 のエディタと同じ考え方
	int m_part = 0;
	u8  m_cc[16][128] = {};
	u8  m_prog[16] = {};
	u8  m_bank[16] = {};

	// エフェクト面が覚えている値。並びは CTL_FX_FIRST から
	int m_fx[CTL_FX_COUNT] = {};

	HFONT m_font_label = nullptr, m_font_small = nullptr;
	// 目盛りの番号用。バー 1 本ぶんの幅に 2 桁を収める
	HFONT m_font_tiny  = nullptr;
	bool   m_grid = false;
	layout m_lay;
};

} // namespace ui

#endif // S_MU2000_UI_PANEL_H
