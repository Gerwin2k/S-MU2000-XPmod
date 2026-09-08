// license:BSD-3-Clause
//
// 実機のフロントパネル風の画面で MU2000 を動かす。
//
//   gui <rom ディレクトリ> [--midi 番号] [--latency ミリ秒] [--size 1400x420]
//   gui --list                             MIDI 入力の一覧
//   gui <rom ディレクトリ> --shot 絵.png    窓を出さずに絵だけ書き出す（見た目の確認用）
//
// 音の作り方は live.exe と同じ。**時計を自分で持たない**（doc/design.md）。
// 画面は別スレッドで、音源とは ui::bridge 越しにしか触れ合わない。
//
// マウスホイールは VALUE の連打に割り当ててある。実機の MU2000 に
// ジョグダイヤルは無く VALUE -/+ のボタンなので、回した分だけ叩いている。

#include "mu2000.h"
#include "ui/audio_out.h"
#include "ui/bridge.h"
#include "ui/driver.h"
#include "ui/midi_in.h"
#include "ui/panel.h"
#include "ui/png.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <windows.h>
#include <windowsx.h>

namespace {

constexpr u32 RATE = ui::AUDIO_RATE;

// ---- 音源側

struct engine {
	mu2000 mu;
	ui::bridge  &br;
	ui::midi_in &midi;

	std::atomic<int> state{0};        // 0 起動中 / 1 準備完了 / 2 だめ
	std::string      message = "起動中...";

	ui::driver drv;

	engine(ui::bridge &b, ui::midi_in &m) : br(b), midi(m) {}

	bool load(const std::string &dir)
	{
		if (!mu.load_program(dir + "/mu2000_flash.bin")) { message = mu.error(); return false; }
		if (!mu.load_wave(dir + "/dump"))                { message = mu.error(); return false; }
		if (!mu.load_sintab(dir + "/standin/sin-table.bin"))
			std::fprintf(stderr, "警告: %s\n", mu.error().c_str());
		if (!mu.load_lcd_font(dir + "/hd44780u_b04.bin") &&
		    !mu.load_lcd_font(dir + "/standin/hd44780u_b04.bin"))
			std::fprintf(stderr, "警告: %s\n", mu.error().c_str());
		return true;
	}

	// 起動（実機と同じ空回し）。窓を出したあと別スレッドで進める
	bool boot()
	{
		mu.set_threaded(true);
		mu.reset();
		const size_t limit = size_t(30.0 * RATE);
		size_t i = 0;
		s32 l, r;
		for (; i < limit && !mu.midi_ready(); i++)
			mu.run_sample(l, r);
		if (i >= limit) {
			message = "起動しなかった";
			return false;
		}
		publish();
		return true;
	}

	void publish()
	{
		if (state.load() == 1)
			ui::driver::publish_now(mu, br, true, nullptr);
		else
			ui::driver::publish_message(br, message.c_str());
	}

	// 音声デバイスに頼まれた分だけ進める
	void fill(s16 *out, u32 n)
	{
		if (state.load() != 1) {
			std::memset(out, 0, size_t(n) * 4);
			return;
		}

		drv.apply_buttons(mu, br);
		drv.pump_midi(mu, br);
		drv.pump_wheel(mu, br);

		u8 b;
		while (midi.pop(b))
			mu.midi_in(b);

		const float g = br.gain();

		for (u32 i = 0; i < n; i++) {
			s32 l = 0, r = 0;
			mu.run_sample(l, r);
			l = s32(l * g) * 32768 / mu2000::DAC_FULL_SCALE;
			r = s32(r * g) * 32768 / mu2000::DAC_FULL_SCALE;
			out[i * 2 + 0] = s16(l < -32768 ? -32768 : l > 32767 ? 32767 : l);
			out[i * 2 + 1] = s16(r < -32768 ? -32768 : r > 32767 ? 32767 : r);
		}

		drv.publish(mu, br, n, RATE, true, nullptr);
	}
};


// ---- 窓

struct window_state {
	ui::panel   panel;
	ui::bridge *br = nullptr;
	engine     *eng = nullptr;
	ui::audio_out *out = nullptr;
	std::string midi_name;


