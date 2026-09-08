// license:BSD-3-Clause
//
// パネルの面。**実機の写真から採寸して並べ直した**。
//
// 論理座標の 1000 × 385 が本体の前面ぜんたい（実機の縦横比はおよそ 2.6:1）。
// 残りの 15 は面を切り替える帯で、本体の外。
//
//   左   A/D INPUT のジャックとつまみ、VOLUME、電源、MIDI IN A、PHONES、カード
//   中   LCD、その下に PART / BANK・PGM# / VOL / EXP / PAN / REV / CHO / VAR / KEY
//        の見出しと、音色カテゴリのボタン 18 個
//   右   PLAY EDIT / UTIL EFFECT / SAMPLING SEQ の 6 個（LED 入り）、
//        MUTE PART−+ / ENTER SELECT−+ / EXIT VALUE−+ の 9 個、
//        SELECT と AUDITION、そして**大きなダイヤル**

#include "panel.h"
#include "draw.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace ui {

namespace {

constexpr double PI = 3.14159265358979;
constexpr double BODY_H = 385;      // 本体の高さ。下の 15 は帯

struct place { mu2000::button b; double x, y, w, h; const char *label; const char *sub; };

// 音色カテゴリ 18 個。6 列 × 3 行。実機は札が上、押すところが下
const double CAT_X[6] = { 288, 354, 419, 482, 545, 607 };
const double CAT_Y[3] = { 219, 266, 310 };
const double CAT_W = 52, CAT_H = 20;

const mu2000::button CAT_B[18] = {
	mu2000::button::piano,      mu2000::button::chrom_perc, mu2000::button::organ,
	mu2000::button::guitar,     mu2000::button::bass,       mu2000::button::strings,
	mu2000::button::ensemble,   mu2000::button::brass,      mu2000::button::reed,
	mu2000::button::pipe,       mu2000::button::synth_lead, mu2000::button::synth_pad,
	mu2000::button::synth_effects, mu2000::button::ethnic,  mu2000::button::percussive,
	mu2000::button::sfx,        mu2000::button::model_excl, mu2000::button::drum,
};
const char *CAT_LABEL[18] = {
	"Piano", "Chrom. perc.", "Organ", "Guitar", "Bass", "Strings",
	"Ensemble", "Brass", "Reed", "Pipe", "Synth lead", "Synth pad",
	"Synth effects", "Ethnic", "Percussive", "SFX", "Model excl.", "Drum",
};

// LCD の下段に並ぶもの。窓の内側の左端からの割合で置く。
// 窓の下に印刷されている札も、ここから位置を取って揃える
// 位置と幅は、上の面の**点 1 つぶん**を単位にした、窓の内側の左端からの
// 数。実機の写真を測って割り出した（上の面は 17 桁 × 6 点 − 1 = 101 点）。
//
// 塊のあいだは 2 点、楽器のかたちの前は 3 点あける。
// 塊の中の桁は**詰めて並べる**（上の面のように 1 点あけない）
const int LOW_X[LOW_COUNT] = {  0, 12, 30, 45, 51, 56, 63, 70, 77, 84, 97 };
const int LOW_W[LOW_COUNT] = { 10, 15, 14,  4,  4,  6,  6,  6,  6, 11,  3 };

// 窓の下に印刷されている札。どの並びの真ん中に置くか
struct column { int at; const char *label; };
const column COLUMNS[] = {
	{ LOW_PART, "PART" }, { LOW_ICON, "BANK/PGM#" }, { LOW_VOL, "VOL" },
	{ LOW_EXP,  "EXP"  }, { LOW_PAN,  "PAN" },       { LOW_REV, "REV" },
	{ LOW_CHO,  "CHO"  }, { LOW_VAR,  "VAR" },       { LOW_KEY, "KEY" },
};

// 右上の 6 個。丸い押しボタンで、中に LED が入っている。
// LED の番号は MAME の mulcd.lay の並び（左列 0,2,4 / 右列 1,3,5）
struct mode_button { mu2000::button b; int led; double x, y; const char *label; };
const mode_button MODES[] = {
	{ mu2000::button::play,          0, 752,  66, "PLAY"     },
	{ mu2000::button::edit,          1, 806,  66, "EDIT"     },
	{ mu2000::button::util,          2, 752, 114, "UTIL"     },
	{ mu2000::button::effect,        3, 806, 114, "EFFECT"   },
	{ mu2000::button::sampling_mode, 4, 752, 162, "SAMPLING" },
	{ mu2000::button::seq,           5, 806, 162, "SEQ"      },
};

// 右端の 9 個
const place NAV[] = {
	{ mu2000::button::mute_solo,    840,  44, 48, 34, "MUTE",   "SOLO" },
	{ mu2000::button::part_minus,   896,  44, 48, 34, "PART",   "-" },
	{ mu2000::button::part_plus,    952,  44, 48, 34, "PART",   "+" },
	{ mu2000::button::enter,        840,  90, 48, 32, "ENTER",  "" },
	{ mu2000::button::select_left,  896,  90, 48, 32, "SELECT", "-" },
	{ mu2000::button::select_right, 952,  90, 48, 32, "SELECT", "+" },
	{ mu2000::button::exit,         840, 134, 48, 34, "EXIT",   "" },
	{ mu2000::button::value_minus,  896, 134, 48, 34, "VALUE",  "-" },
	{ mu2000::button::value_plus,   952, 134, 48, 34, "VALUE",  "+" },
};

// 音色カテゴリの右にある小さな丸ボタン 2 つ
const place ROUND[] = {
	{ mu2000::button::select,   694, 262, 22, 22, "SELECT",   "" },
	{ mu2000::button::audition, 764, 260, 22, 22, "AUDITION", "" },
};

const double DIAL_X = 893, DIAL_Y = 268, DIAL_R = 58;

// 実機の色
const COLORREF PANEL_FACE = RGB(196, 189, 170);
const COLORREF PANEL_INK  = RGB(46, 44, 40);
const COLORREF KEY_FACE   = RGB(216, 205, 165);
const COLORREF KEY_EDGE   = RGB(126, 118, 92);
const COLORREF KEY_DOWN   = RGB(150, 140, 95);

} // namespace


