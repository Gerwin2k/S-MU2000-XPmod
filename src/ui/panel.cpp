// license:BSD-3-Clause

#include "panel.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ui {

namespace {

// ---- 色

const COLORREF BODY      = RGB(28, 30, 34);
const COLORREF BODY_TOP  = RGB(44, 47, 53);
const COLORREF BEZEL     = RGB(12, 12, 12);
const COLORREF LCD_BACK  = RGB(150, 205, 45);
const COLORREF LCD_GHOST = RGB(140, 194, 44);   // 消えている点。実物もうっすら見える
const COLORREF LCD_DOT   = RGB(18, 22, 14);
const COLORREF LED_OFF   = RGB(20, 28, 10);
const COLORREF LED_ON    = RGB(178, 255, 51);
const COLORREF BTN_FACE  = RGB(58, 62, 68);
const COLORREF BTN_EDGE  = RGB(92, 97, 104);
const COLORREF BTN_DOWN  = RGB(126, 170, 70);
const COLORREF TEXT      = RGB(226, 229, 233);
const COLORREF TEXT_DIM  = RGB(150, 155, 162);
const COLORREF WHEEL     = RGB(46, 49, 54);
const COLORREF WHEEL_EDGE= RGB(96, 101, 108);

void fill(HDC dc, const RECT &r, COLORREF c)
{
	HBRUSH b = CreateSolidBrush(c);
	FillRect(dc, &r, b);
	DeleteObject(b);
}

// 角を落とした四角。ボタンはこれで描く
void round_box(HDC dc, const RECT &r, COLORREF face, COLORREF edge, int radius)
{
	HBRUSH b = CreateSolidBrush(face);
	HPEN   p = CreatePen(PS_SOLID, 1, edge);
	HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
	RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
	SelectObject(dc, ob);
	SelectObject(dc, op);
	DeleteObject(b);
	DeleteObject(p);
}

void disc(HDC dc, int cx, int cy, int r, COLORREF face, COLORREF edge)
{
	HBRUSH b = CreateSolidBrush(face);
	HPEN   p = CreatePen(PS_SOLID, 2, edge);
	HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
	Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
	SelectObject(dc, ob);
	SelectObject(dc, op);
	DeleteObject(b);
	DeleteObject(p);
}

// 字は UTF-8 で渡す。DrawTextA だと ANSI と見なされて日本語が化けるので、
// 一度 UTF-16 に直してから描く
void text_in(HDC dc, const RECT &r, const char *s, COLORREF c, HFONT f, UINT flags)
{
	if (!s || !s[0])
		return;
	wchar_t buf[256];
	const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, buf, 256);
	if (n <= 0)
		return;

	HGDIOBJ of = SelectObject(dc, f);
	SetTextColor(dc, c);
	SetBkMode(dc, TRANSPARENT);
	RECT rr = r;
	DrawTextW(dc, buf, n - 1, &rr, flags);
	SelectObject(dc, of);
}

// ---- 配置の表。論理座標（1000 × 300）

struct place { mu2000::button b; double x, y, w, h; const char *label; const char *sub; };

// 音色カテゴリ 18 個。実機と同じ GM の並びで 6 列 × 3 行
const place CATEGORIES[] = {
	{ mu2000::button::piano,         520,  22, 45, 30, "Piano",  "" },
	{ mu2000::button::chrom_perc,    568,  22, 45, 30, "C.Perc", "" },
	{ mu2000::button::organ,         616,  22, 45, 30, "Organ",  "" },
	{ mu2000::button::guitar,        664,  22, 45, 30, "Guitar", "" },
	{ mu2000::button::bass,          712,  22, 45, 30, "Bass",   "" },
	{ mu2000::button::strings,       760,  22, 45, 30, "Strngs", "" },

	{ mu2000::button::ensemble,      520,  55, 45, 30, "Ensmbl", "" },
	{ mu2000::button::brass,         568,  55, 45, 30, "Brass",  "" },
	{ mu2000::button::reed,          616,  55, 45, 30, "Reed",   "" },
	{ mu2000::button::pipe,          664,  55, 45, 30, "Pipe",   "" },
	{ mu2000::button::synth_lead,    712,  55, 45, 30, "S.Lead", "" },
	{ mu2000::button::synth_pad,     760,  55, 45, 30, "S.Pad",  "" },

	{ mu2000::button::synth_effects, 520,  88, 45, 30, "S.FX",   "" },
	{ mu2000::button::ethnic,        568,  88, 45, 30, "Ethnic", "" },
	{ mu2000::button::percussive,    616,  88, 45, 30, "Perc",   "" },
	{ mu2000::button::sfx,           664,  88, 45, 30, "SFX",    "" },
	{ mu2000::button::model_excl,    712,  88, 45, 30, "Model",  "" },
	{ mu2000::button::drum,          760,  88, 45, 30, "Drum",   "" },
};

