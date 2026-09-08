// license:BSD-3-Clause
//
// フロントパネルを文字だけで動かしてみる道具。絵を描く前の確認用。
//
//   panel <rom ディレクトリ>                起動して LCD を出す
//   panel <rom ディレクトリ> --keys "..."   ボタンを順に押す
//   panel <rom ディレクトリ> --list         ボタンの名前を並べる
//
// --keys には短い名前をカンマで並べる。例:
//   panel roms --keys "play,part+,part+,edit"
//
// LCD は実機と同じ HD44780。字の絵は roms の hd44780u_b04.bin から引く。
// firmware は 2 行 40 桁で使うが、実機の窓に出ているのは **2 行 24 桁**。
// 24 桁より先には何も書かれないことを起動画面で確かめた。

#include "mu2000.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr u32 RATE = 44100;

// ボタンの短い名前。--keys で使う
struct alias { const char *key; mu2000::button b; };
const alias ALIASES[] = {
	{ "play",     mu2000::button::play },
	{ "edit",     mu2000::button::edit },
	{ "util",     mu2000::button::util },
	{ "effect",   mu2000::button::effect },
	{ "mute",     mu2000::button::mute_solo },
	{ "part+",    mu2000::button::part_plus },
	{ "part-",    mu2000::button::part_minus },
	{ "value+",   mu2000::button::value_plus },
	{ "value-",   mu2000::button::value_minus },
	{ "exit",     mu2000::button::exit },
	{ "enter",    mu2000::button::enter },
	{ "left",     mu2000::button::select_left },
	{ "right",    mu2000::button::select_right },
	{ "seq",      mu2000::button::seq },
	{ "select",   mu2000::button::select },
	{ "audition", mu2000::button::audition },
	{ "mode",     mu2000::button::sampling_mode },
	{ "piano",    mu2000::button::piano },
	{ "drum",     mu2000::button::drum },
};

// n サンプルぶん空回し
void idle(mu2000 &mu, double seconds)
{
	const size_t n = size_t(seconds * RATE);
	s32 l, r;
	for (size_t i = 0; i < n; i++)
		mu.run_sample(l, r);
}

// ボタンを押して離す。実機の指の速さに合わせて 80ms 押す
void tap(mu2000 &mu, mu2000::button b)
{
	mu.set_button(b, true);
	idle(mu, 0.08);
	mu.set_button(b, false);
	idle(mu, 0.12);
}

void show_lcd(mu2000 &mu)
{
	hd44780_device &lcd = mu.lcd();
	const u8 *img = lcd.render();
	const int lines = lcd.lines(), cols = lcd.line_size(), rows = lcd.char_size();

	std::printf("LCD %d 行 × %d 桁（表示 %s）\n", lines, cols,
	            lcd.display_on() ? "オン" : "オフ");

	// 1. 文字として
	const u8 *dd = lcd.ddram();
	for (int line = 0; line < lines; line++) {
		std::printf("  |");
		for (int pos = 0; pos < cols; pos++) {
			const u8 c = dd[line * 0x40 + pos];
			std::putchar((c >= 0x20 && c < 0x7f) ? char(c) : (c ? '?' : ' '));
		}
		std::printf("|\n");
	}

	// 2. 点として。字の絵が引けているかはこちらで分かる
	std::printf("\n");
	for (int line = 0; line < lines; line++) {
		for (int y = 0; y < rows; y++) {
			std::printf("  ");
			for (int pos = 0; pos < cols; pos++) {
				const u8 v = img[16 * (line * cols + pos) + y];
				for (int x = 4; x >= 0; x--)
					std::printf("%s", BIT(v, x) ? "##" : "  ");
				std::printf(" ");
			}
			std::printf("\n");
		}
		std::printf("\n");
	}

	const u16 led = mu.leds();
	std::printf("LED:");
	for (int i = 0; i < 10; i++)
		std::printf(" %d", BIT(led, i));
	std::printf("\n");
}

} // namespace


