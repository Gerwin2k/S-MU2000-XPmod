// license:BSD-3-Clause
//
// Run the MU2000 behind a front panel that looks like the real machine (macOS).
//
//   gui <rom directory> [--midi n] [--midi-b n]
//       [--midiout n] [--midiout-b n] [--latency ms]
//   gui --list                             list the MIDI inputs and outputs
//   gui <rom directory> --shot image.png   write the picture without a window
//
// The Windows version of this is gui.cpp, and this is the same program: the
// same panel, the same engine, the same arguments. The differences are the
// ones the platform forces.
//
//   * the window is AppKit (src/ui/window_mac.mm) rather than Win32, which is
//     a separate file because the Cocoa headers and compat/gdi.h cannot both
//     be visible at once
//   * the port picker is an NSMenu and choosing a MIDI file is an NSOpenPanel,
//     so both are asked for through ui::mac_app instead of built here
//   * drawing goes into the view's CGContext through the GDI shim, so panel.cpp
//     is literally the same code that paints the Windows window
//   * settings live in ~/Library/Application Support/S-MU2000/gui.ini
//
// Audio is produced the same way as in live: **it keeps no clock of its own**
// (doc/design.md).
//
// The mouse wheel drives the dial. The real machine has a rotary encoder in
// that spot too, and it does the same job as the VALUE -/+ buttons.

#include "compat/console.h"
#include "compat/gdi.h"
#include "compat/paths.h"
#include "mu2000.h"
#include "smf.h"
#include "ui/audio_out.h"
#include "ui/bridge.h"
#include "ui/engine.h"
#include "ui/layout.h"
#include "ui/midi_in.h"
#include "ui/midi_out.h"
#include "ui/panel.h"
#include "ui/player.h"
#include "ui/png.h"
#include "ui/window_mac.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr u32 RATE = ui::AUDIO_RATE;

// Menu command numbers for picking a port. Kept the same as in gui.cpp
enum : int {
	ID_IN_NONE = 900,   ID_IN_BASE = 901,
	ID_INB_NONE = 1400, ID_INB_BASE = 1401,
	ID_OUT_NONE = 1900, ID_OUT_BASE = 1901,
	ID_OUTB_NONE = 2400, ID_OUTB_BASE = 2401,
	ID_PLAY_FILE = 2900, ID_STOP_FILE = 2901,
};

// ---- Remember the chosen ports
//
// They are remembered by **name**, not by number. Replugging a USB device
// shifts the numbers, so a remembered number would connect to a different
// device the next time the window is opened.

std::string settings_path()
{
	const std::string dir = smu2000::ensure_config_dir();
	return dir.empty() ? std::string() : dir + "gui.ini";
}

struct port_names { std::string in, in_b, out, out_b; };

port_names load_settings()
{
	port_names n;
	const std::string path = settings_path();
	if (path.empty())
		return n;
	FILE *f = std::fopen(path.c_str(), "rb");
	if (!f)
		return n;
	char line[512];
	while (std::fgets(line, sizeof(line), f)) {
		std::string t(line);
		while (!t.empty() && (t.back() == '\n' || t.back() == '\r'))
			t.pop_back();
		const size_t eq = t.find('=');
		if (eq == std::string::npos)
			continue;
		const std::string key = t.substr(0, eq), val = t.substr(eq + 1);
		if (key == "midi_in")    n.in   = val;
		if (key == "midi_in_b")  n.in_b = val;
		if (key == "midi_out")   n.out  = val;
		if (key == "midi_out_b") n.out_b = val;
	}
	std::fclose(f);
	return n;
}

void save_settings(const port_names &n)
{
	const std::string path = settings_path();
	if (path.empty())
		return;
	FILE *f = std::fopen(path.c_str(), "wb");
	if (!f)
		return;
	std::fprintf(f, "midi_in=%s\n",   n.in.c_str());
	std::fprintf(f, "midi_in_b=%s\n", n.in_b.c_str());
	std::fprintf(f, "midi_out=%s\n",  n.out.c_str());
	std::fprintf(f, "midi_out_b=%s\n", n.out_b.c_str());
	std::fclose(f);
}

// Look a port up by name. -1 when it is not there
int find_device(const std::vector<std::string> &names, const std::string &want)
{
	if (want.empty())
		return -1;
	for (size_t i = 0; i < names.size(); i++)
		if (names[i] == want)
			return int(i);
	return -1;
}