// 操作ボタン
const place CONTROLS[] = {
	{ mu2000::button::play,          520, 128, 39, 32, "PLAY",   "" },
	{ mu2000::button::edit,          562, 128, 39, 32, "EDIT",   "" },
	{ mu2000::button::util,          604, 128, 39, 32, "UTIL",   "" },
	{ mu2000::button::effect,        646, 128, 39, 32, "EFFECT", "" },
	{ mu2000::button::mute_solo,     688, 128, 39, 32, "MUTE",   "SOLO" },
	{ mu2000::button::part_minus,    730, 128, 39, 32, "PART",   "-" },
	{ mu2000::button::part_plus,     772, 128, 39, 32, "PART",   "+" },

	{ mu2000::button::exit,          520, 166, 39, 32, "EXIT",   "" },
	{ mu2000::button::enter,         562, 166, 39, 32, "ENTER",  "" },
	{ mu2000::button::select_left,   604, 166, 39, 32, "SEL",    "<" },
	{ mu2000::button::select_right,  646, 166, 39, 32, "SEL",    ">" },
	{ mu2000::button::seq,           688, 166, 39, 32, "SEQ",    "" },
	{ mu2000::button::audition,      730, 166, 39, 32, "AUDIT",  "" },
	{ mu2000::button::select,        772, 166, 39, 32, "SELECT", "" },

	{ mu2000::button::sampling_mode, 520, 204, 81, 30, "SAMPLING / MODE", "" },
};

// VALUE の上下。ダイヤルの下に置く
const place VALUES[] = {
	{ mu2000::button::value_minus,   830, 160, 60, 32, "VALUE", "-" },
	{ mu2000::button::value_plus,    896, 160, 60, 32, "VALUE", "+" },
};

} // namespace


panel::panel()
{
	resize(LOGICAL_W, LOGICAL_H);
}

panel::~panel()
{
	if (m_font_label) DeleteObject(m_font_label);
	if (m_font_small) DeleteObject(m_font_small);
}

RECT panel::scale(double x, double y, double w, double h) const
{
	RECT r;
	r.left   = m_ox + int(std::lround(x * m_scale));
	r.top    = m_oy + int(std::lround(y * m_scale));
	r.right  = m_ox + int(std::lround((x + w) * m_scale));
	r.bottom = m_oy + int(std::lround((y + h) * m_scale));
	return r;
}

void panel::resize(int w, int h)
{
	m_w = std::max(w, 200);
	m_h = std::max(h, 60);

	// 縦横比を保ち、余った側に余白を置く
	m_scale = std::min(double(m_w) / LOGICAL_W, double(m_h) / LOGICAL_H);
	m_ox = int((m_w - LOGICAL_W * m_scale) / 2);
	m_oy = int((m_h - LOGICAL_H * m_scale) / 2);

	m_spots.clear();
	for (const place &p : CATEGORIES)
		m_spots.push_back({ spot_kind::button, p.b, scale(p.x, p.y, p.w, p.h), p.label, p.sub });
	for (const place &p : CONTROLS)
		m_spots.push_back({ spot_kind::button, p.b, scale(p.x, p.y, p.w, p.h), p.label, p.sub });
	for (const place &p : VALUES)
		m_spots.push_back({ spot_kind::button, p.b, scale(p.x, p.y, p.w, p.h), p.label, p.sub });

	// LCD は点の並び（横 143 点 × 縦 19 点）に合わせた細長い窓
	m_lcd    = scale(26, 24, 432, 68);
	m_volume = scale(26, 106, 380, 22);
	m_status = scale(26, 138, 470, 18);
	m_hint   = scale(26, 158, 470, 18);
	m_wheel  = scale(828, 20, 128, 128);
	// ダイヤルは丸だが、当たり判定は外接する四角で足りる
	m_spots.push_back({ spot_kind::wheel,  mu2000::button::count, m_wheel,  "VALUE", "" });
	m_spots.push_back({ spot_kind::volume, mu2000::button::count, m_volume, "VOLUME", "" });

	for (int i = 0; i < 6; i++)
		m_leds[i] = scale(472 + (i & 1) * 22, 26 + (i / 2) * 24, 13, 13);

	if (m_font_label) DeleteObject(m_font_label);
	if (m_font_small) DeleteObject(m_font_small);
	auto make_font = [&](int px, int weight) {
		return CreateFontA(-std::max(px, 7), 0, 0, 0, weight, FALSE, FALSE, FALSE,
		                   DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
		                   CLEARTYPE_QUALITY, VARIABLE_PITCH, "Segoe UI");
	};
	m_font_label = make_font(int(11 * m_scale), FW_SEMIBOLD);
	m_font_small = make_font(int(9  * m_scale), FW_NORMAL);
}

