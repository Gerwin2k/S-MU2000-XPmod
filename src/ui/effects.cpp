// license:BSD-3-Clause
//
// エフェクトの面。リバーブ／コーラス／バリエーション（XG のシステム側）と、
// MU2000 が持つインサーション 2 系統。
//
// ここも **音源に手を入れず SysEx を送るだけ**。番地は資料から拾ったのではなく、
// 送って音を測って確かめた。確かめ方は doc/effects.md に書いてある。
//
//   02 01 00  リバーブ種別（MSB, LSB）
//   02 01 0C  リバーブ リターン
//   02 01 20  コーラス種別
//   02 01 2C  コーラス リターン
//   02 01 40  バリエーション種別
//   02 01 5A  バリエーション接続（0 インサーション / 1 システム）
//   02 01 5B  バリエーションを掛けるパート
//   03 00 00  インサーション 1 種別      03 00 0C  そのパート
//   03 01 00  インサーション 2 種別      03 01 0C  そのパート

#include "panel.h"
#include "draw.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ui {

namespace {

struct fx_type { const char *name; u8 msb, lsb; };

// リバーブに置ける種別
const fx_type REV_TYPES[] = {
	{ "NO EFFECT", 0x00, 0x00 }, { "HALL 1", 0x01, 0x00 }, { "HALL 2", 0x01, 0x01 },
	{ "ROOM 1", 0x02, 0x00 },    { "ROOM 2", 0x02, 0x01 }, { "ROOM 3", 0x02, 0x02 },
	{ "STAGE 1", 0x03, 0x00 },   { "STAGE 2", 0x03, 0x01 }, { "PLATE", 0x04, 0x00 },
	{ "WHITE ROOM", 0x10, 0x00 },{ "TUNNEL", 0x11, 0x00 },  { "BASEMENT", 0x13, 0x00 },
};

// コーラスに置ける種別
const fx_type CHO_TYPES[] = {
	{ "NO EFFECT", 0x00, 0x00 }, { "CHORUS 1", 0x41, 0x00 }, { "CHORUS 2", 0x41, 0x01 },
	{ "CHORUS 3", 0x41, 0x02 },  { "CELESTE 1", 0x42, 0x00 },{ "CELESTE 2", 0x42, 0x01 },
	{ "CELESTE 3", 0x42, 0x02 }, { "FLANGER 1", 0x43, 0x00 },{ "FLANGER 2", 0x43, 0x01 },
	{ "FLANGER 3", 0x43, 0x02 },
};

// バリエーションとインサーションに置ける種別。
// 27 個ぜんぶ、1 音ずつ鳴らして別々の音になることを確かめてある
const fx_type INS_TYPES[] = {
	{ "NO EFFECT", 0x00, 0x00 },   { "HALL 1", 0x01, 0x00 },     { "ROOM 1", 0x02, 0x00 },
	{ "STAGE 1", 0x03, 0x00 },     { "PLATE", 0x04, 0x00 },      { "DELAY LCR", 0x05, 0x00 },
	{ "DELAY L,R", 0x06, 0x00 },   { "ECHO", 0x07, 0x00 },       { "CROSS DELAY", 0x08, 0x00 },
	{ "ER 1", 0x09, 0x00 },        { "GATE REVERB", 0x0b, 0x00 },{ "REVERSE GATE", 0x0c, 0x00 },
	{ "THRU", 0x40, 0x00 },        { "CHORUS 1", 0x41, 0x00 },   { "CELESTE 1", 0x42, 0x00 },
	{ "FLANGER 1", 0x43, 0x00 },   { "SYMPHONIC", 0x44, 0x00 },  { "ROTARY SP", 0x45, 0x00 },
	{ "TREMOLO", 0x46, 0x00 },     { "AUTO PAN", 0x47, 0x00 },   { "PHASER 1", 0x48, 0x00 },
	{ "DISTORTION", 0x49, 0x00 },  { "OVERDRIVE", 0x4a, 0x00 },  { "AMP SIM", 0x4b, 0x00 },
	{ "3BAND EQ", 0x4c, 0x00 },    { "2BAND EQ", 0x4d, 0x00 },   { "AUTO WAH", 0x4e, 0x00 },
};

template <size_t N> constexpr int count_of(const fx_type (&)[N]) { return int(N); }

const fx_type *type_table(int ctl, int &n)
{
	switch (ctl) {
	case CTL_REV_TYPE: n = count_of(REV_TYPES); return REV_TYPES;
	case CTL_CHO_TYPE: n = count_of(CHO_TYPES); return CHO_TYPES;
	case CTL_VAR_TYPE:
	case CTL_INS1_TYPE:
	case CTL_INS2_TYPE: n = count_of(INS_TYPES); return INS_TYPES;
	default: n = 0; return nullptr;
	}
}

// 面の並び。1 行が 1 つのブロック
struct fx_row {
	const char *title;
	double y;
	int ctl[3];
	const char *label[3];
	double w[3];
};
const fx_row ROWS[] = {
	{ "REVERB",      44,  { CTL_REV_TYPE,  CTL_REV_RET,   CTL_NONE },
	  { "TYPE", "RETURN", "" }, { 250, 120, 0 } },
	{ "CHORUS",     110,  { CTL_CHO_TYPE,  CTL_CHO_RET,   CTL_NONE },
	  { "TYPE", "RETURN", "" }, { 250, 120, 0 } },
	{ "VARIATION",  176,  { CTL_VAR_TYPE,  CTL_VAR_CONN,  CTL_VAR_PART },
	  { "TYPE", "CONNECT", "PART" }, { 250, 120, 120 } },
	{ "INSERTION 1", 242, { CTL_INS1_TYPE, CTL_INS1_PART, CTL_NONE },
	  { "TYPE", "PART", "" }, { 250, 120, 0 } },
	{ "INSERTION 2", 300, { CTL_INS2_TYPE, CTL_INS2_PART, CTL_NONE },
	  { "TYPE", "PART", "" }, { 250, 120, 0 } },
};

constexpr double COL_X = 150;   // 名札の右端

} // namespace