// ---- Keyboard. The layout matches MAME's mu2000 and gui.cpp
//
// Letters arrive in lower case. macOS hands over the character with Shift
// already stripped, so both '=' and '+' have to be listed.

bool key_to_button(int code, mu2000::button &out)
{
	switch (code) {
	case 'a': out = mu2000::button::play;         return true;
	case 'e': out = mu2000::button::edit;         return true;
	case 'u': out = mu2000::button::util;         return true;
	case 'f': out = mu2000::button::effect;       return true;
	case 's': out = mu2000::button::mute_solo;    return true;
	case ']': out = mu2000::button::part_plus;    return true;
	case '[': out = mu2000::button::part_minus;   return true;
	case '=': case '+': out = mu2000::button::value_plus;  return true;
	case '-': out = mu2000::button::value_minus;  return true;
	case '\r': out = mu2000::button::enter;       return true;
	case 0x7f: case 0x08: out = mu2000::button::exit; return true;
	case '.': out = mu2000::button::select_right; return true;
	case ',': out = mu2000::button::select_left;  return true;
	case 'q': out = mu2000::button::seq;          return true;
	case 'z': out = mu2000::button::audition;     return true;
	case 'x': out = mu2000::button::select;       return true;
	case 'm': out = mu2000::button::sampling_mode; return true;
	default: break;
	}
	return false;
}

// ---- Things handed to the window

ui::menu_item item(const char *label, int id, bool checked, bool enabled)
{
	ui::menu_item m;
	m.label = label;
	m.id = id;
	m.checked = checked;
	m.enabled = enabled;
	return m;
}

ui::menu_group port_group(const char *title, const std::vector<std::string> &names,
                          int now, int id_none, int id_base)
{
	ui::menu_group g;
	g.title = title;
	g.items.push_back(item("使わない", id_none, now < 0, true));
	if (names.empty()) {
		ui::menu_item sep;
		sep.separator = true;
		g.items.push_back(sep);
		g.items.push_back(item("（機器が無い）", 0, false, false));
		return g;
	}
	ui::menu_item sep;
	sep.separator = true;
	g.items.push_back(sep);
	for (size_t i = 0; i < names.size(); i++)
		g.items.push_back(item(names[i].c_str(), id_base + int(i), int(i) == now, true));
	return g;
}


// ---- The screen. Paints the panel, feeds it input, builds the menus

class app : public ui::mac_app
{
public:
	app(ui::bridge &b, ui::midi_in &mi, ui::midi_in &mib,
	    ui::midi_out &mo, ui::midi_out &mob)
	    : br(b), midi(mi), midi_b(mib), mout(mo), mout_b(mob) {}

	ui::panel  panel;
	ui::player play;

	std::string layout_path;

	// ---- mac_app

	void draw(void *cg, int w, int h) override
	{
		// パラメータの層: 音源の返事を読み、見えている面の読み返しを頼む。
		// The window's timer is where this has to happen: it touches the bridge,
		// so it must not run on the audio thread (same as gui.cpp's WM_TIMER)
		panel.tick(br);

		ui::snapshot s;
		br.read(s);
		const u64 pressed = br.buttons();

		char status[256] = {};
		if (out && out->produced())
			std::snprintf(status, sizeof(status),
			              "CPU %.0f%%  最悪 %.1f ms  枯渇 %llu   IN: %s   OUT: %s"
			              "   （MIDI IN A のジャックか右クリックで口を選ぶ）",
			              out->cpu_percent(), out->worst_ms(),
			              (unsigned long long)out->starved(),
			              in_name.empty()  ? "なし" : in_name.c_str(),
			              out_name.empty() ? "なし" : out_name.c_str());
		else
			std::snprintf(status, sizeof(status), "起動中...");

		panel.set_volume(br.gain());

		// The view's context is already top-left, y down, so it can be handed
		// to the shim as it stands
		HDC dc = static_cast<HDC>(smu_gdi_wrap_view_context(cg, w, h));
		panel.paint(dc, s, pressed, status);
		DeleteDC(dc);
	}

	void resized(int w, int h) override
	{
		panel.resize(w, h);
	}

	bool mouse_down(int x, int y, bool right) override
	{
		// A secondary click opens the port picker wherever it lands; on the
		// card slot it opens the file menu instead. Same as gui.cpp does on
		// WM_RBUTTONUP
		if (right)
			return true;

		// The jack and the card slot are pressed rather than clicked: they
		// open a menu instead of moving a panel control
		if (panel.on_midi_jack(x, y) || panel.on_card_slot(x, y))
			return true;

		m_pressed = true;
		panel.press(x, y, br);
		return false;
	}