const spot *panel::hit(int x, int y) const
{
	for (const spot &s : m_spots) {
		if (x >= s.r.left && x < s.r.right && y >= s.r.top && y < s.r.bottom) {
			// ダイヤルは丸の中だけ
			if (s.kind == spot_kind::wheel) {
				const double cx = (s.r.left + s.r.right) * 0.5;
				const double cy = (s.r.top + s.r.bottom) * 0.5;
				const double rr = (s.r.right - s.r.left) * 0.5;
				if (std::hypot(x - cx, y - cy) > rr)
					continue;
			}
			return &s;
		}
	}
	return nullptr;
}


void panel::draw_lcd(HDC dc, const snapshot &s) const
{
	// 枠
	RECT bez = m_lcd;
	InflateRect(&bez, int(6 * m_scale), int(6 * m_scale));
	round_box(dc, bez, BEZEL, RGB(70, 72, 76), int(8 * m_scale));
	fill(dc, m_lcd, LCD_BACK);

	// 点の大きさを窓に合わせる。横 24 桁 ×(5 点 + 隙間 1)、縦 2 行 ×(8 点 + 隙間 3)
	const int cols_dots = LCD_COLS * (CELL_W + 1) - 1;
	const int rows_dots = LCD_ROWS * CELL_H + 3;
	const int aw = m_lcd.right - m_lcd.left, ah = m_lcd.bottom - m_lcd.top;
	const int pad = std::max(2, int(5 * m_scale));
	const int d = std::max(1, std::min((aw - pad * 2) / cols_dots,
	                                   (ah - pad * 2) / rows_dots));
	const int x0 = m_lcd.left + (aw - d * cols_dots) / 2;
	const int y0 = m_lcd.top  + (ah - d * rows_dots) / 2;
	const int dot = std::max(1, d - std::max(1, d / 6));   // 点の隙間

	HBRUSH ghost = CreateSolidBrush(LCD_GHOST);
	HBRUSH lit   = CreateSolidBrush(LCD_DOT);

	for (int row = 0; row < LCD_ROWS; row++) {
		for (int col = 0; col < LCD_COLS; col++) {
			const u8 *cell = s.dots + (row * LCD_COLS + col) * CELL_H;
			for (int y = 0; y < CELL_H; y++) {
				for (int x = 0; x < CELL_W; x++) {
					RECT r;
					r.left   = x0 + (col * (CELL_W + 1) + x) * d;
					r.top    = y0 + (row * (CELL_H + 3) + y) * d;
					r.right  = r.left + dot;
					r.bottom = r.top + dot;
					const bool on = s.lcd_on && BIT(cell[y], 4 - x);
					FillRect(dc, &r, on ? lit : ghost);
				}
			}
		}
	}
	DeleteObject(ghost);
	DeleteObject(lit);

	// 起動中や ROM が無いときは字で重ねる。画面が無いと理由が分からない
	if (s.message[0]) {
		RECT r = m_lcd;
		fill(dc, r, RGB(24, 26, 22));
		text_in(dc, r, s.message, RGB(210, 220, 200), m_font_label,
		        DT_CENTER | DT_VCENTER | DT_WORDBREAK);
	}
}

