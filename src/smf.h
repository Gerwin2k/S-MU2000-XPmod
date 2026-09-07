// license:BSD-3-Clause
//
// 標準 MIDI ファイル（SMF）を「秒 + バイト列」の並びに開く。
// render（ファイルを WAV に）と midisend（実時間で MIDI 出力へ）で共用する。

#ifndef S_MU2000_SMF_H
#define S_MU2000_SMF_H

#pragma once

#include "compat/mamecompat.h"

#include <string>
#include <vector>

namespace smf {

struct event {
	double time;              // 秒
	std::vector<u8> bytes;
};

// format 0/1 に対応。テンポ変化は追う。SMPTE 単位には未対応
bool load(const std::string &path, std::vector<event> &out, std::string &err);

} // namespace smf

#endif // S_MU2000_SMF_H
