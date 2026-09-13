// license:BSD-3-Clause
//
// VST3 の画面（IPlugView）。ホストから渡された親ウィンドウの中に
// 子ウィンドウを 1 枚作り、gui.exe と同じ ui::panel で描く。
//
// SDK の土台（public.sdk / VSTGUI）は使っていないので、ここは素の Win32。
//
// The child window itself now lives per platform behind plug_window.h, so this
// header mentions no window system at all.
//
// This header deliberately mentions no window system at all. The panel is held
// behind a pimpl because ui::panel needs compat/gdi.h, and view_mac.mm has to
// include this header next to Cocoa -- where BOOL and Polygon mean something
// else entirely. The per-platform window lives behind plug_window.h.

#ifndef S_MU2000_VST3_VIEW_H
#define S_MU2000_VST3_VIEW_H

#pragma once

#include "pluginterfaces/gui/iplugview.h"

#include <memory>

namespace smu2000 {
namespace vst3 {

class engine;
class plug_window;

class plug_view : public Steinberg::IPlugView
{
public:
	explicit plug_view(engine &eng);
	virtual ~plug_view();

	// ---- FUnknown
	Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void **obj) override;
	Steinberg::uint32 PLUGIN_API addRef() override;
	Steinberg::uint32 PLUGIN_API release() override;

	// ---- IPlugView
	Steinberg::tresult PLUGIN_API isPlatformTypeSupported(Steinberg::FIDString type) override;
	Steinberg::tresult PLUGIN_API attached(void *parent, Steinberg::FIDString type) override;
	Steinberg::tresult PLUGIN_API removed() override;
	Steinberg::tresult PLUGIN_API onWheel(float distance) override;
	Steinberg::tresult PLUGIN_API onKeyDown(Steinberg::char16 key, Steinberg::int16 code,
	                                        Steinberg::int16 modifiers) override;
	Steinberg::tresult PLUGIN_API onKeyUp(Steinberg::char16 key, Steinberg::int16 code,
	                                      Steinberg::int16 modifiers) override;
	Steinberg::tresult PLUGIN_API getSize(Steinberg::ViewRect *size) override;
	Steinberg::tresult PLUGIN_API onSize(Steinberg::ViewRect *newSize) override;
	Steinberg::tresult PLUGIN_API onFocus(Steinberg::TBool state) override;
	Steinberg::tresult PLUGIN_API setFrame(Steinberg::IPlugFrame *frame) override;
	Steinberg::tresult PLUGIN_API canResize() override;
	Steinberg::tresult PLUGIN_API checkSizeConstraint(Steinberg::ViewRect *rect) override;

	// ---- Called by the platform window (view_win.cpp / view_mac.mm).
	//
	// `native` is whatever that platform paints into: an HDC on Windows, a
	// CGContextRef on macOS. Both are opaque here, which is what lets the
	// Cocoa file compile without compat/gdi.h
	int  width() const { return m_w; }
	int  height() const { return m_h; }

	void repaint(void *native, int w, int h);
	void mouse_down(int x, int y);
	void mouse_drag(int x, int y);
	void mouse_up();
	void wheel(int x, int y, int steps);
	void key(int code, bool down);          // code is a plug_key
	void focus_lost();

private:
	struct impl;                            // the panel, and the Win32 backing store
	std::unique_ptr<impl> m_impl;

	engine &m_engine;
	plug_window *m_window = nullptr;
	int m_w = 1400, m_h = 360;
	Steinberg::int32 m_refs = 1;
	Steinberg::IPlugFrame *m_frame = nullptr;
};

} // namespace vst3
} // namespace smu2000

#endif // S_MU2000_VST3_VIEW_H
