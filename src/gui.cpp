// license:BSD-3-Clause
//
// 実機のフロントパネル風の画面で MU2000 を動かす。
//
//   gui <rom ディレクトリ> [--midi 番号] [--midi-b 番号]
//       [--midiout 番号] [--midiout-b 番号] [--latency ミリ秒]
//   gui --list                             MIDI の入口と出口の一覧
//   gui <rom ディレクトリ> --shot 絵.png    窓を出さずに絵だけ書き出す（見た目の確認用）
//
// 入口と出口は**動かしたまま画面から選べる**。パネルの MIDI IN A の
// ジャックを押すか、どこでも右クリックすると品書きが出る。選んだものは
// %LOCALAPPDATA%\S-MU2000\gui.ini に覚えておいて、次から使う。
//
// MU2000 の設定（ワーク RAM）は終わるときに残し、次の起動で使う（src/nvram.h）。
// --factory か右クリックの「工場出荷状態に戻す」で捨てられる。
//
// 音の作り方は live.exe と同じ。**時計を自分で持たない**（doc/design.md）。
// 画面は別スレッドで、音源とは ui::bridge 越しにしか触れ合わない。
//
// マウスホイールはダイヤルに割り当ててある。実機にもロータリー
// エンコーダがあり、VALUE -/+ のボタンと同じ働きをする。

#include "mu2000.h"
#include "nvram.h"
#include "smf.h"
#include "ui/audio_out.h"
#include "ui/bridge.h"
#include "ui/driver.h"
#include "ui/midi_in.h"
#include "ui/midi_out.h"
#include "ui/layout.h"
#include "ui/panel.h"
#include "ui/player.h"
#include "ui/text.h"
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
	ui::bridge   &br;
	ui::midi_in  &midi;        // MIDI IN A（パート 1-16）
	ui::midi_in  *midi_b = nullptr;   // MIDI IN B（パート 17-32）
	ui::midi_out *mout = nullptr;     // MIDI OUT A（A で受けたものを外へ）
	ui::midi_out *mout_b = nullptr;   // MIDI OUT B（B で受けたものを外へ）

	std::atomic<int> state{0};        // 0 起動中 / 1 準備完了 / 2 だめ
	// fill() が機械に触っている最中か。起動し直すときはこれが落ちるのを待つ
	std::atomic<bool> in_fill{false};
	bool use_nvram = false;           // 覚えている設定で起動するか（窓を出すときだけ）
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
		if (use_nvram && smu2000::nvram::load(mu))
			std::printf("設定: %s\n", smu2000::nvram::path(mu).c_str());
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

	// 工場出荷状態に戻す。覚えている設定を捨てて電源を入れ直す。
	// 音声の糸が機械から手を離すのを待ってから触る
	void factory_reset()
	{
		state.store(0);
		message = "工場出荷状態に戻している...";
		publish();
		while (in_fill.load())
			Sleep(1);

		const std::vector<u8> zero(mu.nvram().size(), 0);
		mu.set_nvram(zero.data(), zero.size());
		const bool keep = use_nvram;
		use_nvram = false;
		const bool ok = boot();
		use_nvram = keep;
		if (!ok) {
			state.store(2);
			publish();
			return;
		}
		// すぐ残す。ここで落ちても前の設定に戻らないように
		smu2000::nvram::save(mu);
		std::printf("工場出荷状態に戻した\n");
		std::fflush(stdout);
		state.store(1);
		publish();
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
		// 先に「触っている」を立ててから state を見る。逆にすると、見た直後に
		// 起動し直しが始まって、両方が機械に触ってしまう
		in_fill.store(true);
		if (state.load() != 1) {
			in_fill.store(false);
			std::memset(out, 0, size_t(n) * 4);
			return;
		}

		drv.apply_buttons(mu, br);
		// 画面から出したものも、外の MIDI 出力へ流す（実機の THRU）
		drv.pump_midi(mu, br, [this](u8 v) { if (mout) mout->send(v); });
		drv.pump_wheel(mu, br);

		u8 b;
		while (midi.pop(b)) {
			mu.midi_in(b, 0);
			if (mout) mout->send(b);
		}
		// B は実機の 2 つめの DIN（内蔵 SCI ch1）。パート 17-32 に届く。
		// THRU も口ごとに分ける。A で受けたものは MIDI OUT A、
		// B で受けたものは MIDI OUT B へ。混ぜると、外に繋いだ音源で
		// パートの割り振りが崩れる
		if (midi_b)
			while (midi_b->pop(b)) {
				mu.midi_in(b, 1);
				if (mout_b) mout_b->send(b);
			}

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
		in_fill.store(false);
	}
};