	void mouse_drag(int x, int y) override
	{
		if (m_pressed)
			panel.drag(x, y, br);
	}

	void mouse_up() override
	{
		if (!m_pressed)
			return;
		m_pressed = false;
		panel.release(br);
	}

	void wheel(int x, int y, int steps) override
	{
		if (steps)
			panel.wheel_at(x, y, steps, br);
	}

	void key(int code, bool down) override
	{
		if (code == ui::MAC_KEY_FUNCTION_BASE + 0x60) {      // F5
			if (down)
				reload_layout();
			return;
		}
		mu2000::button b = mu2000::button::count;
		if (key_to_button(code, b))
			br.press(b, down);
	}

	void focus_lost() override
	{
		m_pressed = false;
		br.release_all();
	}

	bool hand_cursor(int x, int y) override
	{
		return panel.on_midi_jack(x, y) || panel.on_card_slot(x, y);
	}

	std::vector<ui::menu_group> context_menu(int x, int y) override
	{
		std::vector<ui::menu_group> groups;

		if (panel.on_card_slot(x, y)) {
			ui::menu_group g;
			g.items.push_back(item("MIDI ファイルを再生...", ID_PLAY_FILE, false, true));
			std::string stop = "止める";
			if (play.playing())
				stop += "（" + play.name() + "）";
			g.items.push_back(item(stop.c_str(), ID_STOP_FILE, false, play.playing()));
			groups.push_back(g);
			return groups;
		}

		const auto ins  = ui::midi_in::list();
		const auto outs = ui::midi_out::list();
		groups.push_back(port_group("MIDI IN A（パート 1-16）", ins, in_dev,
		                            ID_IN_NONE, ID_IN_BASE));
		groups.push_back(port_group("MIDI IN B（パート 17-32）", ins, in_dev_b,
		                            ID_INB_NONE, ID_INB_BASE));
		groups.push_back(port_group("MIDI OUT A（A で受けたものを外へ）", outs, out_dev,
		                            ID_OUT_NONE, ID_OUT_BASE));
		groups.push_back(port_group("MIDI OUT B（B で受けたものを外へ）", outs, out_dev_b,
		                            ID_OUTB_NONE, ID_OUTB_BASE));
		return groups;
	}

	void menu_chosen(int id) override
	{
		if (id == ID_IN_NONE)                                        choose_in(-1);
		else if (id >= ID_IN_BASE  && id < ID_IN_BASE  + 256)         choose_in(id - ID_IN_BASE);
		else if (id == ID_INB_NONE)                                   choose_in_b(-1);
		else if (id >= ID_INB_BASE && id < ID_INB_BASE + 256)         choose_in_b(id - ID_INB_BASE);
		else if (id == ID_OUT_NONE)                                   choose_out(-1);
		else if (id >= ID_OUT_BASE && id < ID_OUT_BASE + 256)         choose_out(id - ID_OUT_BASE);
		else if (id == ID_OUTB_NONE)                                  choose_out_b(-1);
		else if (id >= ID_OUTB_BASE && id < ID_OUTB_BASE + 256)       choose_out_b(id - ID_OUTB_BASE);
		else if (id == ID_PLAY_FILE) {
			const std::string path = ui::open_midi_file_panel();
			if (!path.empty())
				play_song(path);
		}
		else if (id == ID_STOP_FILE) play.stop();
	}

	void reload_layout() override
	{
		apply_layout(layout_path, false);
	}

	// ---- the rest

	void set_layout(const std::string &path)
	{
		layout_path = path;
		panel.lay() = ui::layout();
		std::string err;
		if (!path.empty() && !panel.lay().load(path, err))
			std::printf("配置: %s を開けない。組み込みの配置を使う\n", path.c_str());
		if (!err.empty())
			std::fprintf(stderr, "%s", err.c_str());
		panel.resize(panel.width(), panel.height());
	}

	void apply_layout(const std::string &path, bool quiet)
	{
		panel.lay() = ui::layout();
		std::string err;
		if (!path.empty() && panel.lay().load(path, err)) {
			if (!quiet)
				std::printf("配置: %s\n", path.c_str());
		} else if (!path.empty() && !quiet) {
			std::printf("配置: %s を開けない。組み込みの配置を使う\n", path.c_str());
		}
		if (!err.empty())
			std::fprintf(stderr, "%s", err.c_str());
		std::fflush(stdout);
		panel.resize(panel.width(), panel.height());
	}

