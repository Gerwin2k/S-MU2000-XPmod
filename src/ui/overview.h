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

namespace ui {

class overview : public imgui_view
{
public:
	const wchar_t *title() const override { return L"S-MU2000 一覧"; }
	int default_width() const override  { return 1400; }
	int default_height() const override { return 760; }
	void draw(xg::model &m, const xg_snapshot &ram, bridge &br) override;

	struct column;                   // 列の中身（overview.cpp）

private:
	void row(int part, xg::model &m, const xg_snapshot &ram, bridge &br, float h);
	// 棒 1 つ。XG のパラメータなら触れる。戻り値は無し（書くときは中で送る）
	void cell(const column &c, int part, xg::model &m, const xg_snapshot &ram, bridge &br, float w, float h);

	int    m_part = 0;
	float  m_level[32] = {};          // VEL メーターの今の高さ（0-1）
	u32    m_seen_ons[32] = {};       // 見張りのノートオンの回数を最後に見た値
	double m_scrolled_at = -1;        // ホイールで表をスクロールした時刻（エディタと同じ決まり）
	bool   m_wheel_taken = false;
};

} // namespace ui

#endif // S_MU2000_UI_OVERVIEW_H