// ---- 窓

struct window_state {
	ui::panel   panel;
	ui::bridge *br = nullptr;
	engine     *eng = nullptr;
	ui::audio_out *out = nullptr;

	std::string audio_name;            // 音の出口（名前の一部）。空なら既定
	std::string layout_path;           // 読んでいる panel.txt。F5 で読み直す
	ui::player    play_file;
	ui::midi_in  *midi = nullptr;      // MIDI IN A
	ui::midi_in  *midi_b = nullptr;    // MIDI IN B
	ui::midi_out *mout = nullptr;      // MIDI OUT A
	ui::midi_out *mout_b = nullptr;    // MIDI OUT B
	std::thread   reboot;              // 工場出荷状態に戻す作業
	int  in_dev  = -1;                 // いま開いている番号。-1 は使っていない
	int  in_dev_b = -1;
	int  out_dev = -1;
	int  out_dev_b = -1;
	std::string in_name, in_name_b, out_name, out_name_b;


	// 二重書き用
	HDC     mem_dc = nullptr;
	HBITMAP mem_bmp = nullptr;
	int     mem_w = 0, mem_h = 0;
};

window_state g_win;

// ---- パネルの配置。作り直さずに文字ファイルで直せる（doc/panel-editing.md）

void apply_layout(const std::string &path, bool quiet)
{
	g_win.panel.lay() = ui::layout();          // まず既定値に戻す
	std::string err;
	if (!path.empty() && g_win.panel.lay().load(path, err)) {
		if (!quiet)
			std::printf("配置: %s\n", path.c_str());
	} else if (!path.empty() && !quiet) {
		std::printf("配置: %s を開けない。組み込みの配置を使う\n", path.c_str());
	}
	if (!err.empty())
		std::fprintf(stderr, "%s", err.c_str());
	std::fflush(stdout);
	g_win.panel.resize(g_win.panel.width(), g_win.panel.height());
}

// ---- 選んだ口を覚えておく
//
// 番号ではなく**名前**で覚える。USB の機器を挿し直すと番号がずれるので、
// 番号で覚えると次に開いたとき別の機器に繋がってしまう。

std::string settings_path()
{
	const char *base = std::getenv("LOCALAPPDATA");
	if (!base || !*base)
		return {};
	std::string dir = std::string(base) + "\\S-MU2000";
	CreateDirectoryA(dir.c_str(), nullptr);
	return dir + "\\gui.ini";
}

void load_settings(std::string &in_name, std::string &in_name_b,
                   std::string &out_name, std::string &out_name_b,
                   std::string &audio_name, float *volume = nullptr)
{
	const std::string path = settings_path();
	if (path.empty())
		return;
	FILE *f = std::fopen(path.c_str(), "rb");
	if (!f)
		return;
	char line[512];
	while (std::fgets(line, sizeof(line), f)) {
		std::string t(line);
		while (!t.empty() && (t.back() == '\n' || t.back() == '\r'))
			t.pop_back();
		const size_t eq = t.find('=');
		if (eq == std::string::npos)
			continue;
		const std::string key = t.substr(0, eq), val = t.substr(eq + 1);
		if (key == "midi_in")   in_name   = val;
		if (key == "midi_in_b") in_name_b = val;
		if (key == "midi_out")   out_name   = val;
		if (key == "midi_out_b") out_name_b = val;
		if (key == "audio_out")  audio_name = val;
		if (key == "volume" && volume && !val.empty())
			*volume = std::clamp(float(std::atof(val.c_str())), 0.0f, 1.0f);
	}
	std::fclose(f);
}