int panel::fx_limit(int ctl) const
{
	int n = 0;
	if (type_table(ctl, n))
		return n - 1;
	switch (ctl) {
	case CTL_VAR_CONN: return 1;              // 0 インサーション / 1 システム
	case CTL_VAR_PART:
	case CTL_INS1_PART:
	case CTL_INS2_PART: return 16;            // 0-15 がパート、16 は掛けない
	default: return 127;                      // リターン
	}
}

const char *panel::fx_text(int ctl, int v) const
{
	static char buf[32];
	int n = 0;
	if (const fx_type *t = type_table(ctl, n))
		return t[std::clamp(v, 0, n - 1)].name;
	if (ctl == CTL_VAR_CONN)
		return v ? "SYSTEM" : "INSERTION";
	if (ctl == CTL_VAR_PART || ctl == CTL_INS1_PART || ctl == CTL_INS2_PART) {
		if (v >= 16) return "OFF";
		std::snprintf(buf, sizeof(buf), "PART %d", v + 1);
		return buf;
	}
	std::snprintf(buf, sizeof(buf), "%d", v);
	return buf;
}

// XG のパラメータチェンジを 1 つ送る
void panel::send_fx(int ctl, bridge &br)
{
	const int v = m_fx[ctl - CTL_FX_FIRST];

	auto send2 = [&](u8 h, u8 m, u8 l, u8 d1, u8 d2) {
		const u8 msg[10] = { 0xf0, 0x43, 0x10, 0x4c, h, m, l, d1, d2, 0xf7 };
		br.send(msg, 10);
	};
	auto send1 = [&](u8 h, u8 m, u8 l, u8 d) {
		const u8 msg[9] = { 0xf0, 0x43, 0x10, 0x4c, h, m, l, d, 0xf7 };
		br.send(msg, 9);
	};

	int n = 0;
	if (const fx_type *t = type_table(ctl, n)) {
		const fx_type &sel = t[std::clamp(v, 0, n - 1)];
		switch (ctl) {
		case CTL_REV_TYPE:  send2(0x02, 0x01, 0x00, sel.msb, sel.lsb); break;
		case CTL_CHO_TYPE:  send2(0x02, 0x01, 0x20, sel.msb, sel.lsb); break;
		case CTL_VAR_TYPE:  send2(0x02, 0x01, 0x40, sel.msb, sel.lsb); break;
		case CTL_INS1_TYPE: send2(0x03, 0x00, 0x00, sel.msb, sel.lsb); break;
		case CTL_INS2_TYPE: send2(0x03, 0x01, 0x00, sel.msb, sel.lsb); break;
		}
		return;
	}

	const u8 part = u8(v >= 16 ? 0x7f : v);
	switch (ctl) {
	case CTL_REV_RET:   send1(0x02, 0x01, 0x0c, u8(v)); break;
	case CTL_CHO_RET:   send1(0x02, 0x01, 0x2c, u8(v)); break;
	case CTL_VAR_CONN:  send1(0x02, 0x01, 0x5a, u8(v)); break;
	case CTL_VAR_PART:  send1(0x02, 0x01, 0x5b, part);  break;
	case CTL_INS1_PART: send1(0x03, 0x00, 0x0c, part);  break;
	case CTL_INS2_PART: send1(0x03, 0x01, 0x0c, part);  break;
	}
}


