// license:BSD-3-Clause

#include "overview.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "xg/fx_types.h"
#include "xg/ram.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ui {

using namespace xgui;

namespace {

constexpr int PARTS = 32;

enum class src { param, exp, mod, bend, hold };

ImU32 col(ImGuiCol c, float a = 1.0f) { return ImGui::GetColorU32(c, a); }

// 押さえている鍵の色。VEL メーターと同じ
const ImU32 NOTE_ON = IM_COL32(236, 116, 70, 255);

// マスターの鍵盤でのパートの色。32 色を色相で振る（隣のパートが似ないよう 7 つ飛ばし）
ImU32 part_color(int part)
{
	float r, g, b;
	ImGui::ColorConvertHSVtoRGB(float((part * 7) % 32) / 32.0f, 0.75f, 1.0f, r, g, b);
	return IM_COL32(int(r * 255), int(g * 255), int(b * 255), 255);
}

// 鍵盤の上の点が、どの鍵か。黒鍵を先に見る。外なら -1。vel に強さ（下ほど強い）
int key_at(ImVec2 pos, float w, float h, ImVec2 at, int &vel)
{
	const float fs = ImGui::GetFontSize();
	const float pad = fs * 0.2f;
	const float top = pos.y + pad, bottom = pos.y + h - pad;
	static const bool BLACK[12] = { 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0 };
	static const float WHITE_POS[12] = { 0, 0.6f, 1, 1.6f, 2, 3, 3.6f, 4, 4.6f, 5, 5.6f, 6 };
	const float kw = (w - pad * 2) / 75;
	const float left = pos.x + pad;
	if (at.y < top || at.y > bottom || at.x < left || at.x > left + kw * 75)
		return -1;
	const float frac = (at.y - top) / std::max(1.0f, bottom - top);
	vel = std::clamp(int(30 + 97 * frac), 1, 127);
	if (at.y < top + (bottom - top) * 0.6f) {
		for (int note = 0; note < 128; note++) {
			if (!BLACK[note % 12]) continue;
			const float x = left + (note / 12 * 7 + WHITE_POS[note % 12]) * kw;
			if (at.x >= x && at.x < x + kw * 0.8f)
				return note;
		}
	}
	for (int note = 0; note < 128; note++) {
		if (BLACK[note % 12]) continue;
		const float x = left + (note / 12 * 7 + WHITE_POS[note % 12]) * kw;
		if (at.x >= x && at.x < x + kw)
			return note;
	}
	return -1;
}

// 128 鍵の鍵盤。color は鍵ごとの色（0 なら押さえていない）
template <typename F>
void draw_keys(ImDrawList *dl, ImVec2 pos, float w, float h, F color)
{
	const float fs = ImGui::GetFontSize();
	const float pad = fs * 0.2f;
	const float top = pos.y + pad, bottom = pos.y + h - pad;
	static const bool BLACK[12] = { 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0 };
	static const float WHITE_POS[12] = { 0, 0.6f, 1, 1.6f, 2, 3, 3.6f, 4, 4.6f, 5, 5.6f, 6 };
	constexpr int WHITES = 75;                    // 0-127 の白鍵
	const float kw = (w - pad * 2) / WHITES;
	const float left = pos.x + pad;
	for (int note = 0; note < 128; note++) {
		if (BLACK[note % 12]) continue;
		const float x = left + (note / 12 * 7 + WHITE_POS[note % 12]) * kw;
		const ImU32 c = color(note);
		dl->AddRectFilled(ImVec2(x, top), ImVec2(x + kw - 1, bottom), c ? c : IM_COL32(220, 220, 215, 255));
	}
	for (int note = 0; note < 128; note++) {
		if (!BLACK[note % 12]) continue;
		const float x = left + (note / 12 * 7 + WHITE_POS[note % 12]) * kw;
		const ImU32 c = color(note);
		dl->AddRectFilled(ImVec2(x, top), ImVec2(x + kw * 0.8f, top + (bottom - top) * 0.6f), c ? c : IM_COL32(30, 30, 32, 255));
	}
}

} // namespace

struct overview::column {
	const char *title;
	src from;
	const char *key;      // from が param のとき
};

// 列の並び。Domino の並び（VOL EXP PAN P.BEND MOD HOLD CUT RESO REV CHO DLY）に倣い、
// DLY の代わりに XG の VAR（バリエーションの送り）
static const overview::column COLUMNS[] = {
	{ "VOL",    src::param, "part.volume" },
	{ "EXP",    src::exp,   nullptr },
	{ "PAN",    src::param, "part.pan" },
	{ "P.BEND", src::bend,  nullptr },
	{ "MOD",    src::mod,   nullptr },
	{ "HOLD",   src::hold,  nullptr },
	{ "CUT",    src::param, "part.cutoff" },
	{ "RESO",   src::param, "part.resonance" },
	{ "REV",    src::param, "part.reverb_send" },
	{ "CHO",    src::param, "part.chorus_send" },
	{ "VAR",    src::param, "part.variation_send" },
};
static constexpr int NCOLS = int(sizeof(COLUMNS) / sizeof(COLUMNS[0]));

// マスターの行で、その列に出すもの。無ければ空欄
static const char *master_key(const char *title)
{
	if (!std::strcmp(title, "VOL")) return "system.master_volume";
	if (!std::strcmp(title, "REV")) return "reverb.return";
	if (!std::strcmp(title, "CHO")) return "chorus.return";
	if (!std::strcmp(title, "VAR")) return "variation.return";
	return nullptr;
}


