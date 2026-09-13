// license:BSD-3-Clause

#include "xg_ui.h"

#include "imgui.h"

#include <cstdio>
#include <cstring>

#include <windows.h>

namespace ui {
namespace xgui {

namespace {

// General MIDI の楽器名（規格の名前）。XG のバンク 0 はこの並び
const char *const GM_NAMES[128] = {
	"Acoustic Grand Piano", "Bright Acoustic Piano", "Electric Grand Piano", "Honky-tonk Piano",
	"Electric Piano 1", "Electric Piano 2", "Harpsichord", "Clavi",
	"Celesta", "Glockenspiel", "Music Box", "Vibraphone", "Marimba", "Xylophone", "Tubular Bells", "Dulcimer",
	"Drawbar Organ", "Percussive Organ", "Rock Organ", "Church Organ", "Reed Organ", "Accordion", "Harmonica", "Tango Accordion",
	"Acoustic Guitar (nylon)", "Acoustic Guitar (steel)", "Electric Guitar (jazz)", "Electric Guitar (clean)",
	"Electric Guitar (muted)", "Overdriven Guitar", "Distortion Guitar", "Guitar Harmonics",
	"Acoustic Bass", "Electric Bass (finger)", "Electric Bass (pick)", "Fretless Bass",
	"Slap Bass 1", "Slap Bass 2", "Synth Bass 1", "Synth Bass 2",
	"Violin", "Viola", "Cello", "Contrabass", "Tremolo Strings", "Pizzicato Strings", "Orchestral Harp", "Timpani",
	"String Ensemble 1", "String Ensemble 2", "Synth Strings 1", "Synth Strings 2",
	"Choir Aahs", "Voice Oohs", "Synth Voice", "Orchestra Hit",
	"Trumpet", "Trombone", "Tuba", "Muted Trumpet", "French Horn", "Brass Section", "Synth Brass 1", "Synth Brass 2",
	"Soprano Sax", "Alto Sax", "Tenor Sax", "Baritone Sax", "Oboe", "English Horn", "Bassoon", "Clarinet",
	"Piccolo", "Flute", "Recorder", "Pan Flute", "Blown Bottle", "Shakuhachi", "Whistle", "Ocarina",
	"Lead 1 (square)", "Lead 2 (sawtooth)", "Lead 3 (calliope)", "Lead 4 (chiff)",
	"Lead 5 (charang)", "Lead 6 (voice)", "Lead 7 (fifths)", "Lead 8 (bass + lead)",
	"Pad 1 (new age)", "Pad 2 (warm)", "Pad 3 (polysynth)", "Pad 4 (choir)",
	"Pad 5 (bowed)", "Pad 6 (metallic)", "Pad 7 (halo)", "Pad 8 (sweep)",
	"FX 1 (rain)", "FX 2 (soundtrack)", "FX 3 (crystal)", "FX 4 (atmosphere)",
	"FX 5 (brightness)", "FX 6 (goblins)", "FX 7 (echoes)", "FX 8 (sci-fi)",
	"Sitar", "Banjo", "Shamisen", "Koto", "Kalimba", "Bag pipe", "Fiddle", "Shanai",
	"Tinkle Bell", "Agogo", "Steel Drums", "Woodblock", "Taiko Drum", "Melodic Tom", "Synth Drum", "Reverse Cymbal",
	"Guitar Fret Noise", "Breath Noise", "Seashore", "Bird Tweet", "Telephone Ring", "Helicopter", "Applause", "Gunshot",
};

const char *const GM_GROUPS[16] = {
	"Piano", "Chromatic Percussion", "Organ", "Guitar", "Bass", "Strings", "Ensemble", "Brass",
	"Reed", "Pipe", "Synth Lead", "Synth Pad", "Synth Effects", "Ethnic", "Percussive", "Sound Effects",
};

} // namespace


namespace {
std::unique_ptr<xg::voice_rom> g_voices;
}

void set_voice_rom(std::shared_ptr<const std::vector<u8>> rom)
{
	g_voices = std::make_unique<xg::voice_rom>(std::move(rom));
	if (!g_voices->ok())
		g_voices.reset();                  // 版が違う。GM の名前で出す
}

const xg::voice_rom *voices() { return g_voices.get(); }

const xg::param &P(const char *key)
{
	const xg::param *p = xg::find(key);
	IM_ASSERT(p);
	return *p;
}

std::string part_name(int part)
{
	char buf[8];
	std::snprintf(buf, sizeof(buf), "%c%d", part < 16 ? 'A' : 'B', part % 16 + 1);
	return buf;
}

// 受信チャンネルは 0-31 が A1-A16・B1-B16、127 が OFF
std::string channel_name(int v)
{
	if (v == 127)
		return "OFF";
	if (v < 0 || v > 31)
		return std::to_string(v);
	return part_name(v);
}

const char *gm_name(int program)
{
	return GM_NAMES[program & 0x7f];
}

std::string voice_text(int msb, int lsb, int prog)
{
	char buf[80];
	if (msb == 127)
		std::snprintf(buf, sizeof(buf), "%3d  Drum Kit", prog + 1);
	else if (msb == 0 && lsb == 0)
		std::snprintf(buf, sizeof(buf), "%3d  %s", prog + 1, gm_name(prog));
	else
		std::snprintf(buf, sizeof(buf), "%3d  %s（%d/%d）", prog + 1, gm_name(prog), msb, lsb);
	return buf;
}


void program_menu(int part, xg::model &m, bridge &br)
{
	int msb = -1, prog = -1;
	m.get(P("part.bank_msb"), part, msb);
	m.get(P("part.program"), part, prog);

	ImGui::TextDisabled("パート %s", part_name(part).c_str());
	ImGui::Separator();

	if (ImGui::BeginMenu("プログラム")) {
		for (int g = 0; g < 16; g++) {
			const bool here = prog >= 0 && prog / 8 == g;
			if (ImGui::BeginMenu(GM_GROUPS[g], true)) {
				for (int i = g * 8; i < g * 8 + 8; i++) {
					char label[64];
					std::snprintf(label, sizeof(label), "%3d  %s", i + 1, GM_NAMES[i]);
					if (ImGui::MenuItem(label, nullptr, i == prog))
						br.send(m.set(P("part.program"), part, i));
				}
				ImGui::EndMenu();
			}
			if (here) {                           // いまの組に印
				ImGui::SameLine();
				ImGui::TextDisabled("●");
			}
		}
		ImGui::EndMenu();
	}

	if (ImGui::BeginMenu("バンク MSB")) {
		static const struct { int msb; const char *name; } BANKS[] = {
			{ 0, "0  Normal" }, { 64, "64  SFX" }, { 126, "126  SFX Kit" }, { 127, "127  Drum Kit" },
		};
		for (const auto &b : BANKS)
			if (ImGui::MenuItem(b.name, nullptr, b.msb == msb))
				br.send(m.set(P("part.bank_msb"), part, b.msb));   // 今のプログラムも続けて送る
		ImGui::EndMenu();
	}

	ImGui::Separator();
	ImGui::TextDisabled("バンクとプログラムの細かい値はパートの面で");
}



// ---- 説明（ヘルプ）と言語
//
// 文は「キー → 言語ごとの文」の表で持つ。言語を足すときは lang に 1 つ足し、
// HELP の各行に文を 1 つ足す（足りない言語は日本語で出る）。

namespace {

// 言語の並び。editor.ini には code で残す
struct language { const char *code; const char *name; };
const language LANGS[] = {
	{ "ja", "日本語" },
	{ "en", "English" },
};
constexpr int NLANG = int(sizeof(LANGS) / sizeof(LANGS[0]));

struct help_text { const char *name; const char *text[NLANG]; };

// 見出し（一覧の列）とパラメータのキー。初めて触る人に向けて、何が変わるかを書く
const help_text HELP[] = {
	{ "パート（右クリックで音色）", {
		"MU2000 は 32 のパートを同時に鳴らせる。A1-A16 は MIDI IN A の 1-16ch、\n"
		"B1-B16 は MIDI IN B の 1-16ch で受ける（受信チャンネルは変えられる）。\n"
		"右クリックで音色（プログラムとバンク）を選ぶ",
		"The MU2000 plays 32 parts at once. A1-A16 receive MIDI IN A channels 1-16,\n"
		"B1-B16 receive MIDI IN B channels 1-16 (the receive channel can be changed).\n"
		"Right-click to choose the voice (program and bank)." } },
	{ "MASTER", {
		"全体に効く値。VOL はマスターボリューム、REV・CHO・VAR はそれぞれのエフェクトの\n"
		"戻り量（エフェクトを通った音を、どれだけ全体に戻すか）",
		"Values for the whole mix. VOL is the master volume; REV, CHO and VAR are the\n"
		"effect return levels (how much of each effect's output is mixed back in)." } },
	{ "MASTER.INS", {
		"システムのエフェクトの種別。R がリバーブ、C がコーラス、V がバリエーション。\n"
		"バリエーションの接続が INSERTION のときは、1 つのパートにだけ掛かるので薄く出す",
		"System effect types: R reverb, C chorus, V variation.\n"
		"The variation is dimmed when it is connected as INSERTION (it then applies to one part only)." } },
	{ "INS", {
		"このパートだけに掛かっているエフェクト。\n"
		"1-4 はインサーションエフェクト、V は接続が INSERTION のバリエーション。\n"
		"歪みやワウ、アンプシミュレータなど、1 つの楽器にだけ掛けたいものに使う",
		"Effects applied to this part only.\n"
		"1-4 are insertion effects; V is the variation effect when connected as INSERTION.\n"
		"Used for things you want on a single instrument, such as distortion, wah or amp simulation." } },
	{ "VEL", {
		"鍵盤を弾いた強さ（ベロシティ）。音が鳴るたびに跳ねて、落ちていく",
		"How hard the key was played (velocity). Jumps on each note and falls back." } },
	{ "VOL", {
		"パートの音量（CC7 / Volume）。曲の中のパートどうしの大きさの釣り合いを取る",
		"Part volume (CC7). Balances the loudness of the parts against each other." } },
	{ "EXP", {
		"エクスプレッション（CC11）。音量をさらに絞る。VOL と掛け算で効き、\n"
		"曲の中で抑揚（だんだん大きく・小さく）をつけるのに使われる。表示だけ",
		"Expression (CC11). Scales the volume further, multiplied with VOL.\n"
		"Songs use it for swells and fades. Display only." } },
	{ "PAN", {
		"左右の位置（CC10 / Pan）。C が真ん中、L は左、R は右。Rnd は弾くたびにばらばら",
		"Stereo position (CC10). C is centre, L left, R right. Rnd moves on every note." } },
	{ "P.BEND", {
		"ピッチベンド。音程を滑らかに上げ下げする。0 が元の音程。表示だけ",
		"Pitch bend. Slides the pitch up or down; 0 is the original pitch. Display only." } },
	{ "MOD", {
		"モジュレーション（CC1）。ビブラートなど、音の揺れの深さ。表示だけ",
		"Modulation (CC1). Depth of vibrato and similar wobble. Display only." } },
	{ "HOLD", {
		"ダンパーペダル（CC64）。ON の間は、鍵盤を離しても音が伸びる。表示だけ",
		"Damper pedal (CC64). While ON, notes keep sounding after the keys are released. Display only." } },
	{ "CUT", {
		"フィルタのカットオフ（CC74 / Brightness）。音の明るさ。\n"
		"＋で明るく（高い音が出る）、−でこもった音になる",
		"Filter cutoff (CC74, brightness).\n"
		"+ makes the sound brighter, - makes it duller." } },
	{ "RESO", {
		"フィルタのレゾナンス（CC71 / Harmonic Content）。カットオフのあたりを強調して、\n"
		"クセのある音にする",
		"Filter resonance (CC71, harmonic content).\n"
		"Emphasises the area around the cutoff for a more peaky sound." } },
	{ "REV", {
		"リバーブへの送り量（CC91）。部屋やホールの響き（残響）をどれだけ足すか",
		"Reverb send (CC91). How much room or hall ambience is added." } },
	{ "CHO", {
		"コーラスへの送り量（CC93）。音をわずかに揺らして、厚みや広がりを足す",
		"Chorus send (CC93). Adds thickness and width by gently detuning the sound." } },
	{ "VAR", {
		"バリエーションエフェクトへの送り量（CC94）。\n"
		"バリエーションの接続が SYSTEM のときだけ効く（リバーブやコーラスと同じく、\n"
		"全パートで 1 台を共有し、各パートが送る量を決める）。\n"
		"接続が INSERTION のときは 1 つのパートにだけ掛かり、この値は使われない",
		"Variation effect send (CC94).\n"
		"Only used when the variation is connected as SYSTEM (like reverb and chorus,\n"
		"one shared effect that every part sends to).\n"
		"When connected as INSERTION it applies to a single part and this value is ignored." } },

	{ "part.volume", { "パートの音量（CC7）", "Part volume (CC7)." } },
	{ "part.pan", { "左右の位置（CC10）。C が真ん中", "Stereo position (CC10). C is centre." } },
	{ "part.dry_level", {
		"エフェクトを通さない元の音の量。下げると、エフェクトの音だけが残る",
		"Level of the unprocessed sound. Lower it to hear only the effects." } },
	{ "part.reverb_send", { "リバーブへの送り量（CC91）。響きの量", "Reverb send (CC91)." } },
	{ "part.chorus_send", { "コーラスへの送り量（CC93）。広がりと揺れ", "Chorus send (CC93)." } },
	{ "part.variation_send", {
		"バリエーションへの送り量（CC94）。接続が SYSTEM のときだけ効く",
		"Variation send (CC94). Only used when the variation is connected as SYSTEM." } },
	{ "part.cutoff", { "フィルタのカットオフ（CC74）。音の明るさ", "Filter cutoff (CC74). Brightness." } },
	{ "part.resonance", {
		"フィルタのレゾナンス（CC71）。カットオフのあたりを強調する",
		"Filter resonance (CC71). Emphasises the area around the cutoff." } },
	{ "part.attack", {
		"アタック（CC73）。鍵盤を押してから音が立ち上がるまでの速さ。−で速く、＋でゆっくり",
		"Attack (CC73). How fast the sound rises after a key is pressed. - is faster, + is slower." } },
	{ "part.decay", {
		"ディケイ（CC75）。立ち上がったあと、伸ばしている音の大きさへ落ち着くまでの速さ",
		"Decay (CC75). How fast the sound settles after the attack." } },
	{ "part.release", {
		"リリース（CC72）。鍵盤を離してから音が消えるまでの長さ",
		"Release (CC72). How long the sound takes to fade after the key is released." } },
	{ "part.vib_rate", { "ビブラートの速さ", "Vibrato speed." } },
	{ "part.vib_depth", { "ビブラートの深さ", "Vibrato depth." } },
	{ "part.vib_delay", { "弾いてからビブラートが掛かり始めるまでの時間", "Time before the vibrato starts." } },
	{ "part.note_shift", { "音程を半音単位でずらす（移調）", "Transposes the part in semitones." } },
	{ "part.detune", { "音程をわずかにずらす（音の厚みを出すときなど）", "Fine pitch offset, e.g. to thicken the sound." } },
	{ "part.rcv_channel", {
		"このパートが受ける MIDI チャンネル。A1-A16 は IN A、B1-B16 は IN B",
		"MIDI channel this part receives. A1-A16 are IN A, B1-B16 are IN B." } },
	{ "part.mono_poly", {
		"POLY は和音が鳴る。MONO は 1 音ずつ（前の音を切って次の音）",
		"POLY plays chords. MONO plays one note at a time." } },
	{ "part.mode", {
		"NORMAL は普通の楽器。DRUM 系はドラムセットとして鳴らす",
		"NORMAL is a regular instrument. The DRUM modes play a drum kit." } },
	{ "part.element_reserve", {
		"このパートのために取っておく同時発音数。音が途切れるパートで増やす",
		"Voices reserved for this part. Raise it if notes on this part get cut off." } },
	{ "part.program", { "音色の番号（プログラムチェンジ）", "Voice number (program change)." } },
	{ "part.bank_msb", {
		"音色の組（バンク）の上の桁。0 が普通、64 が効果音、127 がドラム",
		"Bank select MSB. 0 is normal, 64 sound effects, 127 drum kits." } },
	{ "part.bank_lsb", {
		"音色の組（バンク）の下の桁。同じ番号の音色の別版を選ぶ",
		"Bank select LSB. Picks variations of the same voice number." } },
};

bool g_help = true;
int  g_lang = 0;
bool g_loaded = false;

std::string settings_file()
{
	char buf[1024];
	const DWORD n = GetEnvironmentVariableA("LOCALAPPDATA", buf, sizeof(buf));
	if (n == 0 || n >= sizeof(buf))
		return {};
	return std::string(buf) + "\\S-MU2000\\editor.ini";
}

void load_settings()
{
	g_loaded = true;
	const std::string path = settings_file();
	FILE *f = path.empty() ? nullptr : std::fopen(path.c_str(), "rb");
	if (!f)
		return;
	char line[256];
	while (std::fgets(line, sizeof(line), f)) {
		line[std::strcspn(line, "\r\n")] = 0;
		if (!std::strncmp(line, "help=", 5))
			g_help = line[5] != '0';
		else if (!std::strncmp(line, "lang=", 5))
			for (int i = 0; i < NLANG; i++)
				if (!std::strcmp(line + 5, LANGS[i].code))
					g_lang = i;
	}
	std::fclose(f);
}

void save_settings()
{
	const std::string path = settings_file();
	if (path.empty())
		return;
	CreateDirectoryA(path.substr(0, path.rfind('\\')).c_str(), nullptr);
	if (FILE *f = std::fopen(path.c_str(), "wb")) {
		std::fprintf(f, "help=%d\nlang=%s\n", g_help ? 1 : 0, LANGS[g_lang].code);
		std::fclose(f);
	}
}

void ensure_loaded()
{
	if (!g_loaded)
		load_settings();
}

const char *find_help(const char *name)
{
	for (const help_text &h : HELP)
		if (!std::strcmp(h.name, name))
			return h.text[g_lang] ? h.text[g_lang] : h.text[0];
	return nullptr;
}

} // namespace

bool &help_on()
{
	ensure_loaded();
	return g_help;
}

void help_tip(const char *name)
{
	if (!help_on() || !ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
		return;
	if (const char *t = find_help(name))
		ImGui::SetTooltip("%s", t);
}

void help_checkbox()
{
	ensure_loaded();
	if (ImGui::Checkbox(g_lang == 0 ? "説明を出す" : "Show help", &g_help))
		save_settings();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
		ImGui::SetTooltip(g_lang == 0 ? "見出しや名前にカーソルを当てたとき、何に効くのかを出す"
		                              : "Explain what each heading or name does when you hover over it");
	ImGui::SameLine();
	ImGui::SetNextItemWidth(ImGui::GetFontSize() * 6);
	if (ImGui::BeginCombo("##lang", LANGS[g_lang].name)) {
		for (int i = 0; i < NLANG; i++)
			if (ImGui::Selectable(LANGS[i].name, i == g_lang)) {
				g_lang = i;
				save_settings();
			}
		ImGui::EndCombo();
	}
}

void headers_with_help(int columns)
{
	ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
	for (int c = 0; c < columns; c++) {
		if (!ImGui::TableSetColumnIndex(c))
			continue;
		const char *name = ImGui::TableGetColumnName(c);
		ImGui::TableHeader(name);
		help_tip(name);
	}
}

} // namespace xgui
} // namespace ui