void save_settings()
{
	const std::string path = settings_path();
	if (path.empty())
		return;
	FILE *f = std::fopen(path.c_str(), "wb");
	if (!f)
		return;
	std::fprintf(f, "midi_in=%s\n",   g_win.in_name.c_str());
	std::fprintf(f, "midi_in_b=%s\n", g_win.in_name_b.c_str());
	std::fprintf(f, "midi_out=%s\n",   g_win.out_name.c_str());
	std::fprintf(f, "midi_out_b=%s\n", g_win.out_name_b.c_str());
	std::fprintf(f, "audio_out=%s\n", g_win.audio_name.c_str());
	// パネルの VOLUME のつまみ。実機でも DAC の後ろのアナログのつまみで、
	// firmware の RAM には入らないので、こちらで覚える
	if (g_win.br)
		std::fprintf(f, "volume=%.3f\n", g_win.br->gain());
	std::fclose(f);
}

// 名前で探す。見つからなければ -1
int find_device(const std::vector<std::string> &names, const std::string &want)
{
	if (want.empty())
		return -1;
	for (size_t i = 0; i < names.size(); i++)
		if (names[i] == want)
			return int(i);
	return -1;
}

// ---- 口を選ぶ品書き

enum : UINT {
	ID_IN_NONE = 900, ID_IN_BASE = 901,
	ID_INB_NONE = 1400, ID_INB_BASE = 1401,
	ID_OUT_NONE = 1900, ID_OUT_BASE = 1901,
	ID_OUTB_NONE = 2400, ID_OUTB_BASE = 2401,
	ID_PLAY_FILE = 2900, ID_STOP_FILE = 2901,
	ID_FACTORY = 3000,
};

// 品書きは **W 版**で作る。ソースは UTF-8 なので、A 版に渡すと
// CP932 と思われて文字化けする
void add_item(HMENU m, UINT flags, UINT_PTR id, const char *utf8)
{
	const std::wstring w = ui::to_wide(utf8);
	AppendMenuW(m, flags, id, w.c_str());
}

void fill_port_menu(HMENU m, const std::vector<std::string> &names, int now,
                    UINT id_none, UINT id_base)
{
	add_item(m, MF_STRING | (now < 0 ? MF_CHECKED : 0), id_none, "使わない");
	if (names.empty()) {
		AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
		add_item(m, MF_STRING | MF_GRAYED, 0, "（機器が無い）");
		return;
	}
	AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
	for (size_t i = 0; i < names.size(); i++)
		add_item(m, MF_STRING | (int(i) == now ? MF_CHECKED : 0),
		         id_base + UINT(i), names[i].c_str());
}

void show_port_menu(HWND hwnd, POINT screen)
{
	HMENU top = CreatePopupMenu();
	HMENU mi  = CreatePopupMenu();
	HMENU mib = CreatePopupMenu();
	HMENU mo  = CreatePopupMenu();
	HMENU mob = CreatePopupMenu();

	const auto ins  = ui::midi_in::list();
	const auto outs = ui::midi_out::list();
	fill_port_menu(mi,  ins,  g_win.in_dev,    ID_IN_NONE,   ID_IN_BASE);
	fill_port_menu(mib, ins,  g_win.in_dev_b,  ID_INB_NONE,  ID_INB_BASE);
	fill_port_menu(mo,  outs, g_win.out_dev,   ID_OUT_NONE,  ID_OUT_BASE);
	fill_port_menu(mob, outs, g_win.out_dev_b, ID_OUTB_NONE, ID_OUTB_BASE);

	add_item(top, MF_POPUP, UINT_PTR(mi),  "MIDI IN A（パート 1-16）");
	add_item(top, MF_POPUP, UINT_PTR(mib), "MIDI IN B（パート 17-32）");
	add_item(top, MF_POPUP, UINT_PTR(mo),  "MIDI OUT A（A で受けたものを外へ）");
	add_item(top, MF_POPUP, UINT_PTR(mob), "MIDI OUT B（B で受けたものを外へ）");
	AppendMenuW(top, MF_SEPARATOR, 0, nullptr);
	const bool ready = g_win.eng && g_win.eng->state.load() == 1;
	add_item(top, MF_STRING | (ready ? 0 : MF_GRAYED), ID_FACTORY, "工場出荷状態に戻す...");

	TrackPopupMenu(top, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
	               screen.x, screen.y, 0, hwnd, nullptr);
	DestroyMenu(top);
}

