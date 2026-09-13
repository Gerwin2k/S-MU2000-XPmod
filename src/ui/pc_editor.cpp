// license:BSD-3-Clause

#include "pc_editor.h"

#include "imgui.h"

#include <cstdio>
#include <string>

namespace ui {

namespace {

constexpr int PARTS = 32;

// 選んでいるパートは 1 秒ごと、一覧に出す 32 パートは 3 秒ごとに読み返す。
// 塊 1 つは 20ms ほどで、層は 1 つずつしか頼まないので、32 個で 1 秒弱かかる
constexpr u64 PART_MS = 1000;
constexpr u64 ALL_MS  = 3000;

const xg::param &P(const char *key)
{
	const xg::param *p = xg::find(key);
	IM_ASSERT(p);
	return *p;
}

std::string part_name(int part)
{
	char buf[8];
	std::snprintf(buf, sizeof(buf), "%c%d", part < 16 ? 'A' : 'B', part % 16 + 1);
	return buf;
}

// 受信チャンネルは 0-31 が A1-A16・B1-B16、127 が OFF
std::string channel_name(int v)
{
	if (v == 127)
		return "OFF";
	if (v < 0 || v > 31)
		return std::to_string(v);
	return part_name(v);
}

// 値を名前で見せる。ImGui の書式文字列に渡すので % を潰しておく
std::string shown(const xg::param &p, int v)
{
	std::string s = std::string(p.key) == "part.rcv_channel" ? channel_name(v) : xg::format(p, v);
	std::string out;
	for (char c : s) {
		out += c;
		if (c == '%') out += '%';
	}
	return out;
}

// パートの面に並べる組
struct group { const char *title; const char *const keys[12]; };

const group GROUPS[] = {
	{ "音色",           { "part.bank_msb", "part.bank_lsb", "part.program", "part.mode", "part.element_reserve" } },
	{ "音量と送り",     { "part.volume", "part.pan", "part.dry_level", "part.reverb_send", "part.chorus_send", "part.variation_send" } },
	{ "受信と発音",     { "part.rcv_channel", "part.mono_poly", "part.key_assign", "part.note_low", "part.note_high",
	                      "part.note_shift", "part.detune", "part.vel_depth", "part.vel_offset" } },
	{ "フィルタと EG",  { "part.cutoff", "part.resonance", "part.attack", "part.decay", "part.release" } },
	{ "ビブラート",     { "part.vib_rate", "part.vib_depth", "part.vib_delay" } },
	{ "モジュレーション", { "part.mw_pitch", "part.mw_filter", "part.mw_amp", "part.mw_lfo_pmod", "part.mw_lfo_fmod", "part.mw_lfo_amod" } },
	{ "ピッチベンド",   { "part.bend_pitch", "part.bend_filter", "part.bend_amp", "part.bend_lfo_pmod", "part.bend_lfo_fmod", "part.bend_lfo_amod" } },
};

} // namespace


void pc_editor::request(xg::model &m, u64 now)
{
	if (now >= m_part_at) {
		m.want_part(m_part);
		m_part_at = now + PART_MS;
	}
	if (now >= m_all_at) {
		for (int i = 0; i < PARTS; i++)
			m.want_part(i);
		m_all_at = now + ALL_MS;
	}
}


bool pc_editor::edit(const xg::param &p, int part, xg::model &m, bridge &br, float width)
{
	ImGui::PushID(p.key);
	ImGui::PushID(part);
	ImGui::SetNextItemWidth(width);

	int v = 0;
	const bool known = m.get(p, part, v);
	bool wrote = false;

	if (!known) {
		// まだ読めていない。形だけ出して触らせない
		ImGui::BeginDisabled();
		int dummy = p.min;
		ImGui::SliderInt("##v", &dummy, p.min, p.max, "--");
		ImGui::EndDisabled();
	} else if (p.how == xg::view::choice || std::string(p.key) == "part.rcv_channel") {
		const bool rcv = std::string(p.key) == "part.rcv_channel";
		if (ImGui::BeginCombo("##v", shown(p, v).c_str())) {
			const int hi = rcv ? 32 : p.max;       // 受信チャンネルは最後に OFF
			for (int i = p.min; i <= hi; i++) {
				const int value = rcv && i == 32 ? 127 : i;
				if (ImGui::Selectable((rcv ? channel_name(value) : xg::format(p, value)).c_str(), value == v)) {
					br.send(m.set(p, part, value));
					wrote = true;
				}
			}
			ImGui::EndCombo();
		}
	} else {
		int nv = v;
		// 表示は名前（L12、+3 など）。Ctrl+クリックで数を打てる（打つのは生の値）
		if (ImGui::SliderInt("##v", &nv, p.min, p.max, shown(p, v).c_str(), ImGuiSliderFlags_AlwaysClamp) && nv != v) {
			br.send(m.set(p, part, nv));
			wrote = true;
		}
		// 右クリックで既定の値へ
		if (ImGui::IsItemClicked(ImGuiMouseButton_Right) && xg::valid(p, p.def)) {
			br.send(m.set(p, part, p.def));
			wrote = true;
		}
	}
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
		ImGui::SetTooltip("%s（%s）\n右クリックで既定の値 %s", p.label, p.key, xg::format(p, p.def).c_str());

	ImGui::PopID();
	ImGui::PopID();
	return wrote;
}


void pc_editor::part_list(xg::model &m)
{
	const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
	                              ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit;
	if (!ImGui::BeginTable("parts", 4, flags))
		return;
	ImGui::TableSetupScrollFreeze(0, 1);
	ImGui::TableSetupColumn("パート");
	ImGui::TableSetupColumn("受信");
	ImGui::TableSetupColumn("音色", ImGuiTableColumnFlags_WidthStretch);
	ImGui::TableSetupColumn("音量");
	ImGui::TableHeadersRow();

	for (int i = 0; i < PARTS; i++) {
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		const std::string name = part_name(i);
		if (ImGui::Selectable(name.c_str(), m_part == i, ImGuiSelectableFlags_SpanAllColumns)) {
			m_part = i;
			m_part_at = 0;                           // すぐ読み返す
		}
		int v = 0;
		ImGui::TableNextColumn();
		ImGui::TextUnformatted(m.get(P("part.rcv_channel"), i, v) ? channel_name(v).c_str() : "--");
		ImGui::TableNextColumn();
		int msb = 0, lsb = 0, prog = 0;
		if (m.get(P("part.bank_msb"), i, msb) && m.get(P("part.bank_lsb"), i, lsb) && m.get(P("part.program"), i, prog))
			ImGui::Text("%3d/%3d  %3d", msb, lsb, prog + 1);
		else
			ImGui::TextUnformatted("--");
		ImGui::TableNextColumn();
		if (m.get(P("part.volume"), i, v))
			ImGui::Text("%3d", v);
		else
			ImGui::TextUnformatted(" --");
	}
	ImGui::EndTable();
}


void pc_editor::mixer(xg::model &m, bridge &br)
{
	static const char *const COLS[] = {
		"part.volume", "part.pan", "part.dry_level", "part.reverb_send", "part.chorus_send", "part.variation_send",
	};
	static const char *const TITLES[] = { "Volume", "Pan", "Dry", "Reverb", "Chorus", "Variation" };
	const int ncols = int(sizeof(COLS) / sizeof(COLS[0]));

	const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
	                              ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame;
	if (!ImGui::BeginTable("mixer", ncols + 2, flags))
		return;
	ImGui::TableSetupScrollFreeze(0, 1);
	ImGui::TableSetupColumn("パート", ImGuiTableColumnFlags_WidthFixed);
	ImGui::TableSetupColumn("Program", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 7);
	for (const char *t : TITLES)
		ImGui::TableSetupColumn(t);
	ImGui::TableHeadersRow();

	for (int i = 0; i < PARTS; i++) {
		ImGui::TableNextRow();
		ImGui::TableNextColumn();
		if (ImGui::Selectable(part_name(i).c_str(), m_part == i)) {
			m_part = i;
			m_part_at = 0;
		}
		ImGui::TableNextColumn();
		edit(P("part.program"), i, m, br, -1);
		for (const char *key : COLS) {
			ImGui::TableNextColumn();
			edit(P(key), i, m, br, -1);
		}
	}
	ImGui::EndTable();
}


void pc_editor::part_page(xg::model &m, bridge &br)
{
	ImGui::Text("パート %s", part_name(m_part).c_str());
	ImGui::Separator();

	const float label_w = ImGui::GetFontSize() * 7;
	if (!ImGui::BeginTable("groups", 2, ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchSame))
		return;
	int n = 0;
	for (const group &g : GROUPS) {
		if (n++ % 2 == 0)
			ImGui::TableNextRow();
		ImGui::TableNextColumn();
		ImGui::SeparatorText(g.title);
		for (const char *key : g.keys) {
			if (!key)
				break;
			const xg::param &p = P(key);
			ImGui::AlignTextToFramePadding();
			ImGui::TextUnformatted(p.label);
			ImGui::SameLine(label_w);
			edit(p, m_part, m, br, -1);
		}
		ImGui::Spacing();
	}
	ImGui::EndTable();
}


void pc_editor::draw(xg::model &m, bridge &br)
{
	request(m, br.audio_ms());

	const ImGuiViewport *vp = ImGui::GetMainViewport();
	ImGui::SetNextWindowPos(vp->WorkPos);
	ImGui::SetNextWindowSize(vp->WorkSize);
	const ImGuiWindowFlags wf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
	                            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
	ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0);
	ImGui::Begin("editor", nullptr, wf);
	ImGui::PopStyleVar();

