// license:BSD-3-Clause
//
// The VST3 view's window on Windows: a child HWND inside the host's parent,
// which is what gives the panel real mouse, wheel and key messages. The VST3
// interface, the panel and the input semantics are all in view.cpp; this is
// only the window.
//
// Extracted from view.cpp when the macOS port arrived, so that view.cpp could
// stop including windows.h -- view_mac.mm has to include view.h next to Cocoa,
// and the two cannot see the same BOOL.

#include "plug_window.h"
#include "view.h"

#include <windows.h>
#include <windowsx.h>

namespace smu2000 {
namespace vst3 {

const char *plug_window_type() { return Steinberg::kPlatformTypeHWND; }

namespace {

const char *kClassName = "SMU2000PlugView";

// 窓のクラスはこの DLL で 1 度だけ登録する
HINSTANCE this_module()
{
	HMODULE self = nullptr;
	GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
	                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
	                   reinterpret_cast<LPCSTR>(&this_module), &self);
	return HINSTANCE(self);
}

void register_class(WNDPROC proc)
{
	static bool done = false;
	if (done)
		return;
	WNDCLASSA wc{};
	wc.lpfnWndProc   = proc;
	wc.hInstance     = this_module();
	wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
	wc.lpszClassName = kClassName;
	wc.hbrBackground = nullptr;
	RegisterClassA(&wc);
	done = true;
}

// ホストによってはキーがこちらに回ってくる。gui.exe と同じ割り当て
int plug_key_of(WPARAM vk)
{
	switch (vk) {
	case 'A': return PLUG_KEY_PLAY;
	case 'E': return PLUG_KEY_EDIT;
	case 'U': return PLUG_KEY_UTIL;
	case 'F': return PLUG_KEY_EFFECT;
	case 'S': return PLUG_KEY_MUTE_SOLO;
	case VK_OEM_6: return PLUG_KEY_PART_PLUS;
	case VK_OEM_4: return PLUG_KEY_PART_MINUS;
	case VK_OEM_PLUS:  return PLUG_KEY_VALUE_PLUS;
	case VK_OEM_MINUS: return PLUG_KEY_VALUE_MINUS;
	case VK_BACK:   return PLUG_KEY_EXIT;
	case VK_RETURN: return PLUG_KEY_ENTER;
	case VK_OEM_PERIOD: return PLUG_KEY_SELECT_RIGHT;
	case VK_OEM_COMMA:  return PLUG_KEY_SELECT_LEFT;
	case 'Q': return PLUG_KEY_SEQ;
	case 'Z': return PLUG_KEY_AUDITION;
	case 'X': return PLUG_KEY_SELECT;
	case 'M': return PLUG_KEY_SAMPLING_MODE;
	default: break;
	}
	return PLUG_KEY_NONE;
}

} // namespace


class win_window : public plug_window
{
public:
	explicit win_window(plug_view &owner) : m_owner(owner) {}
	~win_window() override { detach(); }

	bool attach(void *parent, int w, int h) override;
	void detach() override;
	void set_size(int w, int h) override;

private:
	static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
	LRESULT handle(HWND h, UINT msg, WPARAM wp, LPARAM lp);

	plug_view &m_owner;
	HWND m_hwnd = nullptr;
};

bool win_window::attach(void *parent, int w, int h)
{
	if (m_hwnd || !parent)
		return false;

	register_class(&win_window::wnd_proc);
	m_hwnd = CreateWindowExA(0, kClassName, "", WS_CHILD | WS_VISIBLE,
	                         0, 0, w, h, reinterpret_cast<HWND>(parent), nullptr,
	                         this_module(), nullptr);
	if (!m_hwnd)
		return false;

	// The window procedure has to find its way back to this object
	SetWindowLongPtrA(m_hwnd, GWLP_USERDATA, LONG_PTR(this));
	SetTimer(m_hwnd, 1, 33, nullptr);        // 30 コマ／秒
	return true;
}

void win_window::detach()
{
	if (!m_hwnd)
		return;
	KillTimer(m_hwnd, 1);
	SetWindowLongPtrA(m_hwnd, GWLP_USERDATA, 0);
	DestroyWindow(m_hwnd);
	m_hwnd = nullptr;
}

void win_window::set_size(int w, int h)
{
	if (m_hwnd)
		MoveWindow(m_hwnd, 0, 0, w, h, TRUE);
}

LRESULT CALLBACK win_window::wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	auto *self = reinterpret_cast<win_window *>(GetWindowLongPtrA(h, GWLP_USERDATA));
	if (!self)
		return DefWindowProcA(h, msg, wp, lp);
	return self->handle(h, msg, wp, lp);
}

LRESULT win_window::handle(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_TIMER:
		InvalidateRect(h, nullptr, FALSE);
		return 0;

	case WM_ERASEBKGND:
		return 1;                        // 全部自分で描く

	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(h, &ps);
		RECT cr;
		GetClientRect(h, &cr);
		m_owner.repaint(dc, cr.right, cr.bottom);
		EndPaint(h, &ps);
		return 0;
	}

	case WM_SIZE:
		InvalidateRect(h, nullptr, FALSE);
		return 0;

	case WM_LBUTTONDOWN:
		SetCapture(h);
		m_owner.mouse_down(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
		InvalidateRect(h, nullptr, FALSE);
		return 0;

	case WM_MOUSEMOVE:
		m_owner.mouse_drag(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
		return 0;

	case WM_LBUTTONUP:
		m_owner.mouse_up();
		ReleaseCapture();
		InvalidateRect(h, nullptr, FALSE);
		return 0;

	case WM_MOUSEWHEEL: {
		POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
		ScreenToClient(h, &pt);
		const int delta = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
		if (delta)
			m_owner.wheel(pt.x, pt.y, delta);
		return 0;
	}

	case WM_KEYDOWN: {
		if (lp & (1 << 30))              // 押しっぱなしの繰り返しは無視
			return 0;
		const int k = plug_key_of(wp);
		if (k != PLUG_KEY_NONE)
			m_owner.key(k, true);
		return 0;
	}

	case WM_KEYUP: {
		const int k = plug_key_of(wp);
		if (k != PLUG_KEY_NONE)
			m_owner.key(k, false);
		return 0;
	}

	case WM_KILLFOCUS:
		m_owner.focus_lost();
		return 0;
	}
	return DefWindowProcA(h, msg, wp, lp);
}


plug_window *plug_window_create(plug_view &owner)
{
	return new win_window(owner);
}

} // namespace vst3
} // namespace smu2000
