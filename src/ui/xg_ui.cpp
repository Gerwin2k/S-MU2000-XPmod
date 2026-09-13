// license:BSD-3-Clause

#include "xg_ui.h"

#include "imgui.h"

#include <cstdio>

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


} // namespace xgui
} // namespace ui