int main(int argc, char **argv)
{
	std::string dir, keys;
	bool list = false;
	int turn = 0;
	double hold = 0.0;
	std::string holdkey;
	double settle = 1.0;

	for (int i = 1; i < argc; i++) {
		if (!std::strcmp(argv[i], "--keys") && i + 1 < argc) keys = argv[++i];
		else if (!std::strcmp(argv[i], "--list")) list = true;
		else if (!std::strcmp(argv[i], "--turn") && i + 1 < argc) turn = std::atoi(argv[++i]);
		else if (!std::strcmp(argv[i], "--hold") && i + 2 < argc) { holdkey = argv[++i]; hold = std::atof(argv[++i]); }
		else if (!std::strcmp(argv[i], "--settle") && i + 1 < argc) settle = std::atof(argv[++i]);
		else if (dir.empty()) dir = argv[i];
	}

	if (list) {
		std::printf("ボタン（左が --keys で使う名前）:\n");
		for (int i = 0; i < int(mu2000::button::count); i++) {
			const mu2000::button b = mu2000::button(i);
			const char *key = "";
			for (const alias &a : ALIASES)
				if (a.b == b) { key = a.key; break; }
			std::printf("  %-10s %s\n", key, mu2000::button_name(b));
		}
		return 0;
	}

	if (dir.empty()) {
		std::fprintf(stderr, "使い方: panel <rom ディレクトリ> [--keys \"play,edit\"]\n");
		return 1;
	}

	mu2000 mu;
	if (!mu.load_program(dir + "/mu2000_flash.bin")) {
		std::fprintf(stderr, "%s\n", mu.error().c_str()); return 1;
	}
	if (!mu.load_wave(dir + "/dump")) {
		std::fprintf(stderr, "%s\n", mu.error().c_str()); return 1;
	}
	if (!mu.load_sintab(dir + "/standin/sin-table.bin"))
		std::fprintf(stderr, "警告: %s\n", mu.error().c_str());
	// 字の絵。roms の下か standin の下を見る
	if (!mu.load_lcd_font(dir + "/hd44780u_b04.bin") &&
	    !mu.load_lcd_font(dir + "/standin/hd44780u_b04.bin"))
		std::fprintf(stderr, "警告: %s\n", mu.error().c_str());

	mu.set_threaded(true);
	mu.reset();

	std::printf("起動中...");
	std::fflush(stdout);
	{
		const size_t limit = size_t(30.0 * RATE);
		size_t i = 0;
		s32 l, r;
		for (; i < limit && !mu.midi_ready(); i++)
			mu.run_sample(l, r);
		std::printf(" %.2f 秒\n", double(i) / RATE);
	}
	// 起動直後は表示が動いている途中なので、少し落ち着かせる
	idle(mu, settle);

	if (!keys.empty()) {
		size_t at = 0;
		while (at <= keys.size()) {
			const size_t comma = keys.find(',', at);
			std::string k = keys.substr(at, comma == std::string::npos
			                                    ? std::string::npos : comma - at);
			at = (comma == std::string::npos) ? keys.size() + 1 : comma + 1;
			while (!k.empty() && k.front() == ' ') k.erase(0, 1);
			while (!k.empty() && k.back() == ' ') k.pop_back();
			if (k.empty())
				continue;

			bool found = false;
			for (const alias &a : ALIASES)
				if (k == a.key) {
					std::printf("\n--- %s ---\n", mu2000::button_name(a.b));
					tap(mu, a.b);
					idle(mu, 0.3);
					found = true;
					break;
				}
			if (!found)
				std::fprintf(stderr, "知らないボタン: %s\n", k.c_str());
		}
	}

	if (!holdkey.empty()) {
		for (const alias &a : ALIASES)
			if (holdkey == a.key) {
				std::printf("\n--- %s を %.1f 秒押しっぱなし ---\n",
				            mu2000::button_name(a.b), hold);
				mu.set_button(a.b, true);
				idle(mu, hold);
				mu.set_button(a.b, false);
				idle(mu, 0.3);
				break;
			}
	}

	if (turn) {
		std::printf("\n--- ダイヤルを %+d 目盛り ---\n", turn);
		mu.turn_encoder(turn);
		// 位相を送り切るまで回す
		for (int i = 0; i < 400 && mu.encoder_busy(); i++)
			idle(mu, 0.01);
		idle(mu, 0.3);
	}

	std::printf("\n");
	show_lcd(mu);
	return 0;
}