void panel::draw_list(HDC dc, const spot &sp) const
{
	const int v = m_fx[sp.ctl - CTL_FX_FIRST];
	round_box(dc, sp.r, RGB(40, 43, 48), RGB(88, 93, 100), int(4 * m_scale));

	const int edge = (sp.r.right - sp.r.left) / 6;
	RECT inner = sp.r;
	inner.left  += edge;
	inner.right -= edge;
	text_in(dc, inner, fx_text(sp.ctl, v), TEXT, m_font_small,
	        DT_CENTER | DT_VCENTER | DT_SINGLELINE);

	RECT l = sp.r, r = sp.r;
	l.right = l.left + edge;
	r.left  = r.right - edge;
	const bool at_min = (v <= 0), at_max = (v >= fx_limit(sp.ctl));
	text_in(dc, l, "<", at_min ? RGB(80, 84, 90) : ACCENT, m_font_small,
	        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
	text_in(dc, r, ">", at_max ? RGB(80, 84, 90) : ACCENT, m_font_small,
	        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}


// 電源投入直後の実機に近い値。ここを起点にする。
// **インサーションのパートは既定で OFF**。種別が NO EFFECT のまま
// パートを割り当てると、そのパートの音が消えてしまう（実測）
void panel::init_effect_values()
{
	m_fx[CTL_REV_TYPE  - CTL_FX_FIRST] = 1;    // HALL 1
	m_fx[CTL_REV_RET   - CTL_FX_FIRST] = 64;
	m_fx[CTL_CHO_TYPE  - CTL_FX_FIRST] = 3;    // CHORUS 3
	m_fx[CTL_CHO_RET   - CTL_FX_FIRST] = 64;
	m_fx[CTL_VAR_TYPE  - CTL_FX_FIRST] = 0;    // NO EFFECT
	m_fx[CTL_VAR_CONN  - CTL_FX_FIRST] = 1;    // SYSTEM
	m_fx[CTL_VAR_PART  - CTL_FX_FIRST] = 16;   // OFF
	m_fx[CTL_INS1_TYPE - CTL_FX_FIRST] = 0;
	m_fx[CTL_INS1_PART - CTL_FX_FIRST] = 16;   // OFF
	m_fx[CTL_INS2_TYPE - CTL_FX_FIRST] = 0;
	m_fx[CTL_INS2_PART - CTL_FX_FIRST] = 16;   // OFF
}

// 画面に出ている値を全部送り直す。音源から読み返す術がないので、
// パネル側で触ったあとに画面と揃えたいときはこれを押す
void panel::send_all_fx(bridge &br)
{
	static const int ORDER[] = {
		CTL_REV_TYPE, CTL_REV_RET, CTL_CHO_TYPE, CTL_CHO_RET,
		CTL_VAR_TYPE, CTL_VAR_CONN, CTL_VAR_PART,
		CTL_INS1_TYPE, CTL_INS1_PART, CTL_INS2_TYPE, CTL_INS2_PART,
	};
	for (int c : ORDER)
		send_fx(c, br);
}

void panel::build_effect_spots()
{
	for (const fx_row &row : ROWS) {
		double x = COL_X;
		for (int i = 0; i < 3; i++) {
			if (row.ctl[i] == CTL_NONE)
				continue;
			m_spots.push_back({ spot_kind::list, mu2000::button::count, row.ctl[i],
			                    scale(x, row.y, row.w[i], 24), row.label[i], "" });
			x += row.w[i] + 24;
		}
	}

	m_spots.push_back({ spot_kind::action, mu2000::button::count, CTL_FX_SEND,
		                    scale(150, 340, 150, 24), "この画面を送り直す", "" });
}

void panel::paint_effects(HDC dc, const char *status) const
{
	RECT all{ 0, 0, m_w, m_h };
	fill(dc, all, BODY);
	RECT top{ 0, 0, m_w, m_oy + int(26 * m_scale) };
	fill(dc, top, BODY_TOP);

	text_in(dc, scale(20, 6, 460, 16),
	        "エフェクト（送っているのは XG のパラメータチェンジ）", TEXT_DIM,
	        m_font_small, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

	for (const fx_row &row : ROWS) {
		text_in(dc, scale(20, row.y + 4, 120, 16), row.title, TEXT, m_font_small,
		        DT_LEFT | DT_TOP | DT_SINGLELINE);
		double x = COL_X;
		for (int i = 0; i < 3; i++) {
			if (row.ctl[i] == CTL_NONE)
				continue;
			text_in(dc, scale(x, row.y - 13, row.w[i], 12), row.label[i], TEXT_DIM,
			        m_font_small, DT_LEFT | DT_TOP | DT_SINGLELINE);
			x += row.w[i] + 24;
		}
	}

	for (const spot &sp : m_spots) {
		if (sp.kind == spot_kind::list)
			draw_list(dc, sp);
		else if (sp.kind == spot_kind::action) {
			round_box(dc, sp.r, BTN_FACE, BTN_EDGE, int(5 * m_scale));
			text_in(dc, sp.r, sp.label, TEXT, m_font_small,
			        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
		}
	}

	text_in(dc, scale(320, 340, 660, 34),
	        "インサーションは掛けたいパートを選ぶと働く。バリエーションは\n"
	        "CONNECT を INSERTION にするとインサーションとして使える。",
	        RGB(104, 109, 116), m_font_small, DT_LEFT | DT_TOP | DT_WORDBREAK);

	if (status && status[0])
		text_in(dc, m_status, status, TEXT_DIM, m_font_small,
		        DT_LEFT | DT_VCENTER | DT_SINGLELINE);

	draw_tabs(dc);
}

} // namespace ui
