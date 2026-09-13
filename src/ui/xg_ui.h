// license:BSD-3-Clause
//
// ImGui で XG の値を触る窓（エディタ・一覧）が共通で使う小物。

#ifndef S_MU2000_UI_XG_UI_H
#define S_MU2000_UI_XG_UI_H

#pragma once

#include "bridge.h"
#include "snapshot.h"
#include "xg/model.h"
#include "xg/voices.h"

#include <string>

namespace ui {

// ImGui の窓 1 枚ぶんの中身。pc_window が窓と描画装置を用意して、1 コマごとに draw を呼ぶ
class imgui_view
{
public:
	virtual ~imgui_view() = default;
	virtual const wchar_t *title() const = 0;
	virtual int default_width() const = 0;
	virtual int default_height() const = 0;
	// ram は音声の糸が写した RAM と MIDI の見張り。m は同じものを読んだパラメータの層
	virtual void draw(xg::model &m, const xg_snapshot &ram, bridge &br) = 0;
	// 窓を閉じた（隠した）とき。マウスで鳴らしている音を止めるなど
	virtual void hidden(bridge &) {}
};

namespace xgui {

const xg::param &P(const char *key);

std::string part_name(int part);        // A1-A16・B1-B16
std::string channel_name(int value);    // 受信チャンネル。127 は OFF
const char *gm_name(int program);       // General MIDI の楽器名（規格の名前）
std::string voice_text(int msb, int lsb, int program);

// 音色の名前と絵を読む ROM。音源を読み込んだあとで 1 回渡す（無ければ GM の名前で出す）
void set_voice_rom(std::shared_ptr<const std::vector<u8>> rom);
const xg::voice_rom *voices();

// 右クリックで出す品書き（プログラムとバンク）。ROM から読めれば MU2000 の音色の名前で並べる。
// ram は音色の引き方を知るため（無ければ XG の既定）
void program_menu(int part, xg::model &m, const xg_snapshot *ram, bridge &br);

// ---- 説明（ヘルプ）。見出しや名前にカーソルを当てると、何に効くのかを出す（日本語・英語）。
// 邪魔な人もいるので、窓の上のチェックボックスで消せる。選んだ状態は
// %LOCALAPPDATA%\S-MU2000\editor.ini に覚えておく（窓どうしで共通）
bool &help_on();
// 直前の部品にカーソルが載っていれば、説明を出す。name は列の見出しかパラメータのキー
void help_tip(const char *name);
// 「説明を出す」のチェックボックスと、言語の選択
void help_checkbox();
// 表の見出しの行を、説明つきで出す（ImGui::TableHeadersRow の代わり）
void headers_with_help(int columns);

} // namespace xgui
} // namespace ui

#endif // S_MU2000_UI_XG_UI_H
