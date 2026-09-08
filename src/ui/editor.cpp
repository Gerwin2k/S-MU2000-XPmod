// license:BSD-3-Clause
//
// エディタの面。SOL2 の XG エディタに倣って、パートを 1 つ選び、
// そのパートのつまみを並べる。
//
// **音源には手を入れない。MIDI を送るだけ**。ここで動かすものは全部
// XG のコントロールチェンジで決まっているので、実機に送るのと同じことをする。
// 実機と同じ経路なので、パネルから触った結果とも矛盾しない。
//
// 覚えている値は「この画面から送った値」。音源から読み返す術が無いので、
// パネル側で音を変えると表示とずれる。SOL2 のエディタも同じ立て付け。

#include "panel.h"
#include "draw.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ui {

namespace {

struct knob_place { int ctl; double x, y; const char *label; };

// つまみ 18 個。6 列 × 3 行。番号は XG の標準的な割り当て
const knob_place KNOBS[] = {
	{   7, 336,  74, "Volume"    },
	{  10, 440,  74, "Pan"       },
	{  11, 544,  74, "Express"   },
	{  91, 648,  74, "Reverb"    },
	{  93, 752,  74, "Chorus"    },
	{  94, 856,  74, "Variation" },

	{  74, 336, 158, "Cutoff"    },
	{  71, 440, 158, "Resonance" },
	{  73, 544, 158, "Attack"    },
	{  75, 648, 158, "Decay"     },
	{  72, 752, 158, "Release"   },
	{  76, 856, 158, "Vib Rate"  },

	{  77, 336, 242, "Vib Depth" },
	{  78, 440, 242, "Vib Delay" },
	{   5, 544, 242, "Porta"     },
	{   1, 648, 242, "Modulation" },
	{ CTL_BANK_MSB, 752, 242, "Bank"    },
	{ CTL_PROGRAM,  856, 242, "Program" },
};

// XG の初期値。MU2000 の電源投入時に近づけてある
u8 default_cc(int cc)
{
	switch (cc) {
	case 7:  return 100;
	case 10: return 64;
	case 11: return 127;
	case 91: return 40;
	case 71: case 72: case 73: case 74: case 75:
	case 76: case 77: case 78: return 64;
	default: return 0;
	}
}

} // namespace


int panel::value_of(int ctl) const
{
	if (ctl >= 0 && ctl < 128)  return m_cc[m_part][ctl];
	if (ctl == CTL_PROGRAM)     return m_prog[m_part];
	if (ctl == CTL_BANK_MSB)    return m_bank[m_part];
	return 0;
}

void panel::set_value(int ctl, int v, bridge &br)
{
	v = std::clamp(v, 0, 127);
	const u8 ch = u8(m_part);

	if (ctl >= 0 && ctl < 128) {
		m_cc[m_part][ctl] = u8(v);
		const u8 msg[3] = { u8(0xb0 | ch), u8(ctl), u8(v) };
		br.send(msg, 3);
		return;
	}
	if (ctl == CTL_PROGRAM) {
		m_prog[m_part] = u8(v);
		// バンクは音色を選び直したときに効くので、毎回 3 つ揃えて送る
		const u8 msg[8] = { u8(0xb0 | ch), 0, m_bank[m_part],
		                    u8(0xb0 | ch), 32, 0,
		                    u8(0xc0 | ch), u8(v) };
		br.send(msg, 8);
		return;
	}
	if (ctl == CTL_BANK_MSB) {
		m_bank[m_part] = u8(v);
		const u8 msg[8] = { u8(0xb0 | ch), 0, u8(v),
		                    u8(0xb0 | ch), 32, 0,
		                    u8(0xc0 | ch), m_prog[m_part] };
		br.send(msg, 8);
		return;
	}
}