void overview::cell(const column &c, int part, xg::model &m, const xg_snapshot &ram, bridge &br,
                    float w, float h)
{
	ImGuiIO &io = ImGui::GetIO();
	const float fs = ImGui::GetFontSize();

	// part が -1 ならマスターの行。列ごとに、システムやエフェクトの戻りの値を出す
	const bool master = part < 0;
	src from = c.from;
	const char *key = c.key;
	if (master) {
		key = master_key(c.title);
		from = src::param;
		if (!key) {
			ImGui::Dummy(ImVec2(w, h));
			return;
		}
	}
	const int at = master ? 0 : part;
	const u8 *blk = master ? nullptr : ram.parts[part];

	// バリエーションの接続が INSERTION のとき、送り（パートの VAR）も戻り（マスターの VAR）も
	// 使われない。触れはするが薄く出す
	int conn = 1;
	const bool dim = !std::strcmp(c.title, "VAR") && m.get(P("variation.connect"), 0, conn) && conn == 0;

	// 値と、見せ方
	int v = 0, lo = 0, hi = 127;
	bool known = true, bipolar = false, editable = false;
	std::string text;
	const xg::param *p = from == src::param ? &P(key) : nullptr;
	switch (from) {
	case src::param:
		known = m.get(*p, at, v);
		lo = p->min; hi = p->max;
		bipolar = p->how == xg::view::center || p->how == xg::view::pan;
		editable = known;
		text = known ? xg::format(*p, v) : "--";
		break;
	case src::exp:  v = blk[xg::ram::PART_EXP] & 0x7f; text = std::to_string(v); break;
	case src::mod:  v = blk[xg::ram::PART_MOD] & 0x7f; text = std::to_string(v); break;
	case src::bend: {
		// RAM には MSB の半分と、下のバイトの最下位ビットに MSB の残り
		const int msb = (blk[xg::ram::PART_BEND] & 0x3f) * 2 + (blk[xg::ram::PART_BEND + 1] & 1);
		v = msb; bipolar = true;
		char buf[8];
		std::snprintf(buf, sizeof(buf), "%+d", msb - 64);
		text = msb == 64 ? "0" : buf;
		break;
	}
	case src::hold: v = blk[xg::ram::PART_HOLD] ? 127 : 0; text = v ? "ON" : "OFF"; break;
	}

	ImGui::PushID(c.title);
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton("##cell", ImVec2(w, h), ImGuiButtonFlags_MouseButtonLeft);
	const ImGuiID id = ImGui::GetItemID();
	const bool hovered = ImGui::IsItemHovered();
	const bool active = ImGui::IsItemActive();

	int nv = v;
	if (editable) {
		if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
			// 横にも縦にも効く。全域を 200px ほどで（Shift で細かく）
			float &acc = *ImGui::GetStateStorage()->GetFloatRef(id, 0.0f);
			acc += (io.MouseDelta.x - io.MouseDelta.y) * float(hi - lo) / (io.KeyShift ? 800.0f : 200.0f);
			const int step = int(acc);
			if (step) { nv = std::clamp(nv + step, lo, hi); acc -= float(step); }
		}
		if (ImGui::IsItemDeactivated())
			ImGui::GetStateStorage()->SetFloat(id, 0.0f);
		if (hovered && ImGui::GetTime() - m_scrolled_at > 0.5) {
			ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
			if (io.MouseWheel != 0.0f) {
				nv = std::clamp(nv + (io.MouseWheel > 0 ? 1 : -1) * (io.KeyCtrl ? 10 : 1), lo, hi);
				m_wheel_taken = true;
			}
		}
		if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
			ImGui::OpenPopup("##type");
		if (ImGui::BeginPopup("##type")) {
			ImGui::TextDisabled("%s %s（%d-%d）", master ? "MASTER" : part_name(part).c_str(), p->label, lo, hi);
			int &typed = *ImGui::GetStateStorage()->GetIntRef(ImGui::GetID("typed"), v);
			if (ImGui::IsWindowAppearing()) { typed = v; ImGui::SetKeyboardFocusHere(); }
			ImGui::SetNextItemWidth(fs * 6);
			if (ImGui::InputInt("##n", &typed, 1, 10, ImGuiInputTextFlags_EnterReturnsTrue)) {
				nv = std::clamp(typed, lo, hi);
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndPopup();
		}
		if (nv != v) {
			br.send(m.set(*p, at, nv));
			text = xg::format(*p, nv);
		}
	}

	// 描く。上に棒、下に数
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const float pad = fs * 0.2f;
	const float bar_h = std::max(3.0f, fs * 0.45f);
	const ImVec2 b0(pos.x + pad, pos.y + pad);
	const ImVec2 b1(pos.x + w - pad, b0.y + bar_h);
	dl->AddRectFilled(b0, b1, col(ImGuiCol_FrameBg));
	if (known && hi > lo) {
		const float frac = std::clamp(float(nv - lo) / float(hi - lo), 0.0f, 1.0f);
		ImU32 fill = editable ? col(active || hovered ? ImGuiCol_SliderGrabActive : ImGuiCol_SliderGrab)
		                      : IM_COL32(200, 70, 60, 255);
		if (dim)
			fill = col(ImGuiCol_TextDisabled, 0.5f);
		const float x = b0.x + (b1.x - b0.x) * frac;
		if (bipolar) {
			const float mid = (b0.x + b1.x) * 0.5f;
			dl->AddRectFilled(ImVec2(std::min(mid, x) - 1, b0.y), ImVec2(std::max(mid, x) + 1, b1.y), fill);
		} else {
			dl->AddRectFilled(b0, ImVec2(x, b1.y), fill);
		}
	}
	if (hovered)
		dl->AddRect(ImVec2(pos.x + 1, pos.y + 1), ImVec2(pos.x + w - 1, pos.y + h - 1), col(ImGuiCol_Border));
	const ImVec2 ts = ImGui::CalcTextSize(text.c_str());
	dl->AddText(ImVec2(pos.x + w - pad - ts.x, b1.y + (pos.y + h - b1.y - ts.y) * 0.5f),
	            known && !dim ? col(ImGuiCol_Text) : col(ImGuiCol_TextDisabled), text.c_str());

	if (hovered && !active) {
		const char *what = master ? p->label : c.title;
		if (dim)
			ImGui::SetItemTooltip("%s  %s\nバリエーションの接続が INSERTION なので、この値は使われない", what, text.c_str());
		else
			ImGui::SetItemTooltip(editable ? "%s  %s\n左右か上下にドラッグ・ホイール・ダブルクリックで打つ"
			                               : "%s  %s\n演奏の値（表示だけ）", what, text.c_str());
	}
	ImGui::PopID();
}