// ---- カードの差し込み口。MIDI ファイルを流す

void show_card_menu(HWND hwnd, POINT screen)
{
	HMENU m = CreatePopupMenu();
	const bool on = g_win.play_file.playing();
	add_item(m, MF_STRING, ID_PLAY_FILE, "MIDI ファイルを再生...");
	std::string stop = "止める";
	if (on)
		stop += "（" + g_win.play_file.name() + "）";
	add_item(m, MF_STRING | (on ? 0 : MF_GRAYED), ID_STOP_FILE, stop.c_str());
	TrackPopupMenu(m, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
	               screen.x, screen.y, 0, hwnd, nullptr);
	DestroyMenu(m);
}

void choose_midi_file(HWND hwnd)
{
	wchar_t file[MAX_PATH] = {};
	OPENFILENAMEW o{};
	o.lStructSize = sizeof(o);
	o.hwndOwner = hwnd;
	o.lpstrFilter = L"MIDI ファイル (*.mid;*.midi)\0*.mid;*.midi\0すべて (*.*)\0*.*\0";
	o.lpstrFile = file;
	o.nMaxFile = MAX_PATH;
	o.lpstrTitle = L"流す MIDI ファイル";
	o.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
	if (!GetOpenFileNameW(&o))
		return;

	std::string path = ui::to_utf8(file);
	std::string err;
	if (!g_win.play_file.start(path, *g_win.br, err)) {
		const std::wstring w = ui::to_wide("開けない: " + err);
		MessageBoxW(hwnd, w.c_str(), L"S-MU2000", MB_OK | MB_ICONWARNING);
		return;
	}
	std::printf("再生: %s（%.1f 秒)\n", path.c_str(), g_win.play_file.length());
	std::fflush(stdout);
}