void panel::draw_knob(HDC dc, const spot &sp) const
{
	// つまみの場所は「丸の中心 = 枠の上から 26、半径 18」と決めてある
	const double PI = 3.14159265358979;
	const int cx = (sp.r.left + sp.r.right) / 2;
	const int cy = sp.r.top + int(26 * m_scale);
	const int r  = int(18 * m_scale);
	const int v  = value_of(sp.ctl);

	// 12 時を 0 度として、-135 度から +135 度まで
	auto at = [&](double deg, double rad_scale, int &x, int &y) {
		const double a = deg * PI / 180.0;
		x = cx + int(std::sin(a) * r * rad_scale);
		y = cy - int(std::cos(a) * r * rad_scale);
	};

	const int lit = int(std::lround(v / 127.0 * 24));
	for (int i = 0; i <= 24; i++) {
		const double deg = -135.0 + 270.0 * i / 24.0;
		int x1, y1, x2, y2;
		at(deg, 1.18, x1, y1);
		at(deg, 1.42, x2, y2);
		line(dc, x1, y1, x2, y2, (i <= lit) ? ACCENT : RGB(66, 70, 76),
		     std::max(1, int(2 * m_scale)));
	}

	disc(dc, cx, cy, r, RGB(52, 56, 62), RGB(88, 93, 100), std::max(1, int(m_scale)));

	int px, py;
	at(-135.0 + 270.0 * v / 127.0, 0.80, px, py);
	line(dc, cx, cy, px, py, RGB(236, 240, 244), std::max(2, int(2.5 * m_scale)));

	RECT lab{ sp.r.left, sp.r.top + int(44 * m_scale),
	          sp.r.right, sp.r.top + int(55 * m_scale) };
	text_in(dc, lab, sp.label, TEXT_DIM, m_font_small, DT_CENTER | DT_TOP | DT_SINGLELINE);

	char num[16];
	if (sp.ctl == CTL_PROGRAM) std::snprintf(num, sizeof(num), "%d", v + 1);
	else                       std::snprintf(num, sizeof(num), "%d", v);
	RECT val{ sp.r.left, sp.r.top + int(54 * m_scale),
	          sp.r.right, sp.r.top + int(66 * m_scale) };
	text_in(dc, val, num, TEXT, m_font_small, DT_CENTER | DT_TOP | DT_SINGLELINE);
}