	void play_song(const std::string &path)
	{
		std::string err;
		if (!play.start(path, br, err)) {
			std::fprintf(stderr, "開けない: %s\n", err.c_str());
			return;
		}
		std::printf("再生: %s（%.1f 秒）\n", path.c_str(), play.length());
		std::fflush(stdout);
	}

	// Open what the menu picked. On failure it falls back to "unused".
	void choose_in(int dev)
	{
		std::string err;
		if (!midi.open(dev, err)) {
			std::fprintf(stderr, "MIDI 入力: %s\n", err.c_str());
			midi.open(-1, err);
			dev = -1;
		}
		in_dev  = midi.is_open() ? dev : -1;
		in_name = midi.device_name();
		remember();
	}

	void choose_in_b(int dev)
	{
		std::string err;
		if (!midi_b.open(dev, err)) {
			std::fprintf(stderr, "MIDI 入力 B: %s\n", err.c_str());
			midi_b.open(-1, err);
			dev = -1;
		}
		in_dev_b  = midi_b.is_open() ? dev : -1;
		in_name_b = midi_b.device_name();
		remember();
	}

	void choose_out(int dev)
	{
		std::string err;
		if (!mout.open(dev, err)) {
			std::fprintf(stderr, "MIDI 出力: %s\n", err.c_str());
			mout.open(-1, err);
			dev = -1;
		}
		out_dev  = mout.is_open() ? dev : -1;
		out_name = mout.device_name();
		remember();
	}

	void choose_out_b(int dev)
	{
		std::string err;
		if (!mout_b.open(dev, err)) {
			std::fprintf(stderr, "MIDI 出力 B: %s\n", err.c_str());
			mout_b.open(-1, err);
			dev = -1;
		}
		out_dev_b  = mout_b.is_open() ? dev : -1;
		out_name_b = mout_b.device_name();
		remember();
	}

	// Remembered by name rather than number (see the note on settings_path)
	void remember() const
	{
		save_settings(port_names{ in_name, in_name_b, out_name, out_name_b });
	}

	int in_dev = -1, in_dev_b = -1, out_dev = -1, out_dev_b = -1;
	std::string in_name, in_name_b, out_name, out_name_b;

	ui::audio_out *out = nullptr;      // set once the audio device is open

private:
	ui::bridge   &br;
	ui::midi_in  &midi, &midi_b;
	ui::midi_out &mout, &mout_b;
	bool m_pressed = false;
};


// ---- Write just the picture, with no window. Used to check the looks

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
	bi.bmiHeader.biHeight = -h;                 // top down
	bi.bmiHeader.biPlanes = 1;
	bi.bmiHeader.biBitCount = 32;
	bi.bmiHeader.biCompression = BI_RGB;

	void *bits = nullptr;
	HDC dc = CreateCompatibleDC(nullptr);
	HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
	if (!bmp) {
		std::fprintf(stderr, "画面を作れない\n");
		return 1;
	}
	SelectObject(dc, bmp);

	ui::snapshot s;
	br.read(s);
	p.set_volume(0.8);
	p.paint(dc, s, 0, "");
	GdiFlush();

	const bool ok = ui::write_png(path, static_cast<const u8 *>(bits), w, h, w * 4);

	DeleteObject(bmp);
	DeleteDC(dc);

	std::printf(ok ? "書き出した: %s（%d×%d）\n" : "書き出せない: %s\n",
	            path.c_str(), w, h);
	return ok ? 0 : 1;
}

} // namespace