namespace {

// INS 列で扱うエフェクト。1-4 がインサーション、5 がバリエーション（接続が INSERTION のとき）
struct fx_slot { int id; const char *mark; ImU32 color; const char *part_key; const char *type_key; const char *title; };

const fx_slot FX_SLOTS[] = {
	{ 1, "1", IM_COL32(214, 160, 48, 255),  "insertion1.part", "insertion1.type", "インサーション 1" },
	{ 2, "2", IM_COL32(214, 160, 48, 255),  "insertion2.part", "insertion2.type", "インサーション 2" },
	{ 3, "3", IM_COL32(214, 160, 48, 255),  "insertion3.part", "insertion3.type", "インサーション 3" },
	{ 4, "4", IM_COL32(214, 160, 48, 255),  "insertion4.part", "insertion4.type", "インサーション 4" },
	{ 5, "V", IM_COL32(150, 110, 220, 255), "variation.part",  "variation.type",  "バリエーション" },
};

constexpr const char *DRAG_FX = "S_MU2000_FX";

// そのエフェクトが今どのパートに掛かっているか。掛かっていなければ -1
int fx_target(const fx_slot &f, xg::model &m)
{
	int who = 127, conn = 1;
	if (!m.get(P(f.part_key), 0, who) || who >= 32)
		return -1;
	if (f.id == 5 && (!m.get(P("variation.connect"), 0, conn) || conn != 0))
		return -1;                               // SYSTEM のバリエーションはパートに掛からない
	return who;
}

// エフェクトを別のパートへ（バリエーションは INSERTION にもする）
void fx_move(const fx_slot &f, int part, xg::model &m, bridge &br)
{
	if (f.id == 5)
		br.send(m.set(P("variation.connect"), 0, 0));
	br.send(m.set(P(f.part_key), 0, part));
}

void fx_menu(int part, xg::model &m, bridge &br)
{
	ImGui::TextDisabled("パート %s に掛けるエフェクト", part_name(part).c_str());
	ImGui::Separator();
	for (const fx_slot &f : FX_SLOTS) {
		int type = 0;
		const bool has_type = m.get(P(f.type_key), 0, type);
		const int where = fx_target(f, m);
		char label[128];
		if (f.id == 5 && where < 0)
			std::snprintf(label, sizeof(label), "%s（いま SYSTEM・%s）", f.title, has_type ? xg::fx_name(type).c_str() : "--");
		else
			std::snprintf(label, sizeof(label), "%s（いま %s・%s）", f.title,
			              where >= 0 ? part_name(where).c_str() : "OFF", has_type ? xg::fx_name(type).c_str() : "--");
		if (!ImGui::BeginMenu(label))
			continue;
		if (where == part) {
			if (ImGui::MenuItem(f.id == 5 ? "このパートから外して SYSTEM に戻す" : "このパートから外す")) {
				if (f.id == 5) br.send(m.set(P("variation.connect"), 0, 1));
				else           br.send(m.set(P(f.part_key), 0, 127));
			}
		} else if (ImGui::MenuItem(f.id == 5 ? "INSERTION にしてこのパートに掛ける" : "このパートに掛ける")) {
			fx_move(f, part, m, br);
		}
		ImGui::Separator();
		ImGui::TextDisabled("種類");
		for (const xg::fx_type &t : xg::INS_TYPES) {
			const int value = t.msb << 7 | t.lsb;
			if (ImGui::MenuItem(t.name, nullptr, has_type && value == type))
				br.send(m.set(P(f.type_key), 0, value));
		}
		ImGui::EndMenu();
	}
	ImGui::Separator();
	ImGui::TextDisabled("印をドラッグして、別のパートの INS 欄に落とすと移る。\n種類が NO EFFECT のまま掛けると、そのパートの音が消える");
}

} // namespace