void panel::paint_editor(HDC dc, const char *status) const
{
	RECT all{ 0, 0, m_w, m_h };
	fill(dc, all, BODY);
	RECT top{ 0, 0, m_w, m_oy + int(24 * m_scale) };
	fill(dc, top, BODY_TOP);

	// パート
	RECT lab = scale(26, 42, 120, 16);
	text_in(dc, lab, "PART", TEXT_DIM, m_font_small, DT_LEFT | DT_TOP | DT_SINGLELINE);

	for (const spot &sp : m_spots) {
		switch (sp.kind) {
		case spot_kind::part: {
			const bool on = (sp.ctl - CTL_PART) == m_part;
			round_box(dc, sp.r, on ? ACCENT : BTN_FACE, BTN_EDGE, int(4 * m_scale));
			char n[8];
			std::snprintf(n, sizeof(n), "%d", sp.ctl - CTL_PART + 1);
			text_in(dc, sp.r, n, on ? RGB(18, 26, 12) : TEXT, m_font_small,
			        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
			break;
		}
		case spot_kind::knob:
			draw_knob(dc, sp);
			break;
		case spot_kind::action:
			round_box(dc, sp.r, BTN_FACE, BTN_EDGE, int(5 * m_scale));
			text_in(dc, sp.r, sp.label, TEXT, m_font_small,
			        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
			break;
		default:
			break;
		}
	}

	// 選んでいるパートの中身を字でも出す
	char line1[128];
	std::snprintf(line1, sizeof(line1), "Part %d   Bank %d   Voice %d   Volume %d",
	              m_part + 1, m_bank[m_part], m_prog[m_part] + 1, m_cc[m_part][7]);
	RECT info = scale(26, 200, 320, 18);
	text_in(dc, info, line1, TEXT, m_font_small, DT_LEFT | DT_VCENTER | DT_WORDBREAK);

	RECT hint = scale(26, 288, 320, 46);
	text_in(dc, hint,
	        "つまみは上下にドラッグ、またはホイール。\n"
	        "送っているのは XG のコントロールチェンジそのもの。",
	        RGB(104, 109, 116), m_font_small, DT_LEFT | DT_TOP | DT_WORDBREAK);

	if (status && status[0])
		text_in(dc, m_status, status, TEXT_DIM, m_font_small,
		        DT_LEFT | DT_VCENTER | DT_SINGLELINE);

	draw_tabs(dc);
}


// ---- 入力

bool panel::press(int x, int y, bridge &br)
{
	const spot *sp = hit(x, y);
	if (!sp)
		return false;

	switch (sp->kind) {
	case spot_kind::tab:
		m_page = (sp->ctl == CTL_TAB_EDIT) ? page::editor : page::front;
		build_spots();
		return true;

	case spot_kind::button:
		m_held = sp;
		br.press(sp->button, true);
		return true;

	case spot_kind::volume:
		m_held = sp;
		m_volume_now = std::clamp(double(x - sp->r.left) /
		                          std::max(1L, sp->r.right - sp->r.left), 0.0, 1.0);
		br.set_gain(float(m_volume_now));
		return true;

	case spot_kind::part:
		m_part = sp->ctl - CTL_PART;
		return true;

	case spot_kind::knob:
		m_held = sp;
		m_drag_y = y;
		m_drag_from = value_of(sp->ctl);
		return true;

	case spot_kind::action:
		if (sp->ctl == CTL_XG_RESET) {
			// XG システムオン。実機の電源投入直後と同じ状態に戻す
			const u8 xg[9] = { 0xf0, 0x43, 0x10, 0x4c, 0x00, 0x00, 0x7e, 0x00, 0xf7 };
			br.send(xg, 9);
			for (int p = 0; p < 16; p++) {
				for (int c = 0; c < 128; c++)
					m_cc[p][c] = default_cc(c);
				m_prog[p] = 0;
				m_bank[p] = 0;
			}
		} else if (sp->ctl == CTL_ALL_OFF) {
			for (int p = 0; p < 16; p++) {
				const u8 msg[6] = { u8(0xb0 | p), 120, 0, u8(0xb0 | p), 123, 0 };
				br.send(msg, 6);
			}
		}
		return true;

	default:
		return false;
	}
}

bool panel::drag(int x, int y, bridge &br)
{
	if (!m_held)
		return false;

	if (m_held->kind == spot_kind::volume) {
		m_volume_now = std::clamp(double(x - m_held->r.left) /
		                          std::max(1L, m_held->r.right - m_held->r.left), 0.0, 1.0);
		br.set_gain(float(m_volume_now));
		return true;
	}
	if (m_held->kind == spot_kind::knob) {
		// 100 画素で端から端まで。細かく合わせたいときはホイールを使う
		const int v = m_drag_from + int((m_drag_y - y) * 127.0 / (100.0 * m_scale));
		if (v != value_of(m_held->ctl)) {
			set_value(m_held->ctl, v, br);
			return true;
		}
	}
	return false;
}

bool panel::release(bridge &br)
{
	if (!m_held)
		return false;
	if (m_held->kind == spot_kind::button)
		br.press(m_held->button, false);
	m_held = nullptr;
	return true;
}

bool panel::wheel_at(int x, int y, int delta, bridge &br)
{
	const spot *sp = hit(x, y);
	if (sp && sp->kind == spot_kind::knob) {
		set_value(sp->ctl, value_of(sp->ctl) + delta, br);
		return true;
	}
	if (m_page == page::front) {
		// パネルの面ではどこで回しても VALUE。実機に回すものが無いので
		br.turn(delta);
		m_wheel_angle = (m_wheel_angle + delta * 15) % 360;
		return true;
	}
	return false;
}


// ---- 配置

void panel::build_editor_spots()
{
	for (int i = 0; i < 16; i++)
		m_spots.push_back({ spot_kind::part, mu2000::button::count, CTL_PART + i,
		                    scale(26 + (i % 8) * 36, 44 + (i / 8) * 30, 30, 26), "", "" });

	// 枠は 80 × 66。丸の中心は上から 26、名前と値はその下
	for (const knob_place &k : KNOBS)
		m_spots.push_back({ spot_kind::knob, mu2000::button::count, k.ctl,
		                    scale(k.x - 40, k.y - 26, 80, 66), k.label, "" });

	m_spots.push_back({ spot_kind::action, mu2000::button::count, CTL_XG_RESET,
	                    scale(26, 250, 130, 24), "XG リセット", "" });
	m_spots.push_back({ spot_kind::action, mu2000::button::count, CTL_ALL_OFF,
	                    scale(162, 250, 150, 24), "オールノートオフ", "" });
}

void panel::init_editor_values()
{
	for (int p = 0; p < 16; p++) {
		for (int c = 0; c < 128; c++)
			m_cc[p][c] = default_cc(c);
		m_prog[p] = 0;
		m_bank[p] = 0;
	}
}

} // namespace ui