	// 上の帯
	if (ImGui::Button("XG リセット")) {
		static const u8 XG_ON[] = { 0xf0, 0x43, 0x10, 0x4c, 0x00, 0x00, 0x7e, 0x00, 0xf7 };
		br.send(XG_ON, sizeof(XG_ON));
		m.forget();
		m_part_at = m_all_at = 0;
	}
	ImGui::SameLine();
	if (ImGui::Button("オールノートオフ")) {
		for (int ch = 0; ch < 16; ch++) {
			const u8 msg[3] = { u8(0xb0 | ch), 123, 0 };
			br.send(msg, 3);
		}
	}
	ImGui::SameLine();
	ImGui::TextDisabled("値は MU2000 から読み返したもの。-- はまだ読めていない。Ctrl+クリックで数を打てる");

	// 左にパートの一覧、右に面
	const float list_w = ImGui::GetFontSize() * 17;
	ImGui::BeginChild("list", ImVec2(list_w, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_ResizeX);
	part_list(m);
	ImGui::EndChild();
	ImGui::SameLine();
	ImGui::BeginChild("page", ImVec2(0, 0));
	if (ImGui::BeginTabBar("tabs")) {
		if (ImGui::BeginTabItem("ミキサー")) {
			m_tab = 0;
			mixer(m, br);
			ImGui::EndTabItem();
		}
		if (ImGui::BeginTabItem("パート")) {
			m_tab = 1;
			part_page(m, br);
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}
	ImGui::EndChild();

	ImGui::End();
}

} // namespace ui