int main(int argc, char **argv)
{
	smu2000::init_console_utf8();

	std::string dir, shot_path, dump_layout, play_path;
	std::string layout_path;
	int midi_dev = -2;                 // -2 unset (use the remembered one) / -1 unused
	int midib_dev = -2;
	int mout_dev = -2;
	int moutb_dev = -2;
	int latency = 30;
	int win_w = 1400, win_h = 360;
	bool grid = false;
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
			return 0;
		}
		else if (!std::strcmp(argv[i], "--midi") && i + 1 < argc) midi_dev = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--midi-b") && i + 1 < argc) midib_dev = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--midiout") && i + 1 < argc) mout_dev = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--midiout-b") && i + 1 < argc) moutb_dev = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--nomidi")) { midi_dev = -1; midib_dev = -1; }
		else if (!std::strcmp(argv[i], "--latency") && i + 1 < argc) latency = std::atoi(argv[++i]);
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

	// Without --layout, look through the usual places in order
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

	// Picture only. An empty screen can be drawn even without any ROMs.
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
			" [--latency ミリ秒] [--layout panel.txt] [--play 曲.mid]\n"
			"        gui --dump-layout panel.txt   いまの配置を書き出す\n"
			"        gui --list\n"
			"        gui [<rom ディレクトリ> --boot] --shot 絵.png [--size 1400x440]\n");
		return 1;
	}

	static ui::engine eng(br, midi);
	eng.midi_b = &midi_b;
	eng.mout_b = &mout_b;
	eng.mout = &mout;
	if (!eng.load(dir)) {
		std::fprintf(stderr, "%s\n", eng.message.c_str());
		return 1;
	}

	// Picture only, but taken after boot so the LCD has something on it
	if (!shot_path.empty()) {
		if (!eng.boot()) { std::fprintf(stderr, "%s\n", eng.message.c_str()); return 1; }
		eng.state.store(1);

		// The display is still settling right after boot. Idle a little to calm it.
		{
			s32 l, r;
			for (size_t i = 0; i < size_t(2.0 * RATE); i++)
				eng.mu.run_sample(l, r);
		}

		// The level meters need signal, so stream MIDI first when one was given
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

	// ---- Put the window up

	static app gui(br, midi, midi_b, mout, mout_b);
	gui.panel.resize(win_w, win_h);
	gui.set_layout(layout_path);
	gui.panel.resize(win_w, win_h);
	br.set_gain(1.0f);
	eng.publish();

	// Look up the previously chosen ports by name. --midi / --midiout win.
	//
	// Opening the ports here rather than on the boot thread keeps the names
	// settled before the window starts reading them for the status line
	{
		const port_names want = load_settings();
		if (midi_dev == -2)  midi_dev  = find_device(ui::midi_in::list(), want.in);
		if (midib_dev == -2) midib_dev = find_device(ui::midi_in::list(), want.in_b);
		if (mout_dev == -2)  mout_dev  = find_device(ui::midi_out::list(), want.out);
		if (moutb_dev == -2) moutb_dev = find_device(ui::midi_out::list(), want.out_b);

		gui.choose_in(midi_dev);
		gui.choose_in_b(midib_dev);
		gui.choose_out(mout_dev);
		gui.choose_out_b(moutb_dev);
		std::printf("MIDI IN A: %s\n",  gui.in_name.empty()  ? "なし" : gui.in_name.c_str());
		std::printf("MIDI IN B: %s\n",  gui.in_name_b.empty() ? "なし" : gui.in_name_b.c_str());
		std::printf("MIDI OUT A: %s\n", gui.out_name.empty()   ? "なし" : gui.out_name.c_str());
		std::printf("MIDI OUT B: %s\n", gui.out_name_b.empty() ? "なし" : gui.out_name_b.c_str());
		std::fflush(stdout);
	}

	// Boot on a separate thread, and start the audio once it is done
	static ui::audio_out out;
	gui.out = &out;
	std::thread boot_thread([&] {
		if (!eng.boot()) {
			eng.state.store(2);
			eng.publish();
			return;
		}
		eng.state.store(1);
		eng.publish();

		std::string err;
		if (!out.start(latency, [](s16 *o, u32 n) { eng.fill(o, n); }, err)) {
			std::fprintf(stderr, "音声: %s\n", err.c_str());
			eng.message = "音声デバイスを開けない";
			eng.state.store(2);
			eng.publish();
			return;
		}
		// With --play, start streaming as soon as it begins to sound
		if (!play_path.empty())
			gui.play_song(play_path);
		std::printf("鳴らしている（待ち時間 %.1f ms、%s）\n",
		            1000.0 * out.buffer_frames() / RATE,
		            out.mmcss() ? "CoreAudio の実時間スレッド"
		                        : "実時間スレッドを取れていない（途切れやすい）");
		std::fflush(stdout);
	});

	ui::run_window(gui, "S-MU2000", win_w, win_h);

	out.stop();
	if (boot_thread.joinable())
		boot_thread.join();
	gui.play.stop();
	midi.close();
	midi_b.close();
	mout.close();
	mout_b.close();

	if (out.produced())
		std::printf("CPU %.1f%%、1 回の最悪 %.2f ms、枯渇 %llu 回\n",
		            out.cpu_percent(), out.worst_ms(),
		            (unsigned long long)out.starved());
	return 0;
}
