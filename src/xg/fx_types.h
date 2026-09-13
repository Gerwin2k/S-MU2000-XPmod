// license:BSD-3-Clause
//
// XG のエフェクトの種別の名前（MSB × 128 + LSB）。エフェクトの面と一覧の窓が使う。

#ifndef S_MU2000_XG_FX_TYPES_H
#define S_MU2000_XG_FX_TYPES_H

#pragma once

#include "compat/mamecompat.h"

#include <cstdio>
#include <string>

namespace xg {

struct fx_type { const char *name; u8 msb, lsb; };

// リバーブに置ける種別
inline constexpr fx_type REV_TYPES[] = {
	{ "NO EFFECT", 0x00, 0x00 }, { "HALL 1", 0x01, 0x00 }, { "HALL 2", 0x01, 0x01 },
	{ "ROOM 1", 0x02, 0x00 },    { "ROOM 2", 0x02, 0x01 }, { "ROOM 3", 0x02, 0x02 },
	{ "STAGE 1", 0x03, 0x00 },   { "STAGE 2", 0x03, 0x01 }, { "PLATE", 0x04, 0x00 },
	{ "WHITE ROOM", 0x10, 0x00 },{ "TUNNEL", 0x11, 0x00 },  { "BASEMENT", 0x13, 0x00 },
};

// コーラスに置ける種別
inline constexpr fx_type CHO_TYPES[] = {
	{ "NO EFFECT", 0x00, 0x00 }, { "CHORUS 1", 0x41, 0x00 }, { "CHORUS 2", 0x41, 0x01 },
	{ "CHORUS 3", 0x41, 0x02 },  { "CELESTE 1", 0x42, 0x00 },{ "CELESTE 2", 0x42, 0x01 },
	{ "CELESTE 3", 0x42, 0x02 }, { "FLANGER 1", 0x43, 0x00 },{ "FLANGER 2", 0x43, 0x01 },
	{ "FLANGER 3", 0x43, 0x02 },
};

// バリエーションとインサーションに置ける種別。
// 27 個ぜんぶ、1 音ずつ鳴らして別々の音になることを確かめてある
inline constexpr fx_type INS_TYPES[] = {
	{ "NO EFFECT", 0x00, 0x00 },   { "HALL 1", 0x01, 0x00 },     { "ROOM 1", 0x02, 0x00 },
	{ "STAGE 1", 0x03, 0x00 },     { "PLATE", 0x04, 0x00 },      { "DELAY LCR", 0x05, 0x00 },
	{ "DELAY L,R", 0x06, 0x00 },   { "ECHO", 0x07, 0x00 },       { "CROSS DELAY", 0x08, 0x00 },
	{ "ER 1", 0x09, 0x00 },        { "GATE REVERB", 0x0b, 0x00 },{ "REVERSE GATE", 0x0c, 0x00 },
	{ "THRU", 0x40, 0x00 },        { "CHORUS 1", 0x41, 0x00 },   { "CELESTE 1", 0x42, 0x00 },
	{ "FLANGER 1", 0x43, 0x00 },   { "SYMPHONIC", 0x44, 0x00 },  { "ROTARY SP", 0x45, 0x00 },
	{ "TREMOLO", 0x46, 0x00 },     { "AUTO PAN", 0x47, 0x00 },   { "PHASER 1", 0x48, 0x00 },
	{ "DISTORTION", 0x49, 0x00 },  { "OVERDRIVE", 0x4a, 0x00 },  { "AMP SIM", 0x4b, 0x00 },
	{ "3BAND EQ", 0x4c, 0x00 },    { "2BAND EQ", 0x4d, 0x00 },   { "AUTO WAH", 0x4e, 0x00 },
};

// 種別の値から名前。表に無ければ「TYPE 49-01」のように番号で
inline std::string fx_name(int value)
{
	const int msb = value >> 7, lsb = value & 0x7f;
	for (const fx_type &t : INS_TYPES) if (t.msb == msb && t.lsb == lsb) return t.name;
	for (const fx_type &t : REV_TYPES) if (t.msb == msb && t.lsb == lsb) return t.name;
	for (const fx_type &t : CHO_TYPES) if (t.msb == msb && t.lsb == lsb) return t.name;
	char buf[16];
	std::snprintf(buf, sizeof(buf), "TYPE %02X-%02X", msb, lsb);
	return buf;
}

} // namespace xg

#endif // S_MU2000_XG_FX_TYPES_H
