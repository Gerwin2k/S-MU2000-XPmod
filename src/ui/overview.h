// license:BSD-3-Clause
//
// 一覧の窓（doc/pc-editor.md）。Domino のトラック一覧のように、32 パートを 1 行ずつ並べて、
// 曲を流しながら全体のバランスを見て整える。
//
// 1 行に: パートと音色、VEL メーター、VOL / EXP / PAN / P.BEND / MOD / HOLD / CUT / RESO /
// REV / CHO / VAR の棒と数、鳴っている鍵盤。
// 値は RAM の写し（panel::tick が層に入れたもの）と、MIDI の見張り（押さえている鍵）から。

#ifndef S_MU2000_UI_OVERVIEW_H
#define S_MU2000_UI_OVERVIEW_H

#pragma once

#include "xg_ui.h"
#include "xg/fx_types.h"

namespace ui {

class overview : public imgui_view
{
public:
	const wchar_t *title() const override { return L"S-MU2000 一覧"; }
	int default_width() const override  { return 1400; }
	int default_height() const override { return 760; }
	void draw(xg::model &m, const xg_snapshot &ram, bridge &br) override;
	void hidden(bridge &br) override { release_keys(br); }

	struct column;                   // 列の中身（overview.cpp）

private:
	void release_keys(bridge &br);          // マウスで鳴らしている鍵を全部離す
	void row(int part, xg::model &m, const xg_snapshot &ram, bridge &br, float h);
	// INS 列の 1 マス。右クリックで掛ける・外す・種類、印のドラッグで別のパートへ
	void ins_cell(int part, xg::model &m, bridge &br, float h);
	// 上のマスターの表。マスターボリューム、移調、リバーブ・コーラス・バリエーションの種類と戻り、
	// インサーション 1-4 の種類と掛け先、全パートの鍵盤
	void master_pane(xg::model &m, const xg_snapshot &ram, bridge &br);
	template <size_t N>
	void system_fx_cell(const char *title, const xg::fx_type (&types)[N], const char *type_key,
	                    const char *return_col, bool variation, xg::model &m, const xg_snapshot &ram,
	                    bridge &br, float h);
	void insertion_cell(int slot_index, xg::model &m, bridge &br, float h);
	// 棒 1 つ。XG のパラメータなら触れる。part が -1 ならマスターの行
	void cell(const column &c, int part, xg::model &m, const xg_snapshot &ram, bridge &br, float w, float h);

	int    m_part = 0;
	float  m_level[32] = {};          // VEL メーターの今の高さ（0-1）
	u32    m_seen_ons[32] = {};       // 見張りのノートオンの回数を最後に見た値
	int    m_playing[32] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
	                         -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
	int    m_playing_slot[32] = {};   // 鳴らしている鍵（-1 は無し）と、そのときの受信の口×チャンネル
	double m_scrolled_at = -1;        // ホイールで表をスクロールした時刻（エディタと同じ決まり）
	bool   m_wheel_taken = false;
};

} // namespace ui

#endif // S_MU2000_UI_OVERVIEW_H
