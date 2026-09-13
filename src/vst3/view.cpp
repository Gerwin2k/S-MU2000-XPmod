// license:BSD-3-Clause
//
// The shared half of the VST3 view: the VST3 interface itself, the panel, and
// the handling of mouse and key input. The window that holds it is per
// platform (view_win.cpp, view_mac.mm), reached through plug_window.h.
//
// This file is plain C++ and includes compat/gdi.h, which is what paints the
// panel on both platforms. On macOS that means CoreGraphics is fine to include
// here too -- it is Cocoa, not CoreGraphics, that clashes with the GDI shim.

#include "view.h"
#include "plug_window.h"

#include "compat/gdi.h"
#include "engine.h"
#include "ui/bridge.h"
#include "ui/layout.h"
#include "ui/panel.h"

#if !defined(_WIN32)
#include <CoreGraphics/CoreGraphics.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>

using namespace Steinberg;

namespace smu2000 {
namespace vst3 {

namespace {

// plug_key -> mu2000::button: the one place that decides. Each platform maps
// its own key codes onto plug_key, so this stays the single answer to "what
// does this key do", and it matches gui.cpp
mu2000::button button_of(int code, bool &ok)
{
	ok = true;
	switch (code) {
	case PLUG_KEY_PLAY:          return mu2000::button::play;
	case PLUG_KEY_EDIT:          return mu2000::button::edit;
	case PLUG_KEY_UTIL:          return mu2000::button::util;
	case PLUG_KEY_EFFECT:        return mu2000::button::effect;
	case PLUG_KEY_MUTE_SOLO:     return mu2000::button::mute_solo;
	case PLUG_KEY_PART_PLUS:     return mu2000::button::part_plus;
	case PLUG_KEY_PART_MINUS:    return mu2000::button::part_minus;
	case PLUG_KEY_VALUE_PLUS:    return mu2000::button::value_plus;
	case PLUG_KEY_VALUE_MINUS:   return mu2000::button::value_minus;
	case PLUG_KEY_ENTER:         return mu2000::button::enter;
	case PLUG_KEY_EXIT:          return mu2000::button::exit;
	case PLUG_KEY_SELECT_RIGHT:  return mu2000::button::select_right;
	case PLUG_KEY_SELECT_LEFT:   return mu2000::button::select_left;
	case PLUG_KEY_SEQ:           return mu2000::button::seq;
	case PLUG_KEY_AUDITION:      return mu2000::button::audition;
	case PLUG_KEY_SELECT:        return mu2000::button::select;
	case PLUG_KEY_SAMPLING_MODE: return mu2000::button::sampling_mode;
	default: break;
	}
	ok = false;
	return mu2000::button::count;
}

} // namespace


// The panel lives here so that view.h can stay free of compat/gdi.h
struct plug_view::impl
{
	engine  &eng;
	ui::panel panel;

#if defined(_WIN32)
	// Double buffered: the host repaints at 30 frames a second and drawing
	// straight into the window would flicker
	HDC     mem_dc = nullptr;
	HBITMAP mem_bmp = nullptr;
	int     mem_w = 0, mem_h = 0;
#endif

	explicit impl(engine &e) : eng(e) {}

	void paint_panel(HDC dc)
	{
		ui::snapshot s;
		eng.panel().read(s);

		char status[160];
		std::snprintf(status, sizeof(status), "%s", eng.message().c_str());

		panel.set_volume(eng.panel().gain());
		panel.paint(dc, s, eng.panel().buttons(), status);
	}

