// license:BSD-3-Clause

#include "overview.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "xg/fx_types.h"
#include "xg/ram.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
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


void overview::cell(const column &c, int part, xg::model &m, const xg_snapshot &ram, bridge &br,
                    float w, float h)
{
	ImGuiIO &io = ImGui::GetIO();
	const float fs = ImGui::GetFontSize();
	const u8 *blk = ram.parts[part];

	// 値と、見せ方
	int v = 0, lo = 0, hi = 127;
	bool known = true, bipolar = false, editable = false;
	std::string text;
	const xg::param *p = c.from == src::param ? &P(c.key) : nullptr;
	switch (c.from) {
	case src::param:
		known = m.get(*p, part, v);
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
			ImGui::TextDisabled("%s %s（%d-%d）", part_name(part).c_str(), p->label, lo, hi);
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
			br.send(m.set(*p, part, nv));
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
		const ImU32 fill = editable ? col(active || hovered ? ImGuiCol_SliderGrabActive : ImGuiCol_SliderGrab)
		                            : IM_COL32(200, 70, 60, 255);
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
	            known ? col(ImGuiCol_Text) : col(ImGuiCol_TextDisabled), text.c_str());

	if (hovered && !active)
		ImGui::SetItemTooltip(editable ? "%s  %s\n左右か上下にドラッグ・ホイール・ダブルクリックで打つ"
		                               : "%s  %s\n演奏の値（表示だけ）", c.title, text.c_str());
	ImGui::PopID();
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
			program_menu(part, m, br);
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
		const std::string vt = voice ? voice_text(msb, lsb, prog) : "--";
		dl->PushClipRect(pos, ImVec2(pos.x + w, pos.y + h), true);
		dl->AddText(ImVec2(pos.x + fs * 2.4f, pos.y + fs * 0.1f), col(ImGuiCol_Text), vt.c_str());
		char sub[64];
		std::snprintf(sub, sizeof(sub), "受信 %s   M %d  L %d", has_rcv ? channel_name(rcv).c_str() : "--", msb, lsb);
		dl->AddText(ImVec2(pos.x + fs * 2.4f, pos.y + fs * 1.15f), col(ImGuiCol_TextDisabled), sub);
		dl->PopClipRect();
	}

	// ---- インサーション。このパートに割り当てられているものを、印と種別の名前で。
	// バリエーションも接続が INSERTION ならこのパートだけに掛かるので並べる
	ImGui::TableNextColumn();
	{
		struct fx { const char *mark; ImU32 color; std::string name; };
		std::vector<fx> on;
		static const char *const INS[4][3] = {
			{ "1", "insertion1.part", "insertion1.type" }, { "2", "insertion2.part", "insertion2.type" },
			{ "3", "insertion3.part", "insertion3.type" }, { "4", "insertion4.part", "insertion4.type" },
		};
		int who = 0, type = 0;
		for (const auto &ins : INS)
			if (m.get(P(ins[1]), 0, who) && who == part && m.get(P(ins[2]), 0, type))
				on.push_back({ ins[0], IM_COL32(214, 160, 48, 255), xg::fx_name(type) });
		int conn = 1;
		if (m.get(P("variation.connect"), 0, conn) && conn == 0 && m.get(P("variation.part"), 0, who) &&
		    who == part && m.get(P("variation.type"), 0, type))
			on.push_back({ "V", IM_COL32(150, 110, 220, 255), xg::fx_name(type) });

		const ImVec2 pos = ImGui::GetCursorScreenPos();
		const float w = ImGui::GetContentRegionAvail().x;
		ImGui::Dummy(ImVec2(w, h));
		dl->PushClipRect(pos, ImVec2(pos.x + w, pos.y + h), true);
		const float line = fs * 1.05f;
		for (size_t i = 0; i < on.size() && i < 2; i++) {
			const float y = pos.y + fs * 0.1f + line * float(i);
			const float bw = fs * 1.0f;
			dl->AddRectFilled(ImVec2(pos.x + fs * 0.2f, y + 1), ImVec2(pos.x + fs * 0.2f + bw, y + fs), on[i].color, 3.0f);
			const ImVec2 ms = ImGui::CalcTextSize(on[i].mark);
			dl->AddText(ImVec2(pos.x + fs * 0.2f + (bw - ms.x) * 0.5f, y), IM_COL32(20, 20, 20, 255), on[i].mark);
			std::string name = on[i].name;
			if (i == 1 && on.size() > 2)
				name += " ほか";
			dl->AddText(ImVec2(pos.x + fs * 1.5f, y), col(ImGuiCol_Text), name.c_str());
		}
		dl->PopClipRect();
		if (!on.empty() && ImGui::IsItemHovered()) {
			std::string tip;
			for (const fx &f : on)
				tip += std::string(f.mark[0] == 'V' ? "バリエーション（INSERTION）: " : "インサーション ") +
				       (f.mark[0] == 'V' ? "" : std::string(f.mark) + ": ") + f.name + "\n";
			ImGui::SetTooltip("%s", tip.c_str());
		}
	}

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
		ImGui::Dummy(ImVec2(w, h));
		const float pad = fs * 0.2f;
		const float top = pos.y + pad, bottom = pos.y + h - pad;
		static const bool BLACK[12] = { 0, 1, 0, 1, 0, 0, 1, 0, 1, 0, 1, 0 };
		static const float WHITE_POS[12] = { 0, 0.6f, 1, 1.6f, 2, 3, 3.6f, 4, 4.6f, 5, 5.6f, 6 };
		constexpr int WHITES = 75;                    // 0-127 の白鍵
		const float kw = (w - pad * 2) / WHITES;
		const float left = pos.x + pad;
		auto held = [&](int note) {
			return slot >= 0 && ((ram.notes[slot][note >> 6] >> (note & 63)) & 1);
		};
		// 白鍵
		for (int note = 0; note < 128; note++) {
			if (BLACK[note % 12]) continue;
			const float x = left + (note / 12 * 7 + WHITE_POS[note % 12]) * kw;
			dl->AddRectFilled(ImVec2(x, top), ImVec2(x + kw - 1, bottom), held(note) ? NOTE_ON : IM_COL32(220, 220, 215, 255));
		}
		// 黒鍵
		for (int note = 0; note < 128; note++) {
			if (!BLACK[note % 12]) continue;
			const float x = left + (note / 12 * 7 + WHITE_POS[note % 12]) * kw;
			dl->AddRectFilled(ImVec2(x, top), ImVec2(x + kw * 0.8f, top + (bottom - top) * 0.6f),
			                  held(note) ? NOTE_ON : IM_COL32(30, 30, 32, 255));
		}
	}

	ImGui::PopID();
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

	const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV |
	                              ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX;
	ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(1, 1));
	if (ImGui::BeginTable("rows", NCOLS + 4, flags)) {
		ImGui::TableSetupScrollFreeze(1, 1);
		ImGui::TableSetupColumn("パート（右クリックで音色）", ImGuiTableColumnFlags_WidthFixed, fs * 15);
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
