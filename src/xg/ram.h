// license:BSD-3-Clause
//
// XG の値が firmware のワーク RAM（0x400000-0x43ffff）のどこにあるか。
//
// 画面は MU2000 に問い合わせずに、ここを読んで値を出す（doc/params.md の「RAM から読む」）。
// 問い合わせ（ダンプ要求）は MIDI IN に入るので、LCD の受信マークが点きっぱなしになる。
// RAM なら何も流さずに、曲・SysEx・パネル操作のどれで変わった値も見える。
//
// 番地は、1 項目ずつパラメータチェンジで書いて RAM の変わった所を探して決めた。
// 塊の中の並びは XG の番地そのまま。xgtest.exe が全項目を一括ダンプと突き合わせる。

#ifndef S_MU2000_XG_RAM_H
#define S_MU2000_XG_RAM_H

#pragma once

#include "xg/model.h"

namespace xg {
namespace ram {

// ワーク RAM の先頭（0x400000）からの位置
constexpr u32 SYSTEM   = 0x226c1;   // 00 00 00-06
constexpr u32 EFFECT   = 0x0cad8;   // 02 01 00 から。下の EFFECTS の並び
constexpr u32 EFFECT_SIZE = 0xfe;   // 02 01 00 からインサーション 2 の終わりまで
// パートの塊は PART_STRIDE ずつ並ぶが、**並びは XG のパート番号の順ではない**。
// 口ごとに「10 番目のパート（ch10）が先頭、残りが 1-9, 11-16」の固定の順（ドラムかどうかに
// よらない。パートを DRUM にしても並びは変わらなかった）
constexpr u32 PARTS    = 0x28d64;   // 並びの先頭（パート 10 の塊）
constexpr u32 PART_STRIDE = 0x134;

// XG のパート番号（0-31）から、塊の先頭
constexpr u32 part_base(int part)
{
	const int port = part / 16, k = part % 16;
	const int slot = k == 9 ? 0 : k < 9 ? k + 1 : k;
	return PARTS + u32(port * 16 + slot) * PART_STRIDE;
}
constexpr u32 PART_XG_SIZE = 0x29;  // 08 pp 00-28

// パートの塊の中の、XG に番地の無い演奏中の値
constexpr u32 PART_MOD  = 0x7d;     // CC1
constexpr u32 PART_EXP  = 0x7e;     // CC11
constexpr u32 PART_BEND = 0x80;     // ピッチベンドの MSB の半分（0x20 が真ん中）
constexpr u32 PART_HOLD = 0xd9;     // CC64。0 か 1
constexpr u32 PART_COPY = 0xe0;     // 画面へ写す長さ（上の全部を含む）

// エフェクトの塊。xg は XG の番地の先頭、ram はワーク RAM での先頭
struct block { u8 hi, mid, lo; u32 size; u32 ram; };

constexpr block EFFECTS[] = {
	{ 0x02, 0x01, 0x00, 0x14, 0x0cad8 },   // リバーブ
	{ 0x02, 0x01, 0x20, 0x14, 0x0caec },   // コーラス
	{ 0x02, 0x01, 0x40, 0x1c, 0x0cb02 },   // バリエーション
	{ 0x03, 0x00, 0x00, 0x2c, 0x0cb7e },   // インサーション 1
	{ 0x03, 0x01, 0x00, 0x2c, 0x0cbaa },   // インサーション 2
};

// XG の番地から、ワーク RAM での位置。無ければ false
inline bool locate(u32 addr, u32 &off)
{
	const u8 hi = u8(addr >> 14), mid = u8((addr >> 7) & 0x7f), lo = u8(addr & 0x7f);
	if (hi == 0x00 && mid == 0x00 && lo < 7) {
		off = SYSTEM + lo;
		return true;
	}
	if (hi == 0x08 && mid < 32 && lo < PART_XG_SIZE) {
		off = part_base(mid) + lo;
		return true;
	}
	for (const block &b : EFFECTS) {
		if (hi == b.hi && mid == b.mid && lo >= b.lo && lo < b.lo + b.size) {
			off = b.ram + (lo - b.lo);
			return true;
		}
	}
	return false;
}

} // namespace ram
} // namespace xg

#endif // S_MU2000_XG_RAM_H