void overview::ins_cell(int part, xg::model &m, bridge &br, float h)
{
	const float fs = ImGui::GetFontSize();
	ImDrawList *dl = ImGui::GetWindowDrawList();

	struct on_part { const fx_slot *slot; std::string name; };
	std::vector<on_part> on;
	for (const fx_slot &f : FX_SLOTS) {
		int type = 0;
		if (fx_target(f, m) == part && m.get(P(f.type_key), 0, type))
			on.push_back({ &f, xg::fx_name(type) });
	}

	const ImVec2 pos = ImGui::GetCursorScreenPos();
	const float w = ImGui::GetContentRegionAvail().x;
	ImGui::SetNextItemAllowOverlap();
	ImGui::InvisibleButton("##ins", ImVec2(w, h), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
	const bool cell_hovered = ImGui::IsItemHovered();
	// 落とし先。別のパートから印を持ってきたら、そのエフェクトをこのパートへ
	if (ImGui::BeginDragDropTarget()) {
		if (const ImGuiPayload *pl = ImGui::AcceptDragDropPayload(DRAG_FX)) {
			const int id = *static_cast<const int *>(pl->Data);
			for (const fx_slot &f : FX_SLOTS)
				if (f.id == id)
					fx_move(f, part, m, br);
		}
		ImGui::EndDragDropTarget();
	}
	if (ImGui::BeginPopupContextItem("fxmenu")) {
		fx_menu(part, m, br);
		ImGui::EndPopup();
	}
	if (cell_hovered && on.empty() && !ImGui::IsDragDropActive())
		ImGui::SetItemTooltip("右クリックでエフェクトを掛ける");

	dl->PushClipRect(pos, ImVec2(pos.x + w, pos.y + h), true);
	const float line = fs * 1.05f;
	for (size_t i = 0; i < on.size() && i < 2; i++) {
		const float y = pos.y + fs * 0.1f + line * float(i);
		const float bw = fs * 1.0f;
		const fx_slot &f = *on[i].slot;
		std::string name = on[i].name;
		if (i == 1 && on.size() > 2)
			name += " ほか";

		// 印の行はつかめる（ドラッグで移す）
		ImGui::SetCursorScreenPos(ImVec2(pos.x, y));
		ImGui::PushID(f.id);
		ImGui::InvisibleButton("##fx", ImVec2(w, line), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
		const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
		if (ImGui::BeginDragDropSource()) {
			ImGui::SetDragDropPayload(DRAG_FX, &f.id, sizeof(f.id));
			ImGui::Text("%s（%s）を移す", f.title, name.c_str());
			ImGui::EndDragDropSource();
		}
		ImGui::OpenPopupOnItemClick("fxmenu_badge", ImGuiPopupFlags_MouseButtonRight);
		if (ImGui::BeginPopup("fxmenu_badge")) {
			fx_menu(part, m, br);
			ImGui::EndPopup();
		}
		if (ImGui::IsItemHovered() && !ImGui::IsDragDropActive())
			ImGui::SetItemTooltip("%s: %s\nドラッグで別のパートへ・右クリックで種類や外す", f.title, on[i].name.c_str());
		ImGui::PopID();

		dl->AddRectFilled(ImVec2(pos.x + fs * 0.2f, y + 1), ImVec2(pos.x + fs * 0.2f + bw, y + fs), f.color, 3.0f);
		const ImVec2 ms = ImGui::CalcTextSize(f.mark);
		dl->AddText(ImVec2(pos.x + fs * 0.2f + (bw - ms.x) * 0.5f, y), IM_COL32(20, 20, 20, 255), f.mark);
		dl->AddText(ImVec2(pos.x + fs * 1.5f, y), hot ? col(ImGuiCol_SliderGrabActive) : col(ImGuiCol_Text), name.c_str());
	}
	// 落とせる欄を光らせる
	if (cell_hovered && ImGui::GetDragDropPayload() && ImGui::GetDragDropPayload()->IsDataType(DRAG_FX))
		dl->AddRect(ImVec2(pos.x + 1, pos.y + 1), ImVec2(pos.x + w - 1, pos.y + h - 1), col(ImGuiCol_DragDropTarget), 3.0f, 0, 2.0f);
	dl->PopClipRect();
	ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + h));
	ImGui::Dummy(ImVec2(0, 0));
}