panel::panel()
{
	init_editor_values();
	init_effect_values();
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

// パネルに描いてある MIDI IN A のジャック（丸と札）を囲む枠
RECT panel::midi_jack() const
{
	return scale(92, 230, 96, 90);
}

bool panel::on_midi_jack(int x, int y) const
{
	if (m_page != page::front)
		return false;
	const RECT r = midi_jack();
	return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

// 論理座標の点を実座標へ
POINT panel::at(double x, double y) const
{
	POINT p;
	p.x = m_ox + int(std::lround(x * m_scale));
	p.y = m_oy + int(std::lround(y * m_scale));
	return p;
}

void panel::resize(int w, int h)
{
	m_w = std::max(w, 200);
	m_h = std::max(h, 60);

	m_scale = std::min(double(m_w) / LOGICAL_W, double(m_h) / LOGICAL_H);
	m_ox = int((m_w - LOGICAL_W * m_scale) / 2);
	m_oy = int((m_h - LOGICAL_H * m_scale) / 2);

	m_lcd    = scale(240, 42, 439, 135);
	m_volume = scale(111, 124, 60, 60);       // 丸いつまみ。当たりは丸で見る
	m_status = scale(20, 372, 700, 13);
	m_hint   = scale(20, 386, 700, 13);
	m_wheel  = scale(DIAL_X - DIAL_R, DIAL_Y - DIAL_R, DIAL_R * 2, DIAL_R * 2);
	for (int i = 0; i < 6; i++)
		m_leds[i] = scale(MODES[i].x - 11, MODES[i].y - 11, 22, 22);

	if (m_font_label) DeleteObject(m_font_label);
	if (m_font_small) DeleteObject(m_font_small);
	auto make_font = [&](double px, int weight) {
		return CreateFontA(-std::max(7, int(px * m_scale)), 0, 0, 0, weight,
		                   FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS,
		                   CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, VARIABLE_PITCH,
		                   "Segoe UI");
	};
	m_font_label = make_font(13, FW_BOLD);
	m_font_small = make_font(8.5, FW_NORMAL);

	build_spots();
}

// 触れる場所は面ごとに違う。掴んでいる途中に作り直すと迷子になるので離す
void panel::build_spots()
{
	m_held = nullptr;
	m_spots.clear();

	// 面を選ぶつまみ。本体の外（下の帯）
	m_spots.push_back({ spot_kind::tab, mu2000::button::count, CTL_TAB_FRONT,
	                    scale(700, 386, 94, 13), "パネル", "" });
	m_spots.push_back({ spot_kind::tab, mu2000::button::count, CTL_TAB_EDIT,
	                    scale(800, 386, 94, 13), "エディタ", "" });
	m_spots.push_back({ spot_kind::tab, mu2000::button::count, CTL_TAB_FX,
	                    scale(898, 386, 94, 13), "エフェクト", "" });

	if (m_page == page::editor) { build_editor_spots(); return; }
	if (m_page == page::effects) { build_effect_spots(); return; }

	for (int i = 0; i < 18; i++)
		m_spots.push_back({ spot_kind::button, CAT_B[i], CTL_NONE,
		                    scale(CAT_X[i % 6] - CAT_W / 2, CAT_Y[i / 6], CAT_W, CAT_H),
		                    CAT_LABEL[i], "" });
	for (const mode_button &m : MODES)
		m_spots.push_back({ spot_kind::button, m.b, CTL_NONE,
		                    scale(m.x - 11, m.y - 11, 22, 22), m.label, "" });
	for (const place &p : NAV)
		m_spots.push_back({ spot_kind::button, p.b, CTL_NONE,
		                    scale(p.x, p.y, p.w, p.h), p.label, p.sub });
	for (const place &p : ROUND)
		m_spots.push_back({ spot_kind::button, p.b, CTL_NONE,
		                    scale(p.x - p.w / 2, p.y - p.h / 2, p.w, p.h), p.label, p.sub });

	m_spots.push_back({ spot_kind::wheel,  mu2000::button::count, CTL_NONE, m_wheel, "", "" });
	m_spots.push_back({ spot_kind::volume, mu2000::button::count, CTL_NONE, m_volume,
	                    "VOLUME", "" });
}

const spot *panel::hit(int x, int y) const
{
	for (const spot &s : m_spots) {
		if (x >= s.r.left && x < s.r.right && y >= s.r.top && y < s.r.bottom) {
			// 丸いものは丸の中だけ
			if (s.kind == spot_kind::wheel || s.kind == spot_kind::volume) {
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


void panel::draw_tabs(HDC dc) const
{
	for (const spot &sp : m_spots) {
		if (sp.kind != spot_kind::tab)
			continue;
		const bool on = (sp.ctl == CTL_TAB_EDIT   && m_page == page::editor) ||
		                (sp.ctl == CTL_TAB_FX     && m_page == page::effects) ||
		                (sp.ctl == CTL_TAB_FRONT  && m_page == page::front);
		round_box(dc, sp.r, on ? RGB(70, 76, 84) : RGB(38, 41, 46),
		          on ? ACCENT : RGB(70, 74, 80), int(4 * m_scale));
		text_in(dc, sp.r, sp.label, on ? TEXT : TEXT_DIM, m_font_small,
		        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	}
}

void panel::draw_lcd(HDC dc, const snapshot &s) const
{
	RECT bez = m_lcd;
	InflateRect(&bez, int(5 * m_scale), int(5 * m_scale));
	round_box(dc, bez, RGB(60, 58, 52), RGB(110, 106, 96), int(5 * m_scale));
	fill(dc, m_lcd, LCD_BACK);

	// 実機の窓は、DDRAM の桁がそのまま横一列に並んでいるのではない。
	// ボタンを押して確かめた割り振りは（doc/gui.md）
	//
	//   上の面（点の並び。2 行、桁のあいだは 1 点、**行のあいだは空けない**）
	//     0-8   レベルメータ。1 マス 2 本で 18 本（A1 A2 と 1-16）
	//     9-16  文字 8 桁。1 行目が音色名、2 行目が ▶000◀001
	//           表示の仕方によっては、ここにも 19-32 番のバーが出る
	//   下の面（別のところに出る）
	//     行 0 の 17-18   部の番号「01」
	//     行 1 の 17-19   「A01」
	//     20-23（両行）   楽器のかたち。**点が縦に半分の細かさ**
	//
	//   行 0 の 19 桁目が何なのかは、まだ分かっていない
	const int aw = m_lcd.right - m_lcd.left, ah = m_lcd.bottom - m_lcd.top;
	const int pad = std::max(2, int(5 * m_scale));

	// 点の大きさ。上の面 17 桁ぶんが横幅に収まるように決める
	const int top_dots = TOP_COLS * (CELL_W + 1) - 1;      // 桁のあいだ 1 点
	int d = std::max(1, (aw - pad * 2) / top_dots);
	// 縦は 文字 16 点 ＋ 目盛り 6 点 ＋ 下の面 8 点
	while (d > 1 && d * 16 + d * 6 + d * 8 > ah - pad * 2)
		d--;
	const int dot = std::max(1, d - std::max(1, d / 6));   // 点のあいだの隙間

	// 目盛りの帯は 2 段。上に目盛りと番号、下に MIC / BANK / PGM#
	const int tick_h = std::max(2, int(2.5 * m_scale));
	const int line_h = std::max(6, int(8.5 * m_scale));
	const int scale_h = tick_h + line_h * 2;
	const int stack_h = 16 * d + scale_h + 8 * d;
	// 点は正方形のままにして、余った幅は左右に振り分ける
	const int x0 = m_lcd.left + std::max(pad, (aw - top_dots * d) / 2);
	const int y0 = m_lcd.top + std::max(pad, (ah - stack_h) / 2);

	HBRUSH ghost = CreateSolidBrush(LCD_GHOST);
	HBRUSH faint = CreateSolidBrush(RGB(147, 202, 45));    // 絵の区画の消え点
	HBRUSH lit   = CreateSolidBrush(LCD_DOT);

	// 1 マスぶんの点を描く。step は点の間隔、size は点の大きさ
	auto cell = [&](int row, int col, int px, int py, int step, int size, HBRUSH back) {
		const u8 *c = s.dots + (row * LCD_COLS + col) * CELL_H;
		for (int y = 0; y < CELL_H; y++)
			for (int x = 0; x < CELL_W; x++) {
				RECT r;
				r.left   = px + x * step;
				r.top    = py + y * step;
				r.right  = r.left + size;
				r.bottom = r.top  + size;
				FillRect(dc, &r, (s.lcd_on && BIT(c[y], 4 - x)) ? lit : back);
			}
	};

	// ---- 上の面。メータ 9 マス ＋ 文字 8 桁。行のあいだは空けない
	for (int row = 0; row < LCD_ROWS; row++)
		for (int col = 0; col < TOP_COLS; col++)
			cell(row, col, x0 + col * (CELL_W + 1) * d, y0 + row * CELL_H * d, d, dot, ghost);

	// ---- 窓に印刷されている目盛り。A1 A2 と 1-32 で、バー 1 本に 1 目盛り。
	// 実機は数字を全部刷ってあるが、この大きさだと潰れるので節目だけにする
	const int scale_y = y0 + 16 * d;
	{
		HPEN p = CreatePen(PS_SOLID, 1, RGB(90, 118, 38));
		HGDIOBJ op = SelectObject(dc, p);
		const int tick = tick_h;
		for (int i = 0; i < TOP_COLS * 2; i++) {
			// i 本目のバーは、マス i/2 の左半分か右半分
			const int col = i / 2;
			const int bx  = x0 + col * (CELL_W + 1) * d + ((i & 1) ? 3 * d : 0) + d / 2;
			MoveToEx(dc, bx, scale_y, nullptr);
			LineTo(dc, bx, scale_y + tick);

			const int part = i - 1;                 // A1 A2 のぶんを引く
			if (i >= 2 && part != 1 && part % 5 != 0)
				continue;
			char n[8];
			std::snprintf(n, sizeof(n), i < 2 ? "A%d" : "%d", i < 2 ? i + 1 : part);
			RECT t{ bx - int(8 * m_scale), scale_y + tick,
			        bx + int(8 * m_scale), scale_y + tick + line_h };
			text_in(dc, t, n, RGB(70, 92, 30), m_font_small,
			        DT_CENTER | DT_TOP | DT_SINGLELINE);
		}
		SelectObject(dc, op);
		DeleteObject(p);

		// MIC は目盛りの帯の下の段、左端。実機もここに小さく出ている
		const int line2 = scale_y + tick + line_h;
		RECT mic{ x0, line2, x0 + int(20 * m_scale), line2 + line_h };
		text_in(dc, mic, "MIC", RGB(70, 92, 30), m_font_small,
		        DT_LEFT | DT_TOP | DT_SINGLELINE);

		// BANK と PGM# は、文字 2 行目の桁に合わせて印刷されている
		RECT b{ x0 + 10 * (CELL_W + 1) * d, line2, x0 + 14 * (CELL_W + 1) * d,
		        line2 + line_h };
		text_in(dc, b, "BANK", RGB(70, 92, 30), m_font_small, DT_LEFT | DT_TOP | DT_SINGLELINE);
		RECT g{ x0 + 14 * (CELL_W + 1) * d, line2, x0 + 18 * (CELL_W + 1) * d,
		        line2 + line_h };
		text_in(dc, g, "PGM#", RGB(70, 92, 30), m_font_small, DT_LEFT | DT_TOP | DT_SINGLELINE);
	}

	// ---- 下の面
	const int sy = scale_y + scale_h;
	const int seg_h = 8 * d;                       // 文字 1 行ぶんの高さ
	auto lx = [&](int which) { return x0 + LOW_X[which] * d; };
	auto lw = [&](int which) { return LOW_W[which] * d; };

	// 部の番号「01」と「A01」。塊の中の桁は**詰めて**並べる
	for (int i = 0; i < 2; i++)
		cell(0, TOP_COLS + i, lx(LOW_PART) + i * CELL_W * d, sy, d, dot, ghost);
	for (int i = 0; i < 3; i++)
		cell(1, TOP_COLS + i, lx(LOW_BANK) + i * CELL_W * d, sy, d, dot, ghost);

	// 楽器のかたち。20-23 桁の両行が 1 枚の絵になっている。
	// **点が文字より細かく、正方形でもない**。横は 20 点ぶんを LOW_W ぶんに、
	// 縦は 16 点ぶんを文字 1 行（8 点）に詰める
	{
		const int ix = lx(LOW_ICON), iw = lw(LOW_ICON);
		const int cols = LCD_COLS - (TOP_COLS + 3);
		const int nx = cols * CELL_W, ny = LCD_ROWS * CELL_H;
		for (int row = 0; row < LCD_ROWS; row++)
			for (int col = TOP_COLS + 3; col < LCD_COLS; col++) {
				const u8 *c = s.dots + (row * LCD_COLS + col) * CELL_H;
				for (int y = 0; y < CELL_H; y++) {
					const int yy = row * CELL_H + y;
					RECT r;
					r.top    = sy + yy * seg_h / ny;
					r.bottom = sy + (yy + 1) * seg_h / ny;
					if (r.bottom <= r.top)
						r.bottom = r.top + 1;
					for (int x = 0; x < CELL_W; x++) {
						const int xx = (col - TOP_COLS - 3) * CELL_W + x;
						r.left  = ix + xx * iw / nx;
						r.right = ix + (xx + 1) * iw / nx;
						if (r.right <= r.left)
							r.right = r.left + 1;
						FillRect(dc, &r, (s.lcd_on && BIT(c[y], 4 - x)) ? lit : faint);
					}
				}
			}
	}

	// ---- ここから右は、実機では独立したセグメント。HD44780 には流れて
	// こない（MAME もこの区画を持っていない）ので、**消えている形だけ**を
	// 薄く描く。実物も消えた区画はうっすら見えるので、見た目としては近い
	{
		HBRUSH off = CreateSolidBrush(LCD_GHOST);
		HPEN   pen = CreatePen(PS_SOLID, std::max(1, int(1.5 * m_scale)), LCD_GHOST);
		HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
		HGDIOBJ op = SelectObject(dc, pen);

		const int t = std::max(1, int(1.6 * m_scale));

		// 7 セグメントの「8」。消えている桁はこう見える
		auto digit = [&](int x, int w) {
			const int top = sy, h = seg_h, mid = top + h / 2;
			RECT r;
			r = { x, top, x + w, top + t };                  FillRect(dc, &r, off);
			r = { x, mid - t / 2, x + w, mid + t - t / 2 };  FillRect(dc, &r, off);
			r = { x, top + h - t, x + w, top + h };          FillRect(dc, &r, off);
			r = { x, top, x + t, mid };                      FillRect(dc, &r, off);
			r = { x + w - t, top, x + w, mid };              FillRect(dc, &r, off);
			r = { x, mid, x + t, top + h };                  FillRect(dc, &r, off);
			r = { x + w - t, mid, x + w, top + h };          FillRect(dc, &r, off);
		};

		// 縦棒のメータ（VOL と EXP）
		auto bars = [&](int x, int w) {
			for (int i = 0; i < 7; i++) {
				RECT r{ x, sy + seg_h - (i + 1) * seg_h / 8,
				        x + w, sy + seg_h - i * seg_h / 8 - t };
				FillRect(dc, &r, off);
			}
		};

		bars(lx(LOW_VOL), lw(LOW_VOL));
		bars(lx(LOW_EXP), lw(LOW_EXP));

		// パンのつまみ
		{
			const int r  = std::min<int>(seg_h / 2, lw(LOW_PAN) / 2);
			const int cx = lx(LOW_PAN) + lw(LOW_PAN) / 2, cy = sy + seg_h / 2;
			Ellipse(dc, cx - r, cy - r, cx + r, cy + r);
			MoveToEx(dc, cx, cy, nullptr);
			LineTo(dc, cx, cy - r + 1);
		}

		// リバーブ・コーラス・バリエーションの弧。上半分だけ描く
		for (int k = 0; k < 3; k++) {
			const int w  = lw(LOW_REV + k);
			const int r  = std::min<int>(seg_h - int(2 * m_scale), w / 2);
			const int cx = lx(LOW_REV + k) + w / 2;
			const int cy = sy + seg_h - std::max(1, int(m_scale));
			Arc(dc, cx - r, cy - r, cx + r, cy + r, cx + r, cy, cx - r, cy);
		}

		// キー。1 文字目は ＋ と − だけのセグメント、あと 2 桁は 7 セグ
		{
			const int kx = lx(LOW_KEY);
			const int cy = sy + seg_h / 2;
			RECT h{ kx, cy - t / 2, kx + 3 * d, cy + t - t / 2 };
			FillRect(dc, &h, off);
			RECT v{ kx + 3 * d / 2 - t / 2, cy - 3 * d / 2,
			        kx + 3 * d / 2 + t - t / 2, cy + 3 * d / 2 };
			FillRect(dc, &v, off);
			digit(kx + 4 * d, 3 * d);
			digit(kx + 8 * d, 3 * d);
		}

		// いちばん右。XG / TG300B / PERFORM のどれかを ▶ で示す。
		// パネルに印刷されている 3 つの札と高さを合わせてある
		{
			const int mx = lx(LOW_MODE), mw = lw(LOW_MODE);
			for (int k = 0; k < 3; k++) {
				const int cy = sy + seg_h * (2 * k + 1) / 6;
				const int hh = std::max(2, seg_h / 8);
				const POINT tri[4] = { { mx, cy - hh }, { mx + mw, cy },
				                       { mx, cy + hh }, { mx, cy - hh } };
				Polyline(dc, tri, 4);
			}
		}

		SelectObject(dc, ob);
		SelectObject(dc, op);
		DeleteObject(off);
		DeleteObject(pen);
	}

	DeleteObject(ghost);
	DeleteObject(faint);
	DeleteObject(lit);

	if (s.message[0]) {
		RECT r = m_lcd;
		fill(dc, r, RGB(24, 26, 22));
		text_in(dc, r, s.message, RGB(210, 220, 200), m_font_label,
		        DT_CENTER | DT_VCENTER | DT_WORDBREAK);
	}
}

void panel::draw_button(HDC dc, const spot &sp, bool down) const
{
	round_box(dc, sp.r, down ? KEY_DOWN : KEY_FACE, KEY_EDGE, int(3 * m_scale));
}

// 大きなダイヤル。回した角度で窪みが回る
void panel::draw_wheel(HDC dc, int angle) const
{
	const POINT c = at(DIAL_X, DIAL_Y);
	const int r = int(DIAL_R * m_scale);

	disc(dc, c.x, c.y, r, KEY_FACE, KEY_EDGE, std::max(1, int(2 * m_scale)));
	const double a = angle * PI / 180.0;
	const int ox = c.x + int(std::sin(a) * r * 0.36);
	const int oy = c.y - int(std::cos(a) * r * 0.36);
	disc(dc, ox, oy, int(r * 0.45), RGB(186, 176, 140), RGB(146, 137, 106),
	     std::max(1, int(m_scale)));
}

// 音量つまみ
void panel::draw_volume(HDC dc, double v) const
{
	const POINT c = at(141, 154);
	const int r = int(30 * m_scale);
	disc(dc, c.x, c.y, r, KEY_FACE, KEY_EDGE, std::max(1, int(m_scale)));
	const double a = (-135.0 + 270.0 * v) * PI / 180.0;
	line(dc, c.x, c.y, c.x + int(std::sin(a) * r * 0.8), c.y - int(std::cos(a) * r * 0.8),
	     RGB(70, 64, 48), std::max(2, int(2 * m_scale)));
	text_in(dc, scale(105, 188, 72, 12), "VOLUME", PANEL_INK, m_font_small,
	        DT_CENTER | DT_TOP | DT_SINGLELINE);
}


void panel::paint_front(HDC dc, const snapshot &s, u64 pressed, double volume,
                        const char *status) const
{
	RECT all{ 0, 0, m_w, m_h };
	fill(dc, all, RGB(24, 26, 30));
	fill(dc, scale(0, 0, LOGICAL_W, BODY_H), PANEL_FACE);

	// ---- 左

	text_in(dc, scale(10, 6, 170, 26), "YAMAHA", PANEL_INK, m_font_label,
	        DT_LEFT | DT_VCENTER | DT_SINGLELINE);
	text_in(dc, scale(227, 6, 320, 26), "MU2000    TONE GENERATOR", PANEL_INK,
	        m_font_label, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
	text_in(dc, scale(744, 8, 60, 20), "USB", PANEL_INK, m_font_small,
	        DT_LEFT | DT_VCENTER | DT_SINGLELINE);

	for (int i = 0; i < 2; i++) {
		const POINT c = at(32, 74.0 + i * 75);
		disc(dc, c.x, c.y, int(19 * m_scale), RGB(52, 48, 42), RGB(120, 114, 100),
		     std::max(1, int(2 * m_scale)));
	}
	text_in(dc, scale(56, 106, 130, 14), "1 ....  A/D INPUT", PANEL_INK, m_font_small,
	        DT_LEFT | DT_TOP | DT_SINGLELINE);
	text_in(dc, scale(56, 182, 130, 14), "2 ....", PANEL_INK, m_font_small,
	        DT_LEFT | DT_TOP | DT_SINGLELINE);

	// A/D INPUT のつまみ。音は通していないので飾り
	{
		const POINT c = at(141, 78);
		disc(dc, c.x, c.y, int(30 * m_scale), KEY_FACE, KEY_EDGE, std::max(1, int(m_scale)));
	}

	round_box(dc, scale(8, 248, 68, 36), KEY_FACE, KEY_EDGE, int(3 * m_scale));
	text_in(dc, scale(4, 288, 96, 24), "STANDBY / ON", PANEL_INK, m_font_small,
	        DT_LEFT | DT_TOP | DT_WORDBREAK);
	{
		const POINT c = at(130, 266);
		disc(dc, c.x, c.y, int(34 * m_scale), RGB(60, 56, 50), RGB(120, 114, 100),
		     std::max(1, int(2 * m_scale)));
	}
	text_in(dc, scale(96, 304, 90, 14), "MIDI IN A", PANEL_INK, m_font_small,
	        DT_CENTER | DT_TOP | DT_SINGLELINE);
	{
		const POINT c = at(228, 269);
		disc(dc, c.x, c.y, int(14 * m_scale), RGB(52, 48, 42), RGB(120, 114, 100),
		     std::max(1, int(m_scale)));
	}
	text_in(dc, scale(198, 304, 60, 14), "PHONES", PANEL_INK, m_font_small,
	        DT_CENTER | DT_TOP | DT_SINGLELINE);
	round_box(dc, scale(57, 336, 201, 21), RGB(120, 116, 104), RGB(90, 86, 76),
	          int(2 * m_scale));
	text_in(dc, scale(63, 338, 130, 17), "3.3V CARD", RGB(232, 228, 218), m_font_small,
	        DT_LEFT | DT_VCENTER | DT_SINGLELINE);

	// ---- 中

	draw_lcd(dc, s);

	text_in(dc, scale(686, 44, 46, 84), "GM2\nXG\nPLG", PANEL_INK, m_font_small,
	        DT_CENTER | DT_TOP | DT_WORDBREAK);
	text_in(dc, scale(686, 132, 50, 48), "XG\nTG300B\nPERFORM", PANEL_INK,
	        m_font_small, DT_LEFT | DT_TOP | DT_WORDBREAK);

	// 窓の下の札は、下段の並びと同じ割合で置く。窓の中身とずれないように
	{
		const int pad = std::max(2, int(5 * m_scale));
		const int top_dots = TOP_COLS * (CELL_W + 1) - 1;
		const int aw = m_lcd.right - m_lcd.left;
		const int d = std::max<int>(1, (aw - pad * 2) / top_dots);
		const int inner_x = m_lcd.left + std::max(pad, (aw - top_dots * d) / 2);
		const int y = at(0, 186).y, h = int(12 * m_scale), w = int(64 * m_scale);
		for (const column &c : COLUMNS) {
			const int cx = inner_x + (LOW_X[c.at] + LOW_W[c.at] / 2) * d;
			RECT r{ cx - w / 2, y, cx + w / 2, y + h };
			text_in(dc, r, c.label, PANEL_INK, m_font_small,
			        DT_CENTER | DT_TOP | DT_SINGLELINE);
		}
	}

	for (int i = 0; i < 18; i++)
		text_in(dc, scale(CAT_X[i % 6] - 34, CAT_Y[i / 6] - 14, 68, 14), CAT_LABEL[i],
		        PANEL_INK, m_font_small, DT_CENTER | DT_TOP | DT_SINGLELINE);

	// MU / PLG-1..3 の表示灯。LED は 6 番から
	{
		const char *plg[4] = { "MU", "PLG-1", "PLG-2", "PLG-3" };
		for (int i = 0; i < 4; i++) {
			const POINT c = at(524.0 + i * 37, 341);
			disc(dc, c.x, c.y, int(5 * m_scale),
			     BIT(s.leds, 6 + i) ? LED_ON : RGB(64, 62, 52), RGB(110, 106, 92), 1);
			text_in(dc, scale(502.0 + i * 37, 348, 44, 12), plg[i], PANEL_INK,
			        m_font_small, DT_CENTER | DT_TOP | DT_SINGLELINE);
		}
	}

	// ---- 右

	for (size_t i = 0; i < sizeof(MODES) / sizeof(MODES[0]); i++) {
		const mode_button &m = MODES[i];
		text_in(dc, scale(m.x - 34, m.y - 30, 68, 14), m.label, PANEL_INK,
		        m_font_small, DT_CENTER | DT_TOP | DT_SINGLELINE);
		const POINT c = at(m.x, m.y);
		const bool down = ((pressed >> int(m.b)) & 1) != 0;
		disc(dc, c.x, c.y, int(11 * m_scale), down ? KEY_DOWN : RGB(198, 188, 152),
		     KEY_EDGE, std::max(1, int(m_scale)));
		disc(dc, c.x, c.y, int(5 * m_scale),
		     BIT(s.leds, m.led) ? LED_ON : RGB(74, 72, 60), RGB(110, 106, 92), 1);
	}

	text_in(dc, scale(896, 28, 104, 12), "······ ALL ······", PANEL_INK,
	        m_font_small, DT_CENTER | DT_TOP | DT_SINGLELINE);

	// 四角いボタン。名札は上に重ねる
	for (const place &p : NAV) {
		const spot *sp = nullptr;
		for (const spot &q : m_spots)
			if (q.kind == spot_kind::button && q.button == p.b) { sp = &q; break; }
		if (!sp)
			continue;
		draw_button(dc, *sp, ((pressed >> int(p.b)) & 1) != 0);
		text_in(dc, scale(p.x, p.y + 4, p.w, 12), p.label, RGB(58, 53, 38),
		        m_font_small, DT_CENTER | DT_TOP | DT_SINGLELINE);
		if (p.sub[0])
			text_in(dc, scale(p.x, p.y + p.h - 14, p.w, 12), p.sub, RGB(58, 53, 38),
			        m_font_small, DT_CENTER | DT_TOP | DT_SINGLELINE);
	}
	for (int i = 0; i < 18; i++) {
		RECT r = scale(CAT_X[i % 6] - CAT_W / 2, CAT_Y[i / 6], CAT_W, CAT_H);
		round_box(dc, r, ((pressed >> int(CAT_B[i])) & 1) ? KEY_DOWN : KEY_FACE,
		          KEY_EDGE, int(3 * m_scale));
	}
	for (const place &p : ROUND) {
		const POINT c = at(p.x, p.y);
		disc(dc, c.x, c.y, int(p.w / 2 * m_scale),
		     ((pressed >> int(p.b)) & 1) ? KEY_DOWN : KEY_FACE, KEY_EDGE,
		     std::max(1, int(m_scale)));
		text_in(dc, scale(p.x - 40, p.y - 26, 80, 12), p.label, PANEL_INK,
		        m_font_small, DT_CENTER | DT_TOP | DT_SINGLELINE);
	}

	draw_wheel(dc, m_wheel_angle);
	draw_volume(dc, volume);

	if (status && status[0])
		text_in(dc, m_status, status, RGB(170, 174, 180), m_font_small,
		        DT_LEFT | DT_VCENTER | DT_SINGLELINE);
	text_in(dc, m_hint,
	        "大きなダイヤルはホイールで回す ／ ボタンはクリック ／ "
	        "キー: A=PLAY E=EDIT U=UTIL F=EFFECT [ ]=PART",
	        RGB(120, 124, 130), m_font_small, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

	draw_tabs(dc);
}

void panel::paint(HDC dc, const snapshot &s, u64 pressed, const char *status) const
{
	if (m_page == page::editor)       paint_editor(dc, status);
	else if (m_page == page::effects) paint_effects(dc, status);
	else                              paint_front(dc, s, pressed, m_volume_now, status);
}

} // namespace ui
