// license:BSD-3-Clause
//
// PC で触るためのエディタ（doc/pc-editor.md）。Dear ImGui で描く。
//
// 窓や描画装置には依存しない。ImGui の 1 コマの中で draw() を呼んでもらうだけ。
// 値は画面では覚えない。パラメータの層（xg::model）の写しを読み、書くときは
// パラメータチェンジを bridge に積む。パネルの面と同じ写しを使うので、どちらで
// 変えても同じ値が見える。

#ifndef S_MU2000_UI_PC_EDITOR_H
#define S_MU2000_UI_PC_EDITOR_H

#pragma once

#include "bridge.h"
#include "xg/model.h"

namespace ui {

class pc_editor
{
public:
	// 窓いっぱいに描く。層への問い合わせ（want_*）もここで積む。
	// 実際に送るのは model::poll を回している側（panel::tick）
	void draw(xg::model &m, bridge &br);

private:
	void request(xg::model &m, u64 now);
	void part_list(xg::model &m);
	void mixer(xg::model &m, bridge &br);
	void part_page(xg::model &m, bridge &br);
	// 1 つの値を触る部品。戻り値は「書いたか」
	bool edit(const xg::param &p, int part, xg::model &m, bridge &br, float width);

	int  m_part = 0;
	int  m_tab = 0;             // 0 ミキサー、1 パート
	u64  m_part_at = 0;         // 次に選んでいるパートを読み返す時刻（音源の時計）
	u64  m_all_at = 0;          // 次に 32 パート全部を読み返す時刻
};

} // namespace ui

#endif // S_MU2000_UI_PC_EDITOR_H