void overview::row(int part, xg::model &m, const xg_snapshot &ram, bridge &br, float h)
{
	const float fs = ImGui::GetFontSize();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	ImGui::PushID(part);

	// ---- パートと音色
	ImGui::TableNextColumn();
	{
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const float w = ImGui::GetContentRegionAvail().x;
		if (ImGui::InvisibleButton("##name", ImVec2(w, h), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight))
			m_part = part;
		if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
			m_part = part;
		if (ImGui::BeginPopupContextItem("program")) {
			program_menu(part, m, &ram, br);
			ImGui::EndPopup();
		}
		if (m_part == part)
			dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), col(ImGuiCol_Header));
		else if (ImGui::IsItemHovered())
			dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), col(ImGuiCol_HeaderHovered, 0.4f));

		int msb = 0, lsb = 0, prog = 0, rcv = 0;
		const bool voice = m.get(P("part.bank_msb"), part, msb) && m.get(P("part.bank_lsb"), part, lsb) &&
		                   m.get(P("part.program"), part, prog);
		const bool has_rcv = m.get(P("part.rcv_channel"), part, rcv);
		const std::string name = part_name(part);
		dl->AddText(ImVec2(pos.x + fs * 0.3f, pos.y + fs * 0.1f), col(ImGuiCol_Text), name.c_str());
		// 音色の名前と楽器の絵。利用者の ROM から読めれば MU2000 の本当の名前、
		// 読めなければ GM の名前（xg/voices.h）
		std::string vt = voice ? voice_text(msb, lsb, prog) : "--";
		const xg::voice_rom *vr = voices();
		const u8 *blk = ram.parts[part];
		if (voice && vr) {
			const std::string real = vr->name(blk, msb, prog);
			if (!real.empty()) {
				char buf[40];
				std::snprintf(buf, sizeof(buf), "%3d  %s", prog + 1, real.c_str());
				vt = buf;
			}
		}
		dl->PushClipRect(pos, ImVec2(pos.x + w, pos.y + h), true);
		const float icon_x = pos.x + fs * 2.2f;
		// 実機の LCD に寄せて、横に 2 倍（1 ドットが横 2 : 縦 1）
		const float dot = std::max(1.0f, std::floor((h - fs * 0.3f) / 16.0f));
		const float dot_w = dot * 2;
		u16 rows[16];
		if (vr) {
			// パネルの LCD と同じ色（draw.h の LCD_BACK / LCD_GHOST / LCD_DOT）。
			// 絵の無いもの（ドラム）も、LCD の枠だけ出して並びを揃える
			static const ImU32 LCD_BACK  = IM_COL32(150, 205, 45, 255);
			static const ImU32 LCD_GHOST = IM_COL32(140, 194, 44, 255);
			static const ImU32 LCD_DOT   = IM_COL32(18, 22, 14, 255);
			const bool has = voice && vr->icon(blk, msb, prog, rows);
			const float top = pos.y + (h - dot * 16) * 0.5f;
			const float frame = std::max(1.0f, dot);
			dl->AddRectFilled(ImVec2(icon_x - frame, top - frame),
			                  ImVec2(icon_x + dot_w * 16 + frame, top + dot * 16 + frame), LCD_BACK, 2.0f);
			for (int y = 0; y < 16; y++)
				for (int x = 0; x < 16; x++) {
					const bool on = has && ((rows[y] >> (15 - x)) & 1);
					dl->AddRectFilled(ImVec2(icon_x + x * dot_w, top + y * dot),
					                  ImVec2(icon_x + (x + 1) * dot_w, top + (y + 1) * dot),
					                  on ? LCD_DOT : LCD_GHOST);
				}
		}
		const float text_x = icon_x + dot_w * 16 + fs * 0.4f;
		dl->AddText(ImVec2(text_x, pos.y + fs * 0.1f), col(ImGuiCol_Text), vt.c_str());
		char sub[64];
		std::snprintf(sub, sizeof(sub), "受信 %s   M %d  L %d", has_rcv ? channel_name(rcv).c_str() : "--", msb, lsb);
		dl->AddText(ImVec2(text_x, pos.y + fs * 1.15f), col(ImGuiCol_TextDisabled), sub);
		dl->PopClipRect();
	}

	// ---- インサーション（右クリックで掛ける・外す・種類、印をドラッグして別のパートへ）
	ImGui::TableNextColumn();
	ins_cell(part, m, br, h);

	// 受信チャンネルから、見張りの口×チャンネル
	int rcv = 127;
	m.get(P("part.rcv_channel"), part, rcv);
	const int slot = rcv >= 0 && rcv < 32 ? rcv : -1;

	// ---- VEL メーター
	ImGui::TableNextColumn();
	{
		if (slot >= 0 && ram.note_ons[slot] != m_seen_ons[part]) {
			m_seen_ons[part] = ram.note_ons[slot];
			m_level[part] = std::max(m_level[part], ram.velocity[slot] / 127.0f);
		}
		m_level[part] = std::max(0.0f, m_level[part] - ImGui::GetIO().DeltaTime * 1.6f);
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const float w = ImGui::GetContentRegionAvail().x;
		ImGui::Dummy(ImVec2(w, h));
		const float pad = fs * 0.2f;
		const ImVec2 a(pos.x + pad, pos.y + pad), b(pos.x + w - pad, pos.y + h - pad);
		dl->AddRectFilled(a, b, col(ImGuiCol_FrameBg));
		const float top = b.y - (b.y - a.y) * m_level[part];
		dl->AddRectFilled(ImVec2(a.x, top), b, NOTE_ON);
	}

	// ---- 値の棒
	for (const column &c : COLUMNS) {
		ImGui::TableNextColumn();
		cell(c, part, m, ram, br, ImGui::GetContentRegionAvail().x, h);
	}

	// ---- 鍵盤。128 鍵を全部並べる
	ImGui::TableNextColumn();
	{
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const float w = ImGui::GetContentRegionAvail().x;
		// 押すと鳴らす（左でも右でも）。押したまま横に動かすと鍵が替わる。離すとノートオフ。
		// 送り先はこのパートの受信チャンネル（口 B なら口 B へ）
		ImGui::InvisibleButton("##keys", ImVec2(w, h), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
		const bool down = ImGui::IsItemActive() && slot >= 0 &&
		                  (ImGui::IsMouseDown(ImGuiMouseButton_Left) || ImGui::IsMouseDown(ImGuiMouseButton_Right));
		int vel = 100;
		const int want = down ? key_at(pos, w, h, ImGui::GetIO().MousePos, vel) : -1;
		if (want != m_playing[part]) {
			auto send = [&](const u8 msg[3]) {
				if (m_playing_slot[part] >= 16) br.send_b(msg, 3);
				else                            br.send(msg, 3);
			};
			if (m_playing[part] >= 0) {
				const u8 off[3] = { u8(0x80 | (m_playing_slot[part] & 15)), u8(m_playing[part]), 64 };
				send(off);
			}
			m_playing[part] = want;
			if (want >= 0) {
				m_playing_slot[part] = slot;
				const u8 on[3] = { u8(0x90 | (slot & 15)), u8(want), u8(vel) };
				send(on);
			}
		}
		if (ImGui::IsItemHovered() && !down && slot >= 0)
			ImGui::SetItemTooltip("押すと鳴らす（左右どちらのボタンでも）。下ほど強く");
		draw_keys(dl, pos, w, h, [&](int note) -> ImU32 {
			return slot >= 0 && ((ram.notes[slot][note >> 6] >> (note & 63)) & 1) ? NOTE_ON : 0;
		});
	}

	ImGui::PopID();
}