	void forget_backing()
	{
#if defined(_WIN32)
		if (mem_bmp) { DeleteObject(mem_bmp); mem_bmp = nullptr; }
		if (mem_dc)  { DeleteDC(mem_dc); mem_dc = nullptr; }
		mem_w = mem_h = 0;
#endif
	}
};


plug_view::plug_view(engine &eng)
	: m_impl(new impl(eng)), m_engine(eng)
{
	// パネルの配置。%LOCALAPPDATA%\S-MU2000\panel.txt があれば読む
	// （doc/panel-editing.md）。無ければ組み込みの配置のまま
	//
	// find_default() now searches the per-user settings directory on either
	// platform (~/Library/Application Support/S-MU2000 on macOS)
	const std::string lay = ui::layout::find_default();
	if (!lay.empty()) {
		std::string err;
		m_impl->panel.lay().load(lay, err);
	}
	m_impl->panel.resize(m_w, m_h);
}

plug_view::~plug_view()
{
	removed();
}

tresult PLUGIN_API plug_view::queryInterface(const TUID iid, void **obj)
{
	if (FUnknownPrivate::iidEqual(iid, FUnknown::iid) ||
	    FUnknownPrivate::iidEqual(iid, IPlugView::iid)) {
		addRef();
		*obj = static_cast<IPlugView *>(this);
		return kResultOk;
	}
	*obj = nullptr;
	return kNoInterface;
}

uint32 PLUGIN_API plug_view::addRef()  { return uint32(FUnknownPrivate::atomicAdd(m_refs, 1)); }

uint32 PLUGIN_API plug_view::release()
{
	if (FUnknownPrivate::atomicAdd(m_refs, -1) == 0) { delete this; return 0; }
	return uint32(m_refs);
}

tresult PLUGIN_API plug_view::isPlatformTypeSupported(FIDString type)
{
	return (type && !std::strcmp(type, plug_window_type())) ? kResultTrue : kResultFalse;
}

tresult PLUGIN_API plug_view::attached(void *parent, FIDString type)
{
	if (isPlatformTypeSupported(type) != kResultTrue || !parent)
		return kResultFalse;
	if (m_window)
		removed();

	m_window = plug_window_create(*this);
	if (!m_window || !m_window->attach(parent, m_w, m_h)) {
		delete m_window;
		m_window = nullptr;
		return kResultFalse;
	}
	m_impl->panel.resize(m_w, m_h);
	return kResultOk;
}

tresult PLUGIN_API plug_view::removed()
{
	if (m_window) {
		m_window->detach();
		delete m_window;
		m_window = nullptr;
	}
	m_impl->forget_backing();
	return kResultOk;
}

// ホスト経由の入力は使わない。子ウィンドウが本物のメッセージを受け取る
tresult PLUGIN_API plug_view::onWheel(float)                          { return kResultFalse; }
tresult PLUGIN_API plug_view::onKeyDown(char16, int16, int16)         { return kResultFalse; }
tresult PLUGIN_API plug_view::onKeyUp(char16, int16, int16)           { return kResultFalse; }
tresult PLUGIN_API plug_view::onFocus(TBool)                          { return kResultOk; }

tresult PLUGIN_API plug_view::getSize(ViewRect *size)
{
	if (!size)
		return kInvalidArgument;
	size->left = 0; size->top = 0;
	size->right = m_w; size->bottom = m_h;
	return kResultOk;
}

tresult PLUGIN_API plug_view::onSize(ViewRect *r)
{
	if (!r)
		return kInvalidArgument;
	m_w = std::max<int32>(r->getWidth(), 640);
	m_h = std::max<int32>(r->getHeight(), 180);
	if (m_window)
		m_window->set_size(m_w, m_h);
	m_impl->panel.resize(m_w, m_h);
	return kResultOk;
}

tresult PLUGIN_API plug_view::setFrame(IPlugFrame *frame)
{
	m_frame = frame;
	return kResultOk;
}

tresult PLUGIN_API plug_view::canResize() { return kResultTrue; }

tresult PLUGIN_API plug_view::checkSizeConstraint(ViewRect *rect)
{
	if (!rect)
		return kInvalidArgument;
	// 横に長い機械なので、縦横比はこちらで決めてしまう
	const int w = std::max<int32>(rect->getWidth(), 640);
	const int h = std::max<int32>(w * ui::LOGICAL_H / ui::LOGICAL_W, 180);
	rect->right = rect->left + w;
	rect->bottom = rect->top + h;
	return kResultTrue;
}


// ---- Called by the platform window

void plug_view::repaint(void *native, int w, int h)
{
	if (!native || w <= 0 || h <= 0)
		return;

#if defined(_WIN32)
	HDC dst = static_cast<HDC>(native);
	if (!m_impl->mem_dc || m_impl->mem_w != w || m_impl->mem_h != h) {
		m_impl->forget_backing();
		m_impl->mem_dc  = CreateCompatibleDC(dst);
		m_impl->mem_bmp = CreateCompatibleBitmap(dst, w, h);
		SelectObject(m_impl->mem_dc, m_impl->mem_bmp);
		m_impl->mem_w = w;
		m_impl->mem_h = h;
	}
	m_impl->paint_panel(m_impl->mem_dc);
	BitBlt(dst, 0, 0, w, h, m_impl->mem_dc, 0, 0, SRCCOPY);
#else
	// The subview is flipped, so the context is already top-left with y down
	// and only has to be wrapped -- no flipping, same as the GUI window
	CGContextRef ctx = static_cast<CGContextRef>(native);
	HDC dc = static_cast<HDC>(smu_gdi_wrap_view_context(ctx, w, h));
	m_impl->paint_panel(dc);
	DeleteDC(dc);
#endif
}

void plug_view::mouse_down(int x, int y) { m_impl->panel.press(x, y, m_engine.panel()); }

void plug_view::mouse_drag(int x, int y) { m_impl->panel.drag(x, y, m_engine.panel()); }

void plug_view::mouse_up()               { m_impl->panel.release(m_engine.panel()); }

void plug_view::wheel(int x, int y, int steps)
{
	if (steps)
		m_impl->panel.wheel_at(x, y, steps, m_engine.panel());
}

void plug_view::key(int code, bool down)
{
	bool ok = false;
	const mu2000::button b = button_of(code, ok);
	if (ok)
		m_engine.panel().press(b, down);
}

void plug_view::focus_lost() { m_engine.panel().release_all(); }

} // namespace vst3
} // namespace smu2000