void panel::draw_button(HDC dc, const spot &sp, bool down) const
{
	round_box(dc, sp.r, down ? BTN_DOWN : BTN_FACE, BTN_EDGE, int(6 * m_scale));

	RECT r = sp.r;
	if (sp.sub[0]) {
		// 上に名前、下に -/+ などの添え字
		RECT top = r, bot = r;
		top.bottom = r.top + (r.bottom - r.top) * 6 / 10;
		bot.top    = top.bottom - int(2 * m_scale);
		text_in(dc, top, sp.label, down ? RGB(20, 30, 10) : TEXT, m_font_small,
		        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		text_in(dc, bot, sp.sub, down ? RGB(20, 30, 10) : TEXT_DIM, m_font_small,
		        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	} else {
		text_in(dc, r, sp.label, down ? RGB(20, 30, 10) : TEXT, m_font_small,
		        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	}
}

void panel::draw_wheel(HDC dc, int angle) const
{
	const int cx = (m_wheel.left + m_wheel.right) / 2;
	const int cy = (m_wheel.top + m_wheel.bottom) / 2;
	const int r  = (m_wheel.right - m_wheel.left) / 2;

	disc(dc, cx, cy, r, WHEEL, WHEEL_EDGE);
	disc(dc, cx, cy, r * 7 / 10, RGB(38, 41, 46), RGB(70, 74, 80));

	// 回した向きが見えるよう、縁に印を並べる
	HPEN p = CreatePen(PS_SOLID, std::max(1, int(2 * m_scale)), RGB(120, 126, 134));
	HGDIOBJ op = SelectObject(dc, p);
	for (int i = 0; i < 24; i++) {
		const double a = (angle + i * 15) * 3.14159265358979 / 180.0;
		const int x1 = cx + int(std::cos(a) * r * 0.78);
		const int y1 = cy + int(std::sin(a) * r * 0.78);
		const int x2 = cx + int(std::cos(a) * r * 0.94);
		const int y2 = cy + int(std::sin(a) * r * 0.94);
		MoveToEx(dc, x1, y1, nullptr);
		LineTo(dc, x2, y2);
	}
	SelectObject(dc, op);
	DeleteObject(p);

	RECT t = m_wheel;
	t.top = cy - int(8 * m_scale);
	text_in(dc, t, "VALUE", TEXT_DIM, m_font_small, DT_CENTER | DT_TOP | DT_SINGLELINE);
}

void panel::draw_volume(HDC dc, double v) const
{
	RECT track = m_volume;
	track.top += (track.bottom - track.top) / 2 - int(3 * m_scale);
	track.bottom = track.top + int(6 * m_scale);
	round_box(dc, track, RGB(18, 19, 22), RGB(70, 74, 80), int(4 * m_scale));

	const int span = (m_volume.right - m_volume.left) - int(26 * m_scale);
	RECT knob;
	knob.left  = m_volume.left + int(std::lround(v * span));
	knob.right = knob.left + int(26 * m_scale);
	knob.top   = m_volume.top;
	knob.bottom= m_volume.bottom;
	round_box(dc, knob, RGB(80, 85, 92), RGB(120, 126, 134), int(5 * m_scale));

	RECT lab = m_volume;
	lab.left = m_volume.right + int(8 * m_scale);
	lab.right = lab.left + int(80 * m_scale);
	text_in(dc, lab, "VOLUME", TEXT_DIM, m_font_small, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

void panel::paint(HDC dc, const snapshot &s, u64 pressed, double volume,
                  int wheel_angle, const char *status) const
{
	RECT all{ 0, 0, m_w, m_h };
	fill(dc, all, BODY);
	RECT top = scale(0, 0, LOGICAL_W, 14);
	top.left = 0; top.right = m_w;
	fill(dc, top, BODY_TOP);

	// 名札
	RECT name = scale(30, 2, 260, 20);
	text_in(dc, name, "YAMAHA  MU2000  (S-MU2000)", RGB(190, 196, 204),
	        m_font_small, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

	draw_lcd(dc, s);

	for (int i = 0; i < 6; i++) {
		const bool on = BIT(s.leds, i) != 0;
		disc(dc, (m_leds[i].left + m_leds[i].right) / 2,
		         (m_leds[i].top + m_leds[i].bottom) / 2,
		     (m_leds[i].right - m_leds[i].left) / 2,
		     on ? LED_ON : LED_OFF, RGB(60, 64, 60));
	}

	for (const spot &sp : m_spots) {
		if (sp.kind != spot_kind::button)
			continue;
		draw_button(dc, sp, (pressed >> int(sp.button)) & 1);
	}

	draw_wheel(dc, wheel_angle);
	draw_volume(dc, volume);

	if (status && status[0])
		text_in(dc, m_status, status, TEXT_DIM, m_font_small,
		        DT_LEFT | DT_VCENTER | DT_SINGLELINE);
	text_in(dc, m_hint,
		        "ホイール = VALUE ／ クリックでボタン ／ キー: A=PLAY E=EDIT U=UTIL "
		        "F=EFFECT [ ] =PART",
		        RGB(104, 109, 116), m_font_small, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
}

} // namespace ui
