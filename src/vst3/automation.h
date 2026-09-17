// license:BSD-3-Clause
//
// DAW のオートメーションに見せる XG の値（doc/automation.md）。VST3 と CLAP が同じ表を使う。
//
// 64 パートそれぞれの音量・パン・送り・フィルタ・EG・ビブラート・EQ などと、マスター
// （マスターボリューム・チューン・移調、リバーブ・コーラス・バリエーションの戻り、マスター EQ）。
//
// **番号は保存した曲が覚えているので、一度決めたら動かさない。** 足すときは空いている所に。
//   パート pp（0-63）の k 番目   65536 + pp × 32 + k    （k は 0-31。PART_KEYS の並び）
//   マスターの k 番目            67584 + k              （MASTER_KEYS の並び）
// VST3 の今ある番号（MIDI の CC 0-32767 ほか、Output 4096、Status 4097）とは重ならない。
//
// 値は XG の値そのもの（定義表の min〜max の整数）。VST3 はそれを 0-1 に畳んで渡す。
//
// 音源へは、なるべくコントロールチェンジで入れる（SysEx だと LCD に Ex の印が出るので）。
// firmware で確かめた、XG の値と 1 対 1 で同じになる CC:
//   音量 7、パン 10（1-127。0 のランダムは CC では作れない）、リバーブ 91、コーラス 93、
//   バリエーション 94（接続が SYSTEM のときだけ効く）、カットオフ 74、レゾナンス 71、
//   アタック 73、ディケイ 75、リリース 72、ビブラートの速さ 76・深さ 77・遅れ 78
// CC で入れるのは、パートの受信チャンネルがそのパートだけのものなとき（CC はチャンネルの全パートに効く）。
// それ以外（EQ、ドライ、ノートシフト、マスター、チャンネルが共有・OFF）はパラメータチェンジ。

#ifndef S_MU2000_VST3_AUTOMATION_H
#define S_MU2000_VST3_AUTOMATION_H

#pragma once

#include "ui/snapshot.h"
#include "xg/model.h"

#include <cstdint>
#include <string>
#include <vector>

namespace smu2000 {
namespace automation {

constexpr uint32_t PART_BASE   = 65536;
constexpr uint32_t PART_STRIDE = 32;
constexpr uint32_t MASTER_BASE = PART_BASE + 64 * PART_STRIDE;   // 67584

struct entry {
	uint32_t id;
	const xg::param *p;
	int part;               // マスターは 0（定義表の番地のパート番号として渡す）
	bool is_part;
	int cc;                 // 同じ値になる CC（無ければ -1）
	std::string name;       // "A1 Volume" / "Master Rev Return"
	std::string group;      // "Part A1" / "Master"
};

// 表。最初に呼んだときに作る（本の糸で先に 1 回呼んでおくこと。音声の糸で初めて作らせない）
const std::vector<entry> &entries();
// 番号から。無ければ -1
int index_of(uint32_t id);
// 定義表の値とパートから（画面で触った値を番号に直す）。無ければ -1
int index_of(const xg::param &p, int part);

// 値の変換
inline int  steps(const entry &e) { return e.p->max - e.p->min; }
inline double to_normalized(const entry &e, int value)
{
	const int n = steps(e);
	return n > 0 ? double(value - e.p->min) / double(n) : 0.0;
}
int to_value(const entry &e, double normalized);
int clamp_value(const entry &e, double plain);
// 画面に出す文字（"80 Hz"、"+3 dB"、"L12" など）
std::string text(const entry &e, int value);
// 打った文字から値へ。読めなければ false
bool parse(const entry &e, const char *text, int &value);

// 写しから今の値を読む。読めなければ false
bool current(const entry &e, const ui::xg_snapshot &ram, int &value);

// 音源へ入れる MIDI を作る。out は 16 バイト以上。長さを返し、port に流す口を入れる（0-3）。
// ram が無ければ、いつもパラメータチェンジにする
int midi(const entry &e, int value, const ui::xg_snapshot *ram, uint8_t *out, int &port);

} // namespace automation
} // namespace smu2000

#endif // S_MU2000_VST3_AUTOMATION_H