namespace {

const overview::column &column_of(const char *title)
{
	for (const overview::column &c : COLUMNS)
		if (!std::strcmp(c.title, title))
			return c;
	return COLUMNS[0];
}

// 種類の品書き。今の種類に印
template <size_t N>
void type_menu(const xg::fx_type (&types)[N], const char *key, xg::model &m, bridge &br)
{
	int cur = 0;
	const bool has = m.get(P(key), 0, cur);
	for (const xg::fx_type &t : types) {
		const int value = t.msb << 7 | t.lsb;
		if (ImGui::MenuItem(t.name, nullptr, has && value == cur))
			br.send(m.set(P(key), 0, value));
	}
}

// 掛け先のパートの品書き（A1-B16 と OFF）
void part_menu(const char *key, xg::model &m, bridge &br, bool with_off)
{
	int cur = 127;
	m.get(P(key), 0, cur);
	for (int port = 0; port < 2; port++) {
		if (!ImGui::BeginMenu(port == 0 ? "A1-A16" : "B1-B16"))
			continue;
		for (int i = port * 16; i < port * 16 + 16; i++)
			if (ImGui::MenuItem(part_name(i).c_str(), nullptr, cur == i))
				br.send(m.set(P(key), 0, i));
		ImGui::EndMenu();
	}
	if (with_off && ImGui::MenuItem("OFF（どのパートにも掛けない）", nullptr, cur >= 32))
		br.send(m.set(P(key), 0, 127));
}

} // namespace


// システムのエフェクト（リバーブ・コーラス・バリエーション）の 1 マス。
// 上の行が種類（右クリックで選ぶ）、下が戻り量の棒
template <size_t N>
void overview::system_fx_cell(const char *title, const xg::fx_type (&types)[N], const char *type_key,
                              const char *return_col, bool variation, xg::model &m, const xg_snapshot &ram,
                              bridge &br, float h)
{
	const float fs = ImGui::GetFontSize();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	const float w = ImGui::GetContentRegionAvail().x;
	const float line = fs * 1.1f;

	ImGui::PushID(title);
	ImGui::InvisibleButton("##type", ImVec2(w, line), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
	const bool hot = ImGui::IsItemHovered();
	if (ImGui::BeginPopupContextItem("typemenu", ImGuiPopupFlags_MouseButtonRight)) {
		ImGui::TextDisabled("%s の種類", title);
		ImGui::Separator();
		type_menu(types, type_key, m, br);
		if (variation) {
			int conn = 1;
			m.get(P("variation.connect"), 0, conn);
			ImGui::Separator();
			ImGui::TextDisabled("接続");
			if (ImGui::MenuItem("SYSTEM（全パートから送る）", nullptr, conn == 1))
				br.send(m.set(P("variation.connect"), 0, 1));
			if (ImGui::BeginMenu("INSERTION（1 つのパートに掛ける）")) {
				part_menu("variation.part", m, br, false);
				ImGui::EndMenu();
			}
			if (conn == 0 && ImGui::IsItemHovered())
				ImGui::SetTooltip("選んだパートに掛かる");
		}
		ImGui::EndPopup();
	}
	if (ImGui::IsItemHovered() && !ImGui::IsPopupOpen("typemenu"))
		ImGui::SetItemTooltip("右クリックで種類を選ぶ");

	int type = 0, conn = 1, vpart = 127;
	std::string name = m.get(P(type_key), 0, type) ? xg::fx_name(type) : "--";
	bool dim = false;
	if (variation && m.get(P("variation.connect"), 0, conn) && conn == 0) {
		m.get(P("variation.part"), 0, vpart);
		name += vpart < 32 ? " → " + part_name(vpart) : " → OFF";
		dim = false;
	}
	dl->PushClipRect(pos, ImVec2(pos.x + w, pos.y + line), true);
	if (hot)
		dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + line), col(ImGuiCol_HeaderHovered, 0.35f));
	dl->AddText(ImVec2(pos.x + fs * 0.3f, pos.y + (line - fs) * 0.5f), dim ? col(ImGuiCol_TextDisabled) : col(ImGuiCol_Text), name.c_str());
	dl->PopClipRect();
	ImGui::PopID();

	// 戻り量
	ImGui::SetCursorScreenPos(ImVec2(pos.x, pos.y + line));
	cell(column_of(return_col), -1, m, ram, br, w, h - line);
}