// 覚えている設定を捨てて、電源を入れ直す
void choose_factory_reset(HWND hwnd)
{
	if (!g_win.eng || g_win.eng->state.load() != 1)
		return;
	if (MessageBoxW(hwnd,
	                L"MU2000 を工場出荷状態に戻して、電源を入れ直します。\n"
	                L"ユーティリティの設定や、覚えている音量・音色の設定はすべて消えます。",
	                L"S-MU2000", MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK)
		return;
	g_win.play_file.stop();
	if (g_win.reboot.joinable())
		g_win.reboot.join();
	g_win.reboot = std::thread([] { g_win.eng->factory_reset(); });
}

// 品書きで選ばれたものを開く。開けなかったら「使わない」に戻す
void choose_in(int dev)
{
	if (!g_win.midi)
		return;
	std::string err;
	if (!g_win.midi->open(dev, err)) {
		std::fprintf(stderr, "MIDI 入力: %s\n", err.c_str());
		g_win.midi->open(-1, err);
		dev = -1;
	}
	g_win.in_dev  = g_win.midi->is_open() ? dev : -1;
	g_win.in_name = g_win.midi->device_name();
	save_settings();
}

void choose_in_b(int dev)
{
	if (!g_win.midi_b)
		return;
	std::string err;
	if (!g_win.midi_b->open(dev, err)) {
		std::fprintf(stderr, "MIDI 入力 B: %s\n", err.c_str());
		g_win.midi_b->open(-1, err);
		dev = -1;
	}
	g_win.in_dev_b  = g_win.midi_b->is_open() ? dev : -1;
	g_win.in_name_b = g_win.midi_b->device_name();
	save_settings();
}

void choose_out(int dev)
{
	if (!g_win.mout)
		return;
	std::string err;
	if (!g_win.mout->open(dev, err)) {
		std::fprintf(stderr, "MIDI 出力: %s\n", err.c_str());
		g_win.mout->open(-1, err);
		dev = -1;
	}
	g_win.out_dev  = g_win.mout->is_open() ? dev : -1;
	g_win.out_name = g_win.mout->device_name();
	save_settings();
}

void choose_out_b(int dev)
{
	if (!g_win.mout_b)
		return;
	std::string err;
	if (!g_win.mout_b->open(dev, err)) {
		std::fprintf(stderr, "MIDI 出力 B: %s\n", err.c_str());
		g_win.mout_b->open(-1, err);
		dev = -1;
	}
	g_win.out_dev_b  = g_win.mout_b->is_open() ? dev : -1;
	g_win.out_name_b = g_win.mout_b->device_name();
	save_settings();
}

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
			              "CPU %.0f%%  最悪 %.1f ms  待ち %.0f ms  遅れ %llu   IN: %s   OUT: %s"
			              "   （MIDI IN A のジャックか右クリックで口を選ぶ）",
			              g_win.out->cpu_percent(), g_win.out->worst_ms(),
			              g_win.out->output_ms(),
			              (unsigned long long)g_win.out->late(),
			              g_win.in_name.empty()  ? "なし" : g_win.in_name.c_str(),
			              g_win.out_name.empty() ? "なし" : g_win.out_name.c_str());
		else
			std::snprintf(status, sizeof(status), "起動中...");
		g_win.panel.set_volume(g_win.br->gain());
		g_win.panel.paint(g_win.mem_dc, s, pressed, status);

		BitBlt(dc, 0, 0, w, h, g_win.mem_dc, 0, 0, SRCCOPY);
		EndPaint(hwnd, &ps);
		return 0;
	}

	case WM_LBUTTONDOWN: {
		const int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
		// パネルの MIDI IN A のジャックを押したら、口を選ぶ品書きを出す
		if (g_win.panel.on_midi_jack(mx, my)) {
			POINT pt{ mx, my };
			ClientToScreen(hwnd, &pt);
			show_port_menu(hwnd, pt);
			return 0;
		}
		// カードの差し込み口は MIDI ファイル
		if (g_win.panel.on_card_slot(mx, my)) {
			POINT pt{ mx, my };
			ClientToScreen(hwnd, &pt);
			show_card_menu(hwnd, pt);
			return 0;
		}
		SetCapture(hwnd);
		if (g_win.panel.press(mx, my, *g_win.br))
			InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	}

	case WM_RBUTTONUP: {
		const int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
		POINT pt{ mx, my };
		ClientToScreen(hwnd, &pt);
		if (g_win.panel.on_card_slot(mx, my))
			show_card_menu(hwnd, pt);
		else
			show_port_menu(hwnd, pt);
		return 0;
	}

	case WM_COMMAND: {
		const UINT id = LOWORD(wp);
		if (id == ID_IN_NONE)            choose_in(-1);
		else if (id >= ID_IN_BASE  && id < ID_IN_BASE + 256)  choose_in(int(id - ID_IN_BASE));
		else if (id == ID_INB_NONE)      choose_in_b(-1);
		else if (id >= ID_INB_BASE && id < ID_INB_BASE + 256) choose_in_b(int(id - ID_INB_BASE));
		else if (id == ID_OUT_NONE)      choose_out(-1);
		else if (id >= ID_OUT_BASE && id < ID_OUT_BASE + 256) choose_out(int(id - ID_OUT_BASE));
		else if (id == ID_OUTB_NONE)     choose_out_b(-1);
		else if (id >= ID_OUTB_BASE && id < ID_OUTB_BASE + 256) choose_out_b(int(id - ID_OUTB_BASE));
		else if (id == ID_PLAY_FILE) choose_midi_file(hwnd);
		else if (id == ID_STOP_FILE) g_win.play_file.stop();
		else if (id == ID_FACTORY) choose_factory_reset(hwnd);
		InvalidateRect(hwnd, nullptr, FALSE);
		return 0;
	}

	case WM_SETCURSOR: {
		// ジャックの上では指の形にして、押せることを見せる
		POINT pt;
		GetCursorPos(&pt);
		ScreenToClient(hwnd, &pt);
		if (LOWORD(lp) == HTCLIENT &&
		    (g_win.panel.on_midi_jack(pt.x, pt.y) ||
		     g_win.panel.on_card_slot(pt.x, pt.y))) {
			SetCursor(LoadCursor(nullptr, IDC_HAND));
			return TRUE;
		}
		break;
	}

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
		if (wp == VK_F5) {                      // 配置を読み直す
			apply_layout(g_win.layout_path, false);
			InvalidateRect(hwnd, nullptr, FALSE);
			return 0;
		}
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

