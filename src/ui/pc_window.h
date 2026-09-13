// license:BSD-3-Clause
//
// PC エディタを載せる窓（gui.exe 用）。Win32 の窓に Direct3D 11 で ImGui を描く。
//
// 窓は gui の画面の糸で作り、gui のタイマーから frame() を呼んで描く。
// 閉じても消さずに隠すだけなので、開き直すと同じ状態で出る。

#ifndef S_MU2000_UI_PC_WINDOW_H
#define S_MU2000_UI_PC_WINDOW_H

#pragma once

#include "pc_editor.h"

#include <string>

#include <windows.h>

struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGISwapChain;
struct ID3D11RenderTargetView;

namespace ui {

class pc_window
{
public:
	~pc_window();

	// 出す。初めてなら窓と描画装置を作る。失敗したら err に理由
	bool show(HINSTANCE inst, std::string &err);
	bool visible() const;

	// タイマーから。見えていなければ何もしない
	void frame(xg::model &m, bridge &br);

private:
	bool create(HINSTANCE inst, std::string &err);
	bool create_device(std::string &err);
	void make_target();
	void drop_target();
	void destroy();
	static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);

	HWND m_hwnd = nullptr;
	ID3D11Device           *m_dev = nullptr;
	ID3D11DeviceContext    *m_ctx = nullptr;
	IDXGISwapChain         *m_swap = nullptr;
	ID3D11RenderTargetView *m_rtv = nullptr;
	bool m_imgui = false;
	UINT m_resize_w = 0, m_resize_h = 0;     // WM_SIZE で受けて、次に描く前に直す
	pc_editor m_editor;
};

} // namespace ui

#endif // S_MU2000_UI_PC_WINDOW_H