// インサーション（とバリエーション）の 1 マス。上の行が印と種類、下が掛け先。
// 右クリックで種類と掛け先、印をつかんでパートの INS 欄に落とすと掛け先が変わる
void overview::insertion_cell(int slot_index, xg::model &m, bridge &br, float h)
{
	const float fs = ImGui::GetFontSize();
	ImDrawList *dl = ImGui::GetWindowDrawList();
	const fx_slot &f = FX_SLOTS[slot_index];
	const ImVec2 pos = ImGui::GetCursorScreenPos();
	const float w = ImGui::GetContentRegionAvail().x;

	ImGui::PushID(f.id);
	ImGui::InvisibleButton("##slot", ImVec2(w, h), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
	const bool hot = ImGui::IsItemHovered() || ImGui::IsItemActive();
	int type = 0;
	const bool has_type = m.get(P(f.type_key), 0, type);
	const std::string name = has_type ? xg::fx_name(type) : "--";
	const int where = fx_target(f, m);
	if (ImGui::BeginDragDropSource()) {
		ImGui::SetDragDropPayload(DRAG_FX, &f.id, sizeof(f.id));
		ImGui::Text("%s（%s）を掛けるパートの INS 欄へ", f.title, name.c_str());
		ImGui::EndDragDropSource();
	}
	if (ImGui::BeginPopupContextItem("slotmenu", ImGuiPopupFlags_MouseButtonRight)) {
		ImGui::TextDisabled("%s", f.title);
		ImGui::Separator();
		if (ImGui::BeginMenu("種類")) {
			type_menu(xg::INS_TYPES, f.type_key, m, br);
			ImGui::EndMenu();
		}
		if (ImGui::BeginMenu("掛けるパート")) {
			part_menu(f.part_key, m, br, true);
			ImGui::EndMenu();
		}
		ImGui::Separator();
		ImGui::TextDisabled("つかんでパートの INS 欄に落としても掛けられる。\n種類が NO EFFECT のまま掛けると、そのパートの音が消える");
		ImGui::EndPopup();
	}
	if (ImGui::IsItemHovered() && !ImGui::IsDragDropActive())
		ImGui::SetItemTooltip("%s: %s → %s\n右クリックで種類と掛けるパート・つかんでパートの INS 欄へ",
		                      f.title, name.c_str(), where >= 0 ? part_name(where).c_str() : "OFF");
	ImGui::PopID();

	dl->PushClipRect(pos, ImVec2(pos.x + w, pos.y + h), true);
	if (hot)
		dl->AddRectFilled(pos, ImVec2(pos.x + w, pos.y + h), col(ImGuiCol_HeaderHovered, 0.35f));
	const float bw = fs * 1.0f;
	const float y = pos.y + fs * 0.1f;
	const ImU32 badge = where >= 0 ? f.color : col(ImGuiCol_TextDisabled, 0.5f);
	dl->AddRectFilled(ImVec2(pos.x + fs * 0.2f, y + 1), ImVec2(pos.x + fs * 0.2f + bw, y + fs), badge, 3.0f);
	const ImVec2 ms = ImGui::CalcTextSize(f.mark);
	dl->AddText(ImVec2(pos.x + fs * 0.2f + (bw - ms.x) * 0.5f, y), IM_COL32(20, 20, 20, 255), f.mark);
	dl->AddText(ImVec2(pos.x + fs * 1.5f, y), where >= 0 ? col(ImGuiCol_Text) : col(ImGuiCol_TextDisabled), name.c_str());
	const std::string to = where >= 0 ? "→ " + part_name(where) : "OFF";
	dl->AddText(ImVec2(pos.x + fs * 1.5f, y + fs * 1.05f), col(ImGuiCol_TextDisabled), to.c_str());
	dl->PopClipRect();
}


// マスターの表。パートの表とは見出しを分ける
void overview::master_pane(xg::model &m, const xg_snapshot &ram, bridge &br)
{
	const float fs = ImGui::GetFontSize();
	const float h = fs * 2.3f;
	ImDrawList *dl = ImGui::GetWindowDrawList();

	const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_BordersOuterH |
	                              ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX;
	constexpr int NCOL = 10;
	if (!ImGui::BeginTable("master", NCOL, flags))
		return;
	ImGui::TableSetupColumn("マスター", ImGuiTableColumnFlags_WidthFixed, fs * 17);
	ImGui::TableSetupColumn("M.VOL", ImGuiTableColumnFlags_WidthFixed, fs * 3.4f);
	ImGui::TableSetupColumn("REVERB", ImGuiTableColumnFlags_WidthFixed, fs * 7.5f);
	ImGui::TableSetupColumn("CHORUS", ImGuiTableColumnFlags_WidthFixed, fs * 7.5f);
	ImGui::TableSetupColumn("VARIATION", ImGuiTableColumnFlags_WidthFixed, fs * 9.5f);
	ImGui::TableSetupColumn("INS 1", ImGuiTableColumnFlags_WidthFixed, fs * 7);
	ImGui::TableSetupColumn("INS 2", ImGuiTableColumnFlags_WidthFixed, fs * 7);
	ImGui::TableSetupColumn("INS 3", ImGuiTableColumnFlags_WidthFixed, fs * 7);
	ImGui::TableSetupColumn("INS 4", ImGuiTableColumnFlags_WidthFixed, fs * 7);
	ImGui::TableSetupColumn("##mkeys", ImGuiTableColumnFlags_WidthStretch);
	headers_with_help(NCOL);
	ImGui::TableNextRow(0, h);
	ImGui::PushID("master");

	// ---- 名前。移調とマスターチューンも
	ImGui::TableNextColumn();
	{
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const float w = ImGui::GetContentRegionAvail().x;
		ImGui::Dummy(ImVec2(w, h));
		dl->AddText(ImVec2(pos.x + fs * 0.3f, pos.y + fs * 0.1f), col(ImGuiCol_Text), "MASTER");
		int tr = 0x40, tune = 0x400;
		char sub[64];
		if (m.get(P("system.transpose"), 0, tr) && m.get(P("system.master_tune"), 0, tune))
			std::snprintf(sub, sizeof(sub), "Transpose %s   Tune %s",
			              xg::format(P("system.transpose"), tr).c_str(), xg::format(P("system.master_tune"), tune).c_str());
		else
			std::snprintf(sub, sizeof(sub), "--");
		dl->AddText(ImVec2(pos.x + fs * 0.3f, pos.y + fs * 1.15f), col(ImGuiCol_TextDisabled), sub);
	}

	ImGui::TableNextColumn();
	cell(column_of("VOL"), -1, m, ram, br, ImGui::GetContentRegionAvail().x, h);
	ImGui::TableNextColumn();
	system_fx_cell("リバーブ", xg::REV_TYPES, "reverb.type", "REV", false, m, ram, br, h);
	ImGui::TableNextColumn();
	system_fx_cell("コーラス", xg::CHO_TYPES, "chorus.type", "CHO", false, m, ram, br, h);
	ImGui::TableNextColumn();
	system_fx_cell("バリエーション", xg::INS_TYPES, "variation.type", "VAR", true, m, ram, br, h);
	for (int i = 0; i < 4; i++) {
		ImGui::TableNextColumn();
		insertion_cell(i, m, br, h);
	}

	// ---- 鍵盤。全パートで鳴っている鍵を重ねる。色はパートごと、重なったら混ぜる
	ImGui::TableNextColumn();
	{
		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const float w = ImGui::GetContentRegionAvail().x;
		ImGui::Dummy(ImVec2(w, h));
		int slots[PARTS];
		for (int p = 0; p < PARTS; p++) {
			int rcv = 127;
			slots[p] = m.get(P("part.rcv_channel"), p, rcv) && rcv < 32 ? rcv : -1;
		}
		draw_keys(dl, pos, w, h, [&](int note) -> ImU32 {
			int r = 0, g = 0, b = 0, n = 0;
			for (int p = 0; p < PARTS; p++) {
				const int sl = slots[p];
				if (sl < 0 || !((ram.notes[sl][note >> 6] >> (note & 63)) & 1))
					continue;
				const ImU32 c = part_color(p);
				r += (c >> IM_COL32_R_SHIFT) & 0xff;
				g += (c >> IM_COL32_G_SHIFT) & 0xff;
				b += (c >> IM_COL32_B_SHIFT) & 0xff;
				n++;
			}
			return n ? IM_COL32(r / n, g / n, b / n, 255) : 0;
		});
	}
	ImGui::PopID();
	ImGui::EndTable();
}


void overview::draw(xg::model &m, const xg_snapshot &ram, bridge &br)
{
	m_wheel_taken = false;
	const ImGuiViewport *vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(vp->WorkPos);
	ImGui::SetNextWindowSize(vp->WorkSize);
	const ImGuiWindowFlags wf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
	                            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
	ImGui::Begin("overview", nullptr, wf);
	ImGui::PopStyleVar();

	const float fs = ImGui::GetFontSize();
	const float h = fs * 2.3f;

	help_checkbox();

	// マスターの表（見出しは別）。インサーションとバリエーションの設定もここ
	master_pane(m, ram, br);
	ImGui::Spacing();

	const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV |
	                              ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX;
	ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(1, 1));
	if (ImGui::BeginTable("rows", NCOLS + 4, flags)) {
		ImGui::TableSetupScrollFreeze(1, 1);            // 見出しは流さない
		ImGui::TableSetupColumn("パート（右クリックで音色）", ImGuiTableColumnFlags_WidthFixed, fs * 17);
		ImGui::TableSetupColumn("INS", ImGuiTableColumnFlags_WidthFixed, fs * 8.5f);
		ImGui::TableSetupColumn("VEL", ImGuiTableColumnFlags_WidthFixed, fs * 2.2f);
		for (const column &c : COLUMNS)
			ImGui::TableSetupColumn(c.title, ImGuiTableColumnFlags_WidthFixed, fs * 3.4f);
		ImGui::TableSetupColumn("##keys", ImGuiTableColumnFlags_WidthStretch);   // 見出しは要らない
		headers_with_help(NCOLS + 4);

		for (int part = 0; part < PARTS; part++) {
			ImGui::TableNextRow(0, h);
			row(part, m, ram, br, h);
		}
		ImGui::EndTable();
	}
	ImGui::PopStyleVar();
	ImGui::End();

	if (ImGui::GetIO().MouseWheel != 0.0f && !m_wheel_taken)
		m_scrolled_at = ImGui::GetTime();
}

} // namespace ui