	// 二重書き用
	HDC     mem_dc = nullptr;
	HBITMAP mem_bmp = nullptr;
	int     mem_w = 0, mem_h = 0;
};

window_state g_win;

void ensure_backing(HDC dc, int w, int h)
{
	if (g_win.mem_dc && g_win.mem_w == w && g_win.mem_h == h)
		return;
	if (g_win.mem_bmp) DeleteObject(g_win.mem_bmp);
	if (g_win.mem_dc)  DeleteDC(g_win.mem_dc);
	g_win.mem_dc = CreateCompatibleDC(dc);
	g_win.mem_bmp = CreateCompatibleBitmap(dc, w, h);
	SelectObject(g_win.mem_dc, g_win.mem_bmp);
	g_win.mem_w = w;
	g_win.mem_h = h;
}

// キーボードからも押せるように。並びは MAME の mu2000 と同じ
mu2000::button key_to_button(WPARAM vk, bool &ok)
{
	ok = true;
	switch (vk) {
	case 'A': return mu2000::button::play;
	case 'E': return mu2000::button::edit;
	case 'U': return mu2000::button::util;
	case 'F': return mu2000::button::effect;
	case 'S': return mu2000::button::mute_solo;
	case VK_OEM_6: return mu2000::button::part_plus;     // ]
	case VK_OEM_4: return mu2000::button::part_minus;    // [
	case VK_OEM_PLUS:  return mu2000::button::value_plus;
	case VK_OEM_MINUS: return mu2000::button::value_minus;
	case VK_BACK:   return mu2000::button::exit;
	case VK_RETURN: return mu2000::button::enter;
	case VK_OEM_PERIOD: return mu2000::button::select_right;
	case VK_OEM_COMMA:  return mu2000::button::select_left;
	case 'Q': return mu2000::button::seq;
	case 'Z': return mu2000::button::audition;
	case 'X': return mu2000::button::select;
	case 'M': return mu2000::button::sampling_mode;
	default: break;
	}
	ok = false;
	return mu2000::button::count;
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
	switch (msg) {
	case WM_CREATE:
		SetTimer(hwnd, 1, 33, nullptr);        // 30 コマ／秒で描き直す
		return 0;

	case WM_TIMER:
		InvalidateRect(hwnd, nullptr, FALSE);
		return 0;

	case WM_SIZE:
		g_win.panel.resize(LOWORD(lp), HIWORD(lp));
		InvalidateRect(hwnd, nullptr, FALSE);
		return 0;

	case WM_ERASEBKGND:
		return 1;                               // 全部自分で描く

	case WM_PAINT: {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(hwnd, &ps);
		RECT cr;
		GetClientRect(hwnd, &cr);
		const int w = cr.right, h = cr.bottom;
		ensure_backing(dc, w, h);

		ui::snapshot s;
		g_win.br->read(s);
		u64 pressed = g_win.br->buttons();
		char status[128] = {};
		if (g_win.out && g_win.out->produced())
			std::snprintf(status, sizeof(status),
			              "CPU %.0f%%   最悪 %.1f ms   枯渇 %llu   MIDI: %s",
			              g_win.out->cpu_percent(), g_win.out->worst_ms(),
			              (unsigned long long)g_win.out->starved(),
			              g_win.midi_name.empty() ? "なし" : g_win.midi_name.c_str());
		else
			std::snprintf(status, sizeof(status), "起動中...");
		g_win.panel.set_volume(g_win.br->gain());
		g_win.panel.paint(g_win.mem_dc, s, pressed, status);

		BitBlt(dc, 0, 0, w, h, g_win.mem_dc, 0, 0, SRCCOPY);
		EndPaint(hwnd, &ps);
		return 0;
	}

	case WM_LBUTTONDOWN:
		SetCapture(hwnd);
		if (g_win.panel.press(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), *g_win.br))
			InvalidateRect(hwnd, nullptr, FALSE);
		return 0;

	case WM_MOUSEMOVE:
		if (g_win.panel.drag(GET_X_LPARAM(lp), GET_Y_LPARAM(lp), *g_win.br))
			InvalidateRect(hwnd, nullptr, FALSE);
		return 0;

	case WM_LBUTTONUP:
		g_win.panel.release(*g_win.br);
		ReleaseCapture();
		InvalidateRect(hwnd, nullptr, FALSE);
		return 0;

	case WM_MOUSEWHEEL: {
		POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
		ScreenToClient(hwnd, &pt);
		const int delta = GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA;
		if (delta && g_win.panel.wheel_at(pt.x, pt.y, delta, *g_win.br))
			InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	}

	case WM_KEYDOWN: {
		if (lp & (1 << 30))                     // 押しっぱなしの繰り返しは無視
			return 0;
		bool ok = false;
		const mu2000::button b = key_to_button(wp, ok);
		if (ok) g_win.br->press(b, true);
		return 0;
	}

	case WM_KEYUP: {
		bool ok = false;
		const mu2000::button b = key_to_button(wp, ok);
		if (ok) g_win.br->press(b, false);
		return 0;
	}

	case WM_KILLFOCUS:
		g_win.br->release_all();                // 窓から離れたら全部離す
		return 0;

	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcA(hwnd, msg, wp, lp);
}


// ---- 窓を出さずに絵だけ書き出す。見た目を直すときに使う

int shot(const std::string &path, int w, int h, ui::bridge &br)
{
	ui::panel p;
	p.resize(w, h);

	BITMAPINFO bi{};
	bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
	bi.bmiHeader.biWidth = w;
	bi.bmiHeader.biHeight = -h;                 // 上から下へ
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;

	void *bits = nullptr;
	HDC screen = GetDC(nullptr);
	HDC dc = CreateCompatibleDC(screen);
	HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
	SelectObject(dc, bmp);

	ui::snapshot s;
	br.read(s);
	p.set_volume(0.8);
	p.paint(dc, s, 0, "");
	GdiFlush();

	const bool ok = ui::write_png(path, static_cast<const u8 *>(bits), w, h, w * 4);

	DeleteObject(bmp);
	DeleteDC(dc);
	ReleaseDC(nullptr, screen);

	std::printf(ok ? "書き出した: %s（%d×%d）\n" : "書き出せない: %s\n", path.c_str(), w, h);
	return ok ? 0 : 1;
}

} // namespace