int shot(const std::string &path, int w, int h, ui::bridge &br, bool grid,
         const std::string &layout_path)
{
	ui::panel p;
	std::string lerr;
	if (!layout_path.empty() && !p.lay().load(layout_path, lerr))
		std::fprintf(stderr, "配置: %s を開けない\n", layout_path.c_str());
	if (!lerr.empty())
		std::fprintf(stderr, "%s", lerr.c_str());
	p.resize(w, h);
	p.set_grid(grid);

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
	int midi_dev = -2;                 // -2 未指定（覚えているものを使う）/ -1 使わない
	int midib_dev = -2;                // MIDI IN B
	int moutb_dev = -2;                // MIDI OUT B
	int mout_dev = -2;
	int latency = 20;        // 溜める目標
	bool exclusive = false;
	const char *audio_dev = nullptr;
	bool factory = false;
	int win_w = 1400, win_h = 360;
	bool grid = false;
	std::string layout_path, dump_layout, play_path;
	bool boot_for_shot = false;
	std::string shot_mid;
	double shot_secs = 0.0;

	for (int i = 1; i < argc; i++) {
		if (!std::strcmp(argv[i], "--list")) {
			const auto ins = ui::midi_in::list();
			std::printf("MIDI 入力（--midi 番号 / 画面からも選べる）:\n");
			for (size_t k = 0; k < ins.size(); k++)
				std::printf("  %zu: %s\n", k, ins[k].c_str());
			if (ins.empty())
				std::printf("  （なし）\n");
			const auto outs = ui::midi_out::list();
			std::printf("MIDI 出力（--midiout 番号 / 受けたものをそのまま外へ）:\n");
			for (size_t k = 0; k < outs.size(); k++)
				std::printf("  %zu: %s\n", k, outs[k].c_str());
			if (outs.empty())
				std::printf("  （なし）\n");
			const auto aouts = ui::audio_out::list();
			std::printf("音声の出口（--audio に名前の一部）:\n");
			for (size_t k = 0; k < aouts.size(); k++)
				std::printf("  %zu: %s\n", k, aouts[k].c_str());
			return 0;
		}
		else if (!std::strcmp(argv[i], "--midi") && i + 1 < argc) midi_dev = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--midi-b") && i + 1 < argc) midib_dev = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--midiout") && i + 1 < argc) mout_dev = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--midiout-b") && i + 1 < argc) moutb_dev = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--nomidi")) { midi_dev = -1; midib_dev = -1; }
		else if (!std::strcmp(argv[i], "--latency") && i + 1 < argc) latency = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--exclusive")) exclusive = true;
		else if (!std::strcmp(argv[i], "--audio") && i + 1 < argc) audio_dev = argv[++i];
		else if (!std::strcmp(argv[i], "--factory")) factory = true;
		else if (!std::strcmp(argv[i], "--shot") && i + 1 < argc) shot_path = argv[++i];
		else if (!std::strcmp(argv[i], "--boot")) boot_for_shot = true;
		else if (!std::strcmp(argv[i], "--grid")) grid = true;
		else if (!std::strcmp(argv[i], "--layout") && i + 1 < argc) layout_path = argv[++i];
		else if (!std::strcmp(argv[i], "--play") && i + 1 < argc) play_path = argv[++i];
		else if (!std::strcmp(argv[i], "--dump-layout") && i + 1 < argc) dump_layout = argv[++i];
		else if (!std::strcmp(argv[i], "--mid") && i + 2 < argc) {
			shot_mid = argv[++i];
			shot_secs = std::atof(argv[++i]);
			boot_for_shot = true;
		}
		else if (!std::strcmp(argv[i], "--size") && i + 1 < argc) {
			if (std::sscanf(argv[++i], "%dx%d", &win_w, &win_h) != 2) { win_w = 1400; win_h = 360; }
		}
		else if (dir.empty()) dir = argv[i];
	}

	// --layout が無ければ、決まった場所を順に探す
	if (layout_path.empty())
		layout_path = ui::layout::find_default();

	if (!dump_layout.empty()) {
		ui::layout l;
		std::string lerr;
		if (!layout_path.empty())
			l.load(layout_path, lerr);
		if (!l.save(dump_layout)) {
			std::fprintf(stderr, "%s に書けない\n", dump_layout.c_str());
			return 1;
		}
		std::printf("いまの配置を書き出した: %s\n", dump_layout.c_str());
		std::printf("直したら --layout で渡すか、窓で F5 を押す\n");
		return 0;
	}

	static ui::bridge br;
	static ui::midi_in  midi, midi_b;
	static ui::midi_out mout, mout_b;

	// 絵だけ欲しい場合。ROM が無くても中身が空の画面は出せる
	if (!shot_path.empty() && (dir.empty() || !boot_for_shot)) {
		ui::snapshot s;
		std::snprintf(s.message, sizeof(s.message), "S-MU2000");
		br.publish(s);
		return shot(shot_path, win_w, win_h, br, grid, layout_path);
	}

	if (dir.empty()) {
		std::fprintf(stderr,
			"使い方: gui <rom ディレクトリ> [--midi 番号] [--midi-b 番号]"
			" [--midiout 番号] [--midiout-b 番号]"
			" [--latency ミリ秒] [--exclusive] [--layout panel.txt] [--play 曲.mid]\n"
			"        [--factory]   覚えている設定を捨てて工場出荷状態で起動する\n"
			"        gui --dump-layout panel.txt   いまの配置を書き出す\n"
			"        gui --list\n"
			"        gui [<rom ディレクトリ> --boot] --shot 絵.png [--size 1400x440]\n");
		return 1;
	}

	static engine eng(br, midi);
	eng.midi_b = &midi_b;
	eng.mout_b = &mout_b;
	eng.mout = &mout;
	if (!eng.load(dir)) {
		std::fprintf(stderr, "%s\n", eng.message.c_str());
		return 1;
	}

	// 絵だけ、ただし起動後の LCD が欲しい場合
	if (!shot_path.empty()) {
		if (!eng.boot()) { std::fprintf(stderr, "%s\n", eng.message.c_str()); return 1; }
		eng.state.store(1);

		// 起動直後は表示が動いている途中。少し空回しして落ち着かせる
		{
			s32 l, r;
			for (size_t i = 0; i < size_t(2.0 * RATE); i++)
				eng.mu.run_sample(l, r);
		}

		// レベルメータを出したいので、指定があれば MIDI を流しておく
		if (!shot_mid.empty()) {
			std::vector<smf::event> evs;
			std::string err;
			if (!smf::load(shot_mid, evs, err)) {
				std::fprintf(stderr, "%s\n", err.c_str());
			} else {
				std::printf("MIDI %zu 件を %.1f 秒ぶん流す\n", evs.size(), shot_secs);
				size_t at = 0;
				s32 l, r;
				for (size_t i = 0; i < size_t(shot_secs * RATE); i++) {
					const double now = double(i) / RATE;
					while (at < evs.size() && evs[at].time <= now) {
						for (u8 b : evs[at].bytes)
							eng.mu.midi_in(b);
						at++;
					}
					eng.mu.run_sample(l, r);
				}
			}
		}

		eng.publish();
		return shot(shot_path, win_w, win_h, br, grid, layout_path);
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

	g_win.br   = &br;
	g_win.eng  = &eng;
	// 窓を出すときだけ、覚えている設定で起動する（--shot は毎回同じ絵にしたい）
	eng.use_nvram = !factory;
	if (factory)
		std::printf("工場出荷状態で起動する（覚えていた設定は終わるときに上書きされる）\n");
	g_win.layout_path = layout_path;
	g_win.midi   = &midi;
	g_win.midi_b = &midi_b;
	g_win.mout   = &mout;
	g_win.mout_b = &mout_b;
	g_win.panel.resize(win_w, win_h);
	apply_layout(layout_path, false);
	g_win.panel.resize(win_w, win_h);
	{
		// VOLUME のつまみは前に閉じたときの位置から
		std::string a, b, c, d, e;
		float volume = 1.0f;
		load_settings(a, b, c, d, e, &volume);
		br.set_gain(volume);
	}

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

		// 前に選んだ口を名前で探す。--midi / --midiout があればそちらが勝つ
		std::string want_in, want_in_b, want_out, want_out_b, want_audio;
		load_settings(want_in, want_in_b, want_out, want_out_b, want_audio);
		// --audio があればそちらが勝つ。無ければ前に選んだもの
		g_win.audio_name = audio_dev ? std::string(audio_dev) : want_audio;
		if (midi_dev == -2)
			midi_dev = find_device(ui::midi_in::list(), want_in);
		if (midib_dev == -2)
			midib_dev = find_device(ui::midi_in::list(), want_in_b);
		if (mout_dev == -2)
			mout_dev = find_device(ui::midi_out::list(), want_out);
		if (moutb_dev == -2)
			moutb_dev = find_device(ui::midi_out::list(), want_out_b);

		choose_in(midi_dev);
		choose_in_b(midib_dev);
		choose_out(mout_dev);
		choose_out_b(moutb_dev);
		std::printf("MIDI IN A: %s\n",  g_win.in_name.empty()  ? "なし" : g_win.in_name.c_str());
		std::printf("MIDI IN B: %s\n",  g_win.in_name_b.empty() ? "なし" : g_win.in_name_b.c_str());
		std::printf("MIDI OUT A: %s\n", g_win.out_name.empty()   ? "なし" : g_win.out_name.c_str());
		std::printf("MIDI OUT B: %s\n", g_win.out_name_b.empty() ? "なし" : g_win.out_name_b.c_str());

		std::string err;

		if (!out.start(latency, [](s16 *o, u32 n) { eng.fill(o, n); }, err, exclusive,
		               g_win.audio_name)) {
			std::fprintf(stderr, "音声: %s\n", err.c_str());
			eng.message = "音声デバイスを開けない";
			eng.state.store(2);
			eng.publish();
			return;
		}
		// 開けた出口を覚える。**設定を読んで MIDI の口を開いた後でないと
		// いけない**。前はこれを起動直後にやっていて、まだ空の MIDI の名前で
		// gui.ini を上書きしていた（毎回 MIDI が「なし」に戻っていた）
		g_win.audio_name = out.device_name();
		std::printf("音声の出口: %s\n%s\n", out.device_name().c_str(),
		            out.format_line().c_str());
		save_settings();
		// --play が付いていれば、鳴り始めたところで流し出す
		if (!play_path.empty()) {
			std::string perr;
			if (!g_win.play_file.start(play_path, br, perr))
				std::fprintf(stderr, "MIDI ファイル: %s\n", perr.c_str());
			else
				std::printf("再生: %s（%.1f 秒）\n", play_path.c_str(),
				            g_win.play_file.length());
		}
		std::printf("鳴らしている（待ち時間 %.1f ms、MMCSS %s）\n",
		            1000.0 * out.buffer_frames() / RATE,
		            out.mmcss() ? "登録できた" : "登録できない（途切れやすい）");
		std::fflush(stdout);
	});

	MSG msg;
	while (GetMessageA(&msg, nullptr, 0, 0) > 0) {
		TranslateMessage(&msg);
		DispatchMessageA(&msg);
	}

	out.stop();
	if (boot_thread.joinable())
		boot_thread.join();
	if (g_win.reboot.joinable())
		g_win.reboot.join();
	save_settings();   // VOLUME のつまみの位置
	// 音はもう止まっている。起動できていたときだけ残す
	if (eng.state.load() == 1 && !smu2000::nvram::save(eng.mu))
		std::fprintf(stderr, "設定を残せなかった: %s\n", smu2000::nvram::path(eng.mu).c_str());
	g_win.play_file.stop();
	midi.close();
	mout.close();

	if (out.produced())
		std::printf("CPU %.1f%%、1 回の最悪 %.2f ms、間に合わなかった %llu 回\n",
		            out.cpu_percent(), out.worst_ms(),
		            (unsigned long long)out.late());
		std::printf("%s\n%s\n", out.format_line().c_str(), out.latency_line().c_str());
	return 0;
}