int main(int argc, char **argv)
{
	SetConsoleOutputCP(CP_UTF8);

	std::string dir, shot_path;
	int midi_dev = -2;                 // -2 未指定 / -1 使わない
	int latency = 30;
	int win_w = 1400, win_h = 360;
	bool boot_for_shot = false;

	for (int i = 1; i < argc; i++) {
		if (!std::strcmp(argv[i], "--list")) {
			const auto names = ui::midi_in::list();
			std::printf("MIDI 入力:\n");
			for (size_t k = 0; k < names.size(); k++)
				std::printf("  %zu: %s\n", k, names[k].c_str());
			if (names.empty())
				std::printf("  （なし）\n");
			return 0;
		}
		else if (!std::strcmp(argv[i], "--midi") && i + 1 < argc) midi_dev = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--nomidi")) midi_dev = -1;
		else if (!std::strcmp(argv[i], "--latency") && i + 1 < argc) latency = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--shot") && i + 1 < argc) shot_path = argv[++i];
		else if (!std::strcmp(argv[i], "--boot")) boot_for_shot = true;
		else if (!std::strcmp(argv[i], "--size") && i + 1 < argc) {
			if (std::sscanf(argv[++i], "%dx%d", &win_w, &win_h) != 2) { win_w = 1400; win_h = 360; }
		}
		else if (dir.empty()) dir = argv[i];
	}

	static ui::bridge br;
	static ui::midi_in midi;

	// 絵だけ欲しい場合。ROM が無くても中身が空の画面は出せる
	if (!shot_path.empty() && (dir.empty() || !boot_for_shot)) {
		ui::snapshot s;
		std::snprintf(s.message, sizeof(s.message), "S-MU2000");
		br.publish(s);
		return shot(shot_path, win_w, win_h, br);
	}

	if (dir.empty()) {
		std::fprintf(stderr,
			"使い方: gui <rom ディレクトリ> [--midi 番号] [--latency ミリ秒]\n"
			"        gui --list\n"
			"        gui [<rom ディレクトリ> --boot] --shot 絵.png [--size 1400x440]\n");
		return 1;
	}

	static engine eng(br, midi);
	if (!eng.load(dir)) {
		std::fprintf(stderr, "%s\n", eng.message.c_str());
		return 1;
	}

	// 絵だけ、ただし起動後の LCD が欲しい場合
	if (!shot_path.empty()) {
		if (!eng.boot()) { std::fprintf(stderr, "%s\n", eng.message.c_str()); return 1; }
		eng.state.store(1);
		eng.publish();
		return shot(shot_path, win_w, win_h, br);
	}

	// ---- 窓を出す

	const HINSTANCE inst = GetModuleHandleA(nullptr);
	WNDCLASSA wc{};
	wc.lpfnWndProc   = wnd_proc;
	wc.hInstance     = inst;
	wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
	wc.lpszClassName = "SMU2000Panel";
	wc.hbrBackground = nullptr;
	RegisterClassA(&wc);

	RECT want{ 0, 0, win_w, win_h };
	AdjustWindowRect(&want, WS_OVERLAPPEDWINDOW, FALSE);
	HWND hwnd = CreateWindowA("SMU2000Panel", "S-MU2000", WS_OVERLAPPEDWINDOW,
	                          CW_USEDEFAULT, CW_USEDEFAULT,
	                          want.right - want.left, want.bottom - want.top,
	                          nullptr, nullptr, inst, nullptr);
	if (!hwnd) {
		std::fprintf(stderr, "窓を出せない\n");
		return 1;
	}

	g_win.br  = &br;
	g_win.eng = &eng;
	g_win.panel.resize(win_w, win_h);
	br.set_gain(1.0f);

	eng.publish();
	ShowWindow(hwnd, SW_SHOW);
	UpdateWindow(hwnd);

	// 起動は別スレッド。終わったら音を出し始める
	static ui::audio_out out;
	g_win.out = &out;
	std::thread boot_thread([&] {
		if (!eng.boot()) {
			eng.state.store(2);
			eng.publish();
			return;
		}
		eng.state.store(1);
		eng.publish();

		std::string err;
		if (midi_dev == -2)
			midi_dev = ui::midi_in::list().empty() ? -1 : 0;
		if (!midi.open(midi_dev, err))
			std::fprintf(stderr, "MIDI: %s\n", err.c_str());
		else if (midi.is_open())
			std::printf("MIDI 入力: %s\n", midi.device_name().c_str());

		if (!out.start(latency, [](s16 *o, u32 n) { eng.fill(o, n); }, err)) {
			std::fprintf(stderr, "音声: %s\n", err.c_str());
			eng.message = "音声デバイスを開けない";
			eng.state.store(2);
			eng.publish();
			return;
		}
		std::printf("鳴らしている（待ち時間 %.1f ms）\n",
		            1000.0 * out.buffer_frames() / RATE);
	});

	MSG msg;
	while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageA(&msg);
	}

	out.stop();
	if (boot_thread.joinable())
		boot_thread.join();
	midi.close();

	if (out.produced())
		std::printf("CPU %.1f%%、1 回の最悪 %.2f ms、枯渇 %llu 回\n",
		            out.cpu_percent(), out.worst_ms(),
		            (unsigned long long)out.starved());
	return 0;
}
